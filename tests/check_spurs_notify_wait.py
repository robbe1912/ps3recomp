"""Check the wait of the SPURS_KERNEL_NOTIFY coordinator.

Compiles the production wait header (libs/spurs/spurs_notify_wait.h) with an
authored harness and asserts the unit conversion of the clock, the period,
that the clock has no system-tick step, that the timer ends a wait at a
2 ms deadline below the tick, that a wake from another thread ends a long
wait, and that a new arm replaces a pending signal. Checks the wiring of the
wait in libs/spurs/cellSpurs.c. Rejects one mutant per rule. Then runs
tools/test_spurs_scheduler.py: the real coordinator in notification mode, with
a faked performance counter for the deadline.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'libs/spurs/spurs_notify_wait.h'
SOURCE = ROOT / 'libs/spurs/cellSpurs.c'
HARNESS = ROOT / 'tests/spurs_notify_wait_harness.c'
OUT = ROOT / 'build/self-tests/spurs-notify-wait'
PREFIX = '[SPURS_NOTIFY_WAIT] '
TICK = 78000            # half of a 15.6 ms system tick, in 100 ns
CONVERSIONS = [
    ((1, 1), 10000000),
    ((10000000, 10000000), 10000000),
    ((3000000000, 3000000000), 10000000),
    ((12345678901234567, 1000000000), 123456789012345),
    ((3579500, 3579500), 10000000),
    ((0, 0), 0),
    (((1 << 62) + 12345, 1 << 40), 41943040000000),
]
# Each text must occur in libs/spurs/cellSpurs.c exactly count times.
WIRING = [
    ('#include "spurs_notify_wait.h"', 1),
    ('HANDLE poll_timer;', 1),
    ('const int notify = spurs_kernel_notify() && si->poll_timer != NULL;', 1),
    ('uint64_t next_poll = notify ? spurs_kernel_now() + SPURS_KERNEL_POLL_100NS : 0;', 1),
    ('            uint64_t now = spurs_kernel_now();', 1),
    ('                spurs_poll_arm(si->poll_timer, next_poll);\n'
     '                ReleaseSRWLockExclusive(&si->lock);\n'
     '                WaitForSingleObject(si->poll_timer, INFINITE);\n'
     '                AcquireSRWLockExclusive(&si->lock);\n'
     '                now = spurs_kernel_now();\n', 1),
    ('            timed = now >= next_poll;\n'
     '            if (timed) next_poll = now + SPURS_KERNEL_POLL_100NS;\n', 1),
    ('si->poll_timer = spurs_kernel_notify() ? spurs_poll_timer_create() : NULL;', 1),
    ('spurs_poll_wake(si->poll_timer);', 3),
    ('spurs_poll_timer_close(si);', 2),
    ('    timer = si->poll_timer; si->poll_timer = NULL;\n'
     '    ReleaseSRWLockExclusive(&si->lock);\n'
     '    if (timer) CloseHandle(timer);\n', 1),
    ('CloseHandle(si->poll_timer)', 0),
    ('GetTickCount64', 0),
    ('CONDITION_VARIABLE kick;', 0),
    ('SleepConditionVariableSRW(&si->kick', 0),
]
HEADER_MUTANTS = [
    # name, text in the header, replacement
    ('tick-clock',
     '    QueryPerformanceCounter(&counter);\n'
     '    QueryPerformanceFrequency(&frequency);\n'
     '    return spurs_kernel_100ns((uint64_t)counter.QuadPart, (uint64_t)frequency.QuadPart);\n',
     '    return GetTickCount64() * 10000u;\n'),
    ('rate-in-milliseconds',
     'counter / frequency * 10000000u + counter % frequency * 10000000u / frequency',
     'counter / frequency * 1000u + counter % frequency * 1000u / frequency'),
    ('counter-times-rate',
     'counter / frequency * 10000000u + counter % frequency * 10000000u / frequency',
     'counter * 10000000u / frequency'),
    ('no-rate-guard', '    if (!frequency)\n        return 0;\n', ''),
    ('period-in-milliseconds', '((uint64_t)SPURS_KERNEL_POLL_MS * 10000u)',
     '((uint64_t)SPURS_KERNEL_POLL_MS * 1000u)'),
    ('arm-absolute-time', 'due.QuadPart = deadline > now ? -(LONGLONG)(deadline - now) : -1;',
     'due.QuadPart = deadline > now ? (LONGLONG)(deadline - now) : -1;'),
    ('wake-arms-one-second', '    due.QuadPart = -1;\n    SetWaitableTimer',
     '    due.QuadPart = -10000000;\n    SetWaitableTimer'),
]
SOURCE_MUTANTS = [
    # name, text in libs/spurs/cellSpurs.c, replacement
    ('tick-clock-at-start', 'spurs_kernel_now() + SPURS_KERNEL_POLL_100NS',
     'GetTickCount64() + SPURS_KERNEL_POLL_MS'),
    ('tick-clock-after-wait', '                now = spurs_kernel_now();',
     '                now = GetTickCount64() * 10000u;'),
    ('clock-not-read-after-wait', '                now = spurs_kernel_now();\n', ''),
    ('period-in-ticks', 'next_poll = now + SPURS_KERNEL_POLL_100NS;', 'next_poll = now + SPURS_KERNEL_POLL_MS;'),
    ('wait-holds-lock',
     '                ReleaseSRWLockExclusive(&si->lock);\n'
     '                WaitForSingleObject(si->poll_timer, INFINITE);\n'
     '                AcquireSRWLockExclusive(&si->lock);\n',
     '                WaitForSingleObject(si->poll_timer, INFINITE);\n'),
    ('kick-without-wake', '    si->kicks++;\n    spurs_poll_wake(si->poll_timer);', '    si->kicks++;'),
    ('stop-without-wake',
     'si->stopping = 1; WakeAllConditionVariable(&si->wake); spurs_poll_wake(si->poll_timer);\n'
     '    ReleaseSRWLockExclusive(&si->lock);\n    if (si->coordinator)',
     'si->stopping = 1; WakeAllConditionVariable(&si->wake);\n'
     '    ReleaseSRWLockExclusive(&si->lock);\n    if (si->coordinator)'),
    ('timer-not-created', 'si->poll_timer = spurs_kernel_notify() ? spurs_poll_timer_create() : NULL;',
     'si->poll_timer = NULL;'),
    ('timer-handle-not-cleared', '    timer = si->poll_timer; si->poll_timer = NULL;\n',
     '    timer = si->poll_timer;\n'),
    ('wait-without-timer', 'spurs_kernel_notify() && si->poll_timer != NULL', 'spurs_kernel_notify()'),
]


def compiler_command(compiler, harness, out, exe):
    msvc_style = Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    if msvc_style:
        return [compiler, '/nologo', '/O2', '/clang:-Wno-everything', str(harness),
                '/Fo' + str(out) + os.sep, '/Fe' + str(exe)]
    return [compiler, '-std=c11', '-O2', str(harness), '-o', str(exe)]


def build(out, harness):
    out.mkdir(parents=True, exist_ok=True)
    compiler = (os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl')
                or shutil.which('clang') or shutil.which('cc'))
    if not compiler:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    exe = out / ('harness.exe' if os.name == 'nt' else 'harness')
    result = subprocess.run(compiler_command(compiler, harness, out, exe),
                            capture_output=True, text=True)
    (out / 'build.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe


def run(exe):
    """Return the harness result and its records as (kind, fields) pairs."""
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    records = []
    for line in result.stdout.splitlines():
        if line.startswith(PREFIX):
            kind, _, fields = line[len(PREFIX):].partition(' ')
            records.append((kind, dict(re.findall(r'(\w+)=(\w+)', fields))))
    return result, records


def of_kind(records, kind):
    return [fields for name, fields in records if name == kind]


def verify(exe):
    """Raise AssertionError when the harness at exe breaks one rule."""
    result, records = run(exe)
    assert result.returncode == 0 and not result.stderr, (result.returncode, result.stderr[-2000:])
    converted = of_kind(records, 'convert')
    assert len(converted) == len(CONVERSIONS), records
    for fields, ((counter, frequency), units) in zip(converted, CONVERSIONS):
        assert (int(fields['counter']), int(fields['frequency']), int(fields['units'])) == (counter, frequency, units), fields
    period, = of_kind(records, 'period')
    assert int(period['units']) == int(period['ms']) * 10000, period

    # The clock has no system-tick step.
    busy = of_kind(records, 'busy')
    assert len(busy) == 8, records
    assert all(fields['after_before'] == '1' for fields in busy), busy
    elapsed = sorted(int(fields['elapsed']) for fields in busy)
    assert elapsed[0] >= 15000, elapsed
    assert sum(value < TICK for value in elapsed) >= 4, elapsed

    # The timer ends a wait at its deadline, below the tick.
    timer, = of_kind(records, 'timer')
    assert timer['created'] == '1', timer
    deadline = of_kind(records, 'deadline')
    assert len(deadline) == 6 and all(fields['wait'] == '0' for fields in deadline), deadline
    elapsed = sorted(int(fields['elapsed']) for fields in deadline)
    assert 20000 <= elapsed[0] < TICK, elapsed

    # A wake from another thread ends a long wait.
    wake, = of_kind(records, 'wake')
    assert wake['wait'] == '0' and 15000 <= int(wake['elapsed']) < 2500000, wake
    thread, = of_kind(records, 'thread')
    assert thread['created'] == '1', thread

    # A new arm replaces a pending signal.
    signaled = of_kind(records, 'signaled')
    rearmed = of_kind(records, 'rearmed')
    assert len(signaled) == len(rearmed) == 3, records
    assert all(fields['wait'] == '0' for fields in signaled + rearmed), records
    # The shortest sample shows the rule; load on the host lengthens the others.
    assert min(int(fields['elapsed']) for fields in signaled) < TICK, signaled
    assert 20000 <= min(int(fields['elapsed']) for fields in rearmed) < TICK, rearmed


def verify_wiring(source):
    """Raise AssertionError when the coordinator wait in source is not wired."""
    for text, count in WIRING:
        assert source.count(text) == count, (count, source.count(text), text[:70])


def scheduler():
    """The real coordinator: notification admission, prompt stop, deadline."""
    command = [sys.executable, str(ROOT / 'tools/test_spurs_scheduler.py'), '--output', str(OUT / 'scheduler')]
    if os.environ.get('CC'):
        command += ['--compiler', os.environ['CC']]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=900)
    (OUT / 'scheduler.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-3000:]
    for line in ('SPURS notify: publication admission / returned-lane admission / prompt stop PASS',
                 'SPURS elapsed deadline admits fallback under continuous kicks PASS'):
        assert line in result.stdout, line


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    header = HEADER.read_text()
    source = SOURCE.read_text()
    verify_wiring(source)
    verify(build(OUT / 'production', HARNESS))
    print('PASS: clock units, period, no tick step, deadline wait, wake, re-arm, coordinator wiring')

    for name, before, after in HEADER_MUTANTS:
        assert header.count(before) == 1, (name, before)
        # The harness includes the header relative to tests/, and the header
        # includes the platform shim relative to libs/spurs/.
        mutant = OUT / 'mutant' / name
        for part in ('tests', 'libs/spurs', 'runtime/platform'):
            (mutant / part).mkdir(parents=True, exist_ok=True)
        shutil.copy(HARNESS, mutant / 'tests' / HARNESS.name)
        (mutant / 'libs/spurs' / HEADER.name).write_text(header.replace(before, after))
        shutil.copy(ROOT / 'runtime/platform/win32_compat.h', mutant / 'runtime/platform')
        try:
            verify(build(mutant, mutant / 'tests' / HARNESS.name))
        except AssertionError:
            pass
        else:
            raise AssertionError('mutant accepted: ' + name)

    for name, before, after in SOURCE_MUTANTS:
        assert source.count(before) == 1, (name, before)
        try:
            verify_wiring(source.replace(before, after))
        except AssertionError:
            pass
        else:
            raise AssertionError('mutant accepted: ' + name)

    scheduler()
    print('PASS: %d header mutants and %d source mutants rejected, scheduler fixture passed'
          % (len(HEADER_MUTANTS), len(SOURCE_MUTANTS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
