"""Compile the production guest read sync gate and check it.

The fixture `tests/guest_read_sync_gate_harness.c` includes the production
header and links the production source with an authored handler. Each mutant
changes one expression and must make the fixture fail."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/ps3emu/guest_read_sync.h'
SOURCE = ROOT / 'runtime/memory/guest_read_sync.c'
FIXTURE = ROOT / 'tests/guest_read_sync_gate_harness.c'

GATE = '    if (!vm_read_sync_any) return;\n'
# Both compiler branches of the flag store.
SET_FLAG = ('    _InterlockedExchange((volatile long *)&vm_read_sync_any,1);\n'
            '#else\n'
            '    __atomic_store_n(&vm_read_sync_any,1,__ATOMIC_RELEASE);\n')

MUTANTS = [
    ('gate inverted: return on a set flag', HEADER, GATE,
     '    if (vm_read_sync_any) return;\n'),
    ('gate removed', HEADER, GATE, ''),
    ('flag not set by mark', SOURCE, SET_FLAG, ''),
    ('loop continues after the dispatch', HEADER,
     'if(vm_read_sync_page(page)) { vm_read_sync_dispatch(address,bytes); return; }',
     'if(vm_read_sync_page(page)) { vm_read_sync_dispatch(address,bytes); }'),
    ('zero bytes read the table', HEADER,
     'if (!bytes || (uint64_t)address+bytes', 'if ((uint64_t)address+bytes'),
]


def compiler():
    found = os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl') or shutil.which('clang') or shutil.which('cc')
    if not found:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    return found


PAGE_READER = 'static inline unsigned char vm_read_sync_page(unsigned index) {'
RENAMED_READER = 'static inline unsigned char real_page_read(unsigned index) {'


def build(run, cc, msvc_style, header_text, source):
    """`header_text` is the production header, with its page reader renamed."""
    if header_text.count(PAGE_READER) != 1:
        raise RuntimeError('Production page reader definition not found once')
    run.mkdir(parents=True, exist_ok=True)
    (run / 'include/ps3emu').mkdir(parents=True, exist_ok=True)
    (run / 'include/ps3emu/guest_read_sync.h').write_bytes(
        header_text.replace(PAGE_READER, RENAMED_READER).encode())
    exe = run / ('check.exe' if os.name == 'nt' else 'check')
    obj = run / ('gate.obj' if os.name == 'nt' else 'gate.o')
    if msvc_style:
        common = [cc, '/nologo', '/O2', '/UNDEBUG', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(ROOT / 'include')]
        commands = [common + ['/c', str(source), '/Fo' + str(obj)],
                    common[:5] + ['/I' + str(run / 'include')] + common[7:] + [str(FIXTURE), str(obj),
                              '/Fe' + str(exe), '/Fo' + str(run) + os.sep]]
    else:
        common = [cc, '-std=c11', '-O2', '-UNDEBUG', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(ROOT / 'include')]
        commands = [common + ['-c', str(source), '-o', str(obj)],
                    common[:5] + ['-I' + str(run / 'include')] + common[6:] + [str(FIXTURE), str(obj), '-o', str(exe)]]
    # The source is compiled with the production header. The fixture is compiled
    # with the copy whose page reader has the wrapper name.
    log = ''
    for command in commands:
        done = subprocess.run(command, capture_output=True, text=True, errors='replace')
        log += done.stdout + done.stderr
        if done.returncode:
            break
    (run / 'build.log').write_text(log)
    if done.returncode:
        print(log)
        return None
    return exe


def run_fixture(exe):
    done = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace')
    return done.returncode, done.stdout + done.stderr


def main():
    cc = compiler()
    msvc_style = Path(cc).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    base = ROOT / 'build/self-tests/guest-read-sync-gate'
    out = base / 'production'
    shutil.rmtree(out, ignore_errors=True)
    exe = build(out, cc, msvc_style, HEADER.read_bytes().decode(), SOURCE)
    if exe is None:
        return 1
    code, text = run_fixture(exe)
    (out / 'run.log').write_text(text)
    print(text.strip())
    if code or 'guest_read_sync_gate: PASS' not in text:
        return 1
    for name, path, old, new in MUTANTS:
        source = path.read_bytes().decode().replace('\r\n', '\n')
        if source.count(old) != 1:
            raise RuntimeError('Mutant target is not unique: ' + name)
        changed = source.replace(old, new)
        out = base / 'mutant'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        if path is HEADER:
            exe = build(out, cc, msvc_style, changed, SOURCE)
        else:
            (out / 'guest_read_sync.c').write_bytes(changed.encode())
            exe = build(out, cc, msvc_style, HEADER.read_bytes().decode(), out / 'guest_read_sync.c')
        if exe is None:
            print('mutant did not compile: ' + name)
            return 1
        code, text = run_fixture(exe)
        (out / 'run.log').write_text(text)
        if code == 0:
            print('mutant accepted: ' + name)
            return 1
        print('mutant rejected: ' + name)
    print('PASS tests/check_guest_read_sync_gate.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
