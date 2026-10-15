#!/usr/bin/env python3
"""Bounded x64 write watch on an owned Windows process; no target memory writes.

The owner JSON must contain pid, create_time, exe and exe_sha256. Hardware
debug registers are restored before detach. Unrelated exceptions are passed
to the application. Payloads, registers and optional dumps are private output.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time


def filetime_to_unix(value):
    # Subtract the integer epoch before floating-point conversion; otherwise
    # rounding can reject a genuine launch manifest by about a microsecond.
    return (value - 116444736000000000) / 10000000


def process_creation_time(handle):
    """Unix creation time, also used by the standard owned-launch manifests."""
    k = C.WinDLL('kernel32', use_last_error=True)
    k.GetProcessTimes.argtypes = [C.c_void_p] + [C.POINTER(C.c_uint64)] * 4
    times = [C.c_uint64() for _ in range(4)]
    if not k.GetProcessTimes(handle, *[C.byref(t) for t in times]):
        raise C.WinError(C.get_last_error())
    return filetime_to_unix(times[0].value)


def same_creation_time(actual, expected):
    # Independent FILETIME-to-Unix implementations can round to adjacent
    # doubles. Allow one representable step, never a fixed time window.
    return (math.isfinite(actual) and math.isfinite(expected) and actual > 0 and expected > 0
            and abs(actual-expected) <= max(math.ulp(actual), math.ulp(expected)))


class ExceptionRecord(C.Structure):
    _fields_ = [('code', C.c_uint32), ('flags', C.c_uint32),
                ('record', C.c_void_p), ('address', C.c_void_p),
                ('count', C.c_uint32), ('information', C.c_uint64 * 15)]


class ExceptionInfo(C.Structure):
    _fields_ = [('record', ExceptionRecord), ('first', C.c_uint32)]


class EventData(C.Union):
    _fields_ = [('exception', ExceptionInfo), ('words', C.c_uint64 * 20)]


class DebugEvent(C.Structure):
    _fields_ = [('code', C.c_uint32), ('pid', C.c_uint32),
                ('tid', C.c_uint32), ('data', EventData)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--owner', type=Path, required=True)
    ap.add_argument('--address', type=lambda v: int(v, 0), action='append', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--events', type=int, default=256)
    ap.add_argument('--dump-nonzero', type=int, default=0, help='Maximum matching-hit minidumps')
    ap.add_argument('--stop-nonfinite-be-f32', action='store_true',
                    help='Stop and dump the first write with a NaN/infinity in the triggered watched eight bytes, interpreted as two big-endian floats')
    args = ap.parse_args()
    if os.name != 'nt' or C.sizeof(C.c_void_p) != 8:
        raise SystemExit('Requires native 64-bit Windows Python')
    if not 1 <= len(args.address) <= 4 or len(set(args.address)) != len(args.address):
        ap.error('Supply one to four distinct addresses')
    if any(a <= 0 or a % 8 for a in args.address):
        ap.error('Addresses must be positive and eight-byte aligned')
    if not 0 < args.seconds <= 120 or not 1 <= args.events <= 4096 or not 0 <= args.dump_nonzero <= 8:
        ap.error('Invalid bounded duration/event/dump budget')
    if args.stop_nonfinite_be_f32 and args.dump_nonzero:
        ap.error('Use either nonfinite-stop dump or nonzero dumps')
    owner = json.loads(args.owner.read_text())
    exe = Path(owner['exe']).resolve()
    if hashlib.sha256(exe.read_bytes()).hexdigest() != owner['exe_sha256']:
        raise RuntimeError('Owned executable changed')
    k = C.WinDLL('kernel32', use_last_error=True)
    def api(name, result, params):
        f = getattr(k, name); f.restype = result; f.argtypes = params; return f
    handle = C.c_void_p; u32 = C.c_uint32; boolean = C.c_int
    open_process = api('OpenProcess', handle, [u32, boolean, u32])
    open_thread = api('OpenThread', handle, [u32, boolean, u32])
    close = api('CloseHandle', boolean, [handle])
    attach = api('DebugActiveProcess', boolean, [u32])
    break_process = api('DebugBreakProcess', boolean, [handle])
    detach = api('DebugActiveProcessStop', boolean, [u32])
    kill_exit = api('DebugSetProcessKillOnExit', boolean, [boolean])
    wait = api('WaitForDebugEvent', boolean, [C.POINTER(DebugEvent), u32])
    resume_event = api('ContinueDebugEvent', boolean, [u32, u32, u32])
    get_context = api('GetThreadContext', boolean, [handle, handle])
    set_context = api('SetThreadContext', boolean, [handle, handle])
    suspend = api('SuspendThread', u32, [handle])
    resume_thread = api('ResumeThread', u32, [handle])
    exit_thread = api('GetExitCodeThread', boolean, [handle, C.POINTER(u32)])
    wait_handle = api('WaitForSingleObject', u32, [handle, u32])
    read_mem = api('ReadProcessMemory', boolean, [handle, handle, handle, C.c_size_t, C.POINTER(C.c_size_t)])
    create_file = api('CreateFileW', handle, [C.c_wchar_p, u32, u32, handle, u32, u32, handle])
    image_name = api('QueryFullProcessImageNameW', boolean, [handle, u32, C.c_wchar_p, C.POINTER(u32)])
    exit_process = api('GetExitCodeProcess', boolean, [handle, C.POINTER(u32)])
    ph = open_process(0x1fffff, False, owner['pid'])
    if not ph: raise C.WinError(C.get_last_error())
    try:
        name = C.create_unicode_buffer(32768); size = u32(len(name))
        if not image_name(ph, 0, name, C.byref(size)): raise C.WinError(C.get_last_error())
        actual_creation_time = process_creation_time(ph)
        if not same_creation_time(actual_creation_time, owner['create_time']) or Path(name.value).resolve() != exe:
            raise RuntimeError(f'Owned process identity mismatch: creation={actual_creation_time!r}, '
                               f'expected={owner["create_time"]!r}, image={name.value!r}, expected_image={str(exe)!r}')
        args.out.mkdir(parents=True, exist_ok=False)
    except BaseException:
        close(ph); raise
    attached = False; threads = {}; hits = dumps = 0; initial_break = False
    started = time.monotonic(); reason = 'error'; failure = None
    restored = False; cleanup_break = False; attach_break_address = None
    CONTINUE = 0x10002; NOT_HANDLED = 0x80010001
    assert C.sizeof(DebugEvent) == 176 and DebugEvent.data.offset == 16
    log = (args.out / 'events.jsonl').open('x', buffering=1)
    def record(kind, **values):
        log.write(json.dumps(dict(kind=kind, utc=time.time(), elapsed=time.monotonic()-started, **values))+'\n')
    def checked(ok):
        if not ok: raise C.WinError(C.get_last_error())
    def context(th, flags=0x10001f):
        storage = C.create_string_buffer(1232+15)
        address = (C.addressof(storage)+15) & ~15
        struct.pack_into('<I', storage, address-C.addressof(storage)+48, flags)
        checked(get_context(th, address))
        return storage, address
    def debug_regs(ptr):
        return struct.unpack('<6Q', C.string_at(ptr+72, 48))
    def store_debug(th, ptr, regs):
        C.memmove(ptr+48, struct.pack('<I', 0x100010), 4)
        C.memmove(ptr+72, struct.pack('<6Q', *regs), 48)
        checked(set_context(th, ptr))
    def arm(tid):
        if tid in threads: return
        th = open_thread(0x10005a, False, tid)
        if not th: raise C.WinError(C.get_last_error())
        try:
            buf, ptr = context(th); old = debug_regs(ptr)
            if old[5] & 0xff: raise RuntimeError('Existing hardware breakpoint; refusing interference')
            regs = [0, 0, 0, 0, 0, 0]
            for i, a in enumerate(args.address):
                regs[i] = a; regs[5] |= (1 << (2*i)) | (9 << (16+4*i))
            store_debug(th, ptr, regs); threads[tid] = (th, old)
            record('armed', tid=tid)
        except BaseException:
            close(th); raise
    def read(address, size=8):
        b = C.create_string_buffer(size); n = C.c_size_t()
        checked(read_mem(ph, address, b, size, C.byref(n)))
        if n.value != size: raise RuntimeError('Short target read')
        return b.raw
    def dump(index):
        path = args.out / ('hit_%04d_stall.dmp' % index)
        fh = create_file(str(path.resolve()), 0x40000000, 0, None, 1, 0x80, None)
        if fh == C.c_void_p(-1).value: raise C.WinError(C.get_last_error())
        try:
            dbg = C.WinDLL('dbghelp', use_last_error=True)
            dbg.MiniDumpWriteDump.argtypes = [handle, u32, handle, u32, handle, handle, handle]
            checked(dbg.MiniDumpWriteDump(ph, owner['pid'], fh, 0x1000, None, None, None))
        finally: close(fh)
        return dict(path=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    def restore_stopped():
        nonlocal restored
        for tid, (th, old) in threads.items():
            active = u32(); checked(exit_thread(th, C.byref(active)))
            if active.value == 259:
                buf, ptr = context(th); store_debug(th, ptr, old)
        restored = True
    try:
        checked(attach(owner['pid'])); attached = True
        checked(kill_exit(False))
        record('attach', owner=owner, addresses=args.address,
               stop_nonfinite_be_f32=args.stop_nonfinite_be_f32)
        while True:
            if time.monotonic()-started > args.seconds+10:
                raise TimeoutError('No cleanup debug event within bounded grace')
            if time.monotonic()-started >= args.seconds and not cleanup_break:
                checked(break_process(ph)); cleanup_break = True
                record('cleanup_break_requested')
            event = DebugEvent()
            if not wait(C.byref(event), 100):
                error = C.get_last_error()
                if error in (121, 258): continue
                raise C.WinError(error)
            status = CONTINUE; finished = False
            try:
                assert event.pid == owner['pid']
                if event.code in (2, 3):
                    arm(event.tid)
                if event.code in (3, 6) and event.data.words[0]:
                    close(event.data.words[0])  # CREATE_PROCESS/LOAD_DLL hFile
                if event.code == 4:
                    item = threads.pop(event.tid, None)
                    if item: close(item[0])
                if event.code == 5:
                    record('process_exit', code=event.data.words[0] & 0xffffffff)
                    reason = 'process exit'; break
                if event.code == 1:
                    ex = event.data.exception
                    status = NOT_HANDLED
                    if ex.record.code == 0x80000003 and not initial_break:
                        initial_break = True; status = CONTINUE
                        attach_break_address = ex.record.address
                        ready = dict(threads=len(threads), values=[read(a).hex() for a in args.address])
                        (args.out / 'ready.json').write_text(json.dumps(ready, indent=2))
                        record('attach_breakpoint', **ready)
                    elif ex.record.code == 0x80000003 and cleanup_break and ex.record.address == attach_break_address:
                        status = CONTINUE; reason = 'duration limit'; finished = True
                    elif ex.record.code == 0x80000004:
                        th, old = threads[event.tid]; buf, ptr = context(th)
                        regs = list(debug_regs(ptr)); mask = regs[4] & ((1 << len(args.address))-1)
                        if mask:
                            status = CONTINUE; hits += 1
                            values = [read(a).hex() for a in args.address]
                            row = dict(tid=event.tid, index=hits, dr6=hex(regs[4]), mask=mask,
                                       rip=hex(struct.unpack('<Q', C.string_at(ptr+248, 8))[0]),
                                       registers=[hex(x) for x in struct.unpack('<16Q', C.string_at(ptr+120, 128))],
                                       values=values)
                            nonfinite = args.stop_nonfinite_be_f32 and any(
                                (word & 0x7f800000) == 0x7f800000
                                for i, value in enumerate(values) if mask & (1 << i)
                                for word in struct.unpack('>2I', bytes.fromhex(value)))
                            if nonfinite:
                                row['nonfinite_be_f32'] = True
                                row['dump'] = dump(hits); dumps += 1
                            elif dumps < args.dump_nonzero and any(int.from_bytes(bytes.fromhex(v),'little') for i,v in enumerate(values) if mask & (1 << i)):
                                row['dump'] = dump(hits); dumps += 1
                            record('write', **row)
                            regs[4] = 0; store_debug(th, ptr, regs)
                            if nonfinite:
                                reason = 'nonfinite BE float'; finished = True
                            elif hits >= args.events and not cleanup_break:
                                reason = 'event limit'; finished = True
                    if status == NOT_HANDLED:
                        record('exception_passed', code=hex(ex.record.code), first=ex.first, address=hex(ex.record.address or 0))
                if finished: restore_stopped()
            except BaseException:
                # A held debug event suspends all target threads. Restore here,
                # before continuing it, rather than racing a resumed exit.
                restore_stopped()
                raise
            finally:
                checked(resume_event(event.pid, event.tid, status))
            if finished: break
    except BaseException as exc:
        failure = repr(exc); raise
    finally:
        cleanup_errors = []
        if attached and not restored and reason != 'process exit':
            for tid, (th, old) in threads.items():
                active = u32()
                if exit_thread(th, C.byref(active)) and active.value == 259:
                    suspended = False
                    try:
                        if suspend(th) == 0xffffffff:
                            error = C.get_last_error()
                            # A resumed writer may be in thread teardown. Its
                            # signalled handle proves there is no register state
                            # left to restore; a live thread still fails visibly.
                            if wait_handle(th, 1000) == 0:
                                record('thread_exited_during_cleanup', tid=tid)
                                continue
                            raise C.WinError(error)
                        suspended = True; buf, ptr = context(th)
                        store_debug(th, ptr, old)
                    except BaseException as exc: cleanup_errors.append([tid, repr(exc)])
                    finally:
                        if suspended and resume_thread(th) == 0xffffffff:
                            cleanup_errors.append([tid, 'ResumeThread failed'])
        if attached and not detach(owner['pid']):
            error = C.get_last_error(); active = u32()
            if not exit_process(ph, C.byref(active)) or active.value == 259:
                cleanup_errors.append(['detach', error])
        for th, _ in threads.values(): close(th)
        close(ph)
        result = dict(reason=reason, hits=hits, dumps=dumps, failure=failure,
                      cleanup_errors=cleanup_errors, elapsed=time.monotonic()-started)
        (args.out / 'final.json').write_text(json.dumps(result, indent=2))
        log.close(); print(json.dumps(result), flush=True)
        if cleanup_errors: raise RuntimeError('Debugger cleanup failed: '+repr(cleanup_errors))


if __name__ == '__main__': main()
