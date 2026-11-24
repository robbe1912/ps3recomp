"""Check the SPURS_WAIT_PRIORITY admission order.

Compiles the production header (libs/spurs/spurs_wait_priority.h) with an
authored harness and asserts that the switch accepts the value 1 only, that a
registered callback gives the waited job list only with the switch, that a new
registration replaces the old one and that a null function removes it, that
the workload of a job list is the first workload of the instance, that the
scan of a free lane tests each workload once with the wanted workload first
and the others in the order of the cursor, and that the counters print at most
one line per second and only with the switch. Checks the wiring of the pass in
libs/spurs/cellSpurs.c. Rejects one mutant per rule. Then compiles
libs/spurs/cellSpurs.c syntax-only with tools/test_spurs_scheduler.py --syntax.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'libs/spurs/spurs_wait_priority.h'
SOURCE = ROOT / 'libs/spurs/cellSpurs.c'
HARNESS = ROOT / 'tests/spurs_wait_priority_harness.c'
OUT = ROOT / 'build/self-tests/spurs-wait-priority'
PREFIX = '[wait_priority] '
SWITCH = 'SPURS_WAIT_PRIORITY'
LINE = '[SPURS_WAIT_PRIORITY] '
OFF_VALUES = [None, '0', 'true', '11', '1 ', ' 1', '01', 'on']
ON_VALUE = '1'
LINES = [LINE + 'waits=2 boosted_jobs=1', LINE + 'waits=3 boosted_jobs=1', LINE + 'waits=3 boosted_jobs=1']
# Each text must occur in libs/spurs/cellSpurs.c exactly count times.
WIRING = [
    ('#include "spurs_wait_priority.h"', 1),
    # The wanted workload is a workload of the instance that runs the pass.
    ('    return s_workloads[wid].in_use && s_workloads[wid].spurs_ea == (u32)(uintptr_t)ctx\n'
     '        ? s_workloads[wid].data : 0;\n', 1),
    ('        const int wanted = spurs_wait_priority_workload(spurs_wait_priority_job_list(),\n'
     '                                                        spurs_wait_priority_data, (void *)(uintptr_t)ea);\n', 1),
    # One wait for each free lane that the pass gave while a PPU thread waited.
    ('            if (wanted >= 0) spurs_wait_priority_wait();\n', 1),
    ('            for (u32 checked = 0; checked < spurs_wait_priority_count(wanted); checked++) {\n'
     '                int at = spurs_wait_priority_at(wanted, &si->cursor, checked);\n'
     '                if (at < 0) continue;   /* the wanted workload had its position */\n'
     '                u32 wid = (u32)at;\n', 1),
    ('                if ((int)wid == wanted) spurs_wait_priority_job();\n'
     '                slot->busy = 1;\n', 1),
    # The line prints outside the registry lock, after each pass.
    ('        ReleaseSRWLockExclusive(&s_workload_registry_lock);\n'
     '        spurs_wait_priority_report(GetTickCount());\n', 1),
    ('checked < 16; checked++', 0),
    ('u32 wid = si->cursor;', 0),
]
HEADER_MUTANTS = [
    # name, text in the header, replacement
    ('switch-any-value', "v = e && e[0] == '1' && !e[1];", 'v = e != NULL;'),
    ('switch-prefix', "v = e && e[0] == '1' && !e[1];", "v = e && e[0] == '1';"),
    ('switch-always-off', "v = e && e[0] == '1' && !e[1];", 'v = 0;'),
    ('callback-without-switch', '    if (!spurs_wait_priority_on()) return 0;\n', ''),
    ('callback-ignored', '    return fn ? fn(__atomic_load_n(&s_wait_priority_ctx, __ATOMIC_ACQUIRE)) : 0;\n',
     '    return fn ? 0 : 0;\n'),
    ('callback-without-context', 'fn(__atomic_load_n(&s_wait_priority_ctx, __ATOMIC_ACQUIRE))', 'fn(0)'),
    ('registration-not-stored',
     '    __atomic_store_n(&s_wait_priority_fn, fn, __ATOMIC_RELEASE);\n}\n',
     '    (void)fn; (void)ctx;\n}\n'),
    ('callback-without-context-dropped',
     '    __atomic_store_n(&s_wait_priority_fn, fn, __ATOMIC_RELEASE);\n}\n',
     '    if (ctx) __atomic_store_n(&s_wait_priority_fn, fn, __ATOMIC_RELEASE);\n}\n'),
    ('workload-last-match',
     '    if (!list) return -1;\n'
     '    for (unsigned wid = 0; wid < SPURS_WAIT_PRIORITY_WORKLOADS; wid++)\n'
     '        if (data(ctx, wid) == list) return (int)wid;\n'
     '    return -1;\n',
     '    int found = -1;\n'
     '    if (!list) return found;\n'
     '    for (unsigned wid = 0; wid < SPURS_WAIT_PRIORITY_WORKLOADS; wid++)\n'
     '        if (data(ctx, wid) == list) found = (int)wid;\n'
     '    return found;\n'),
    ('workload-misses-last', 'wid < SPURS_WAIT_PRIORITY_WORKLOADS; wid++)',
     'wid + 1 < SPURS_WAIT_PRIORITY_WORKLOADS; wid++)'),
    ('workload-without-list-guard', '    if (!list) return -1;\n', ''),
    ('scan-without-wanted-position', '    return SPURS_WAIT_PRIORITY_WORKLOADS + (wanted >= 0);\n',
     '    return SPURS_WAIT_PRIORITY_WORKLOADS;\n'),
    ('scan-one-position-too-many', '    return SPURS_WAIT_PRIORITY_WORKLOADS + (wanted >= 0);\n',
     '    return SPURS_WAIT_PRIORITY_WORKLOADS + 1;\n'),
    ('wanted-not-first', '    if (wanted >= 0 && !checked) return wanted;\n',
     '    if (wanted >= 0 && checked == 1) return wanted;\n'),
    ('hit-position-not-empty', '    return (int)wid == wanted ? -1 : (int)wid;\n',
     '    return (int)wid;\n'),
    ('cursor-not-advanced',
     '    unsigned wid = *cursor;\n'
     '    *cursor = (wid + 1) & (SPURS_WAIT_PRIORITY_WORKLOADS - 1u);\n',
     '    unsigned wid = *cursor;\n'),
    ('report-each-pass', 'now_ms - s_wait_priority_last_line < SPURS_WAIT_PRIORITY_LINE_MS', '0'),
    ('report-without-switch', 'if (!spurs_wait_priority_on() || now_ms', 'if (now_ms'),
    ('report-one-line-per-two-seconds', '#define SPURS_WAIT_PRIORITY_LINE_MS 1000',
     '#define SPURS_WAIT_PRIORITY_LINE_MS 2000'),
    ('waits-not-counted', '    s_wait_priority_waits++;\n', ''),
    ('jobs-not-counted', '    s_wait_priority_jobs++;\n', ''),
    ('report-counts-swapped',
     '(unsigned long long)s_wait_priority_waits, (unsigned long long)s_wait_priority_jobs);',
     '(unsigned long long)s_wait_priority_jobs, (unsigned long long)s_wait_priority_waits);'),
]
SOURCE_MUTANTS = [
    # name, text in libs/spurs/cellSpurs.c, replacement
    ('scan-without-wanted-position', 'checked < spurs_wait_priority_count(wanted)', 'checked < 16'),
    ('cursor-order-only',
     '                int at = spurs_wait_priority_at(wanted, &si->cursor, checked);\n'
     '                if (at < 0) continue;   /* the wanted workload had its position */\n'
     '                u32 wid = (u32)at;\n',
     '                u32 wid = si->cursor; si->cursor = (wid + 1) & 15;\n'),
    ('empty-position-not-skipped',
     '                if (at < 0) continue;   /* the wanted workload had its position */\n', ''),
    ('wanted-from-any-instance',
     '    return s_workloads[wid].in_use && s_workloads[wid].spurs_ea == (u32)(uintptr_t)ctx\n'
     '        ? s_workloads[wid].data : 0;\n',
     '    return s_workloads[wid].data;\n'),
    ('wanted-of-no-thread', 'const int wanted = spurs_wait_priority_workload(spurs_wait_priority_job_list(),',
     'const int wanted = spurs_wait_priority_workload(0,'),
    ('waits-not-counted', '            if (wanted >= 0) spurs_wait_priority_wait();\n', ''),
    ('jobs-not-counted', '                if ((int)wid == wanted) spurs_wait_priority_job();\n', ''),
    ('report-missing', '        spurs_wait_priority_report(GetTickCount());\n', ''),
    ('report-under-registry-lock',
     '        ReleaseSRWLockExclusive(&s_workload_registry_lock);\n'
     '        spurs_wait_priority_report(GetTickCount());\n',
     '        spurs_wait_priority_report(GetTickCount());\n'
     '        ReleaseSRWLockExclusive(&s_workload_registry_lock);\n'),
    ('header-not-included', '#include "spurs_wait_priority.h"', '#include "spurs_wait_priority_off.h"'),
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


def run(exe, value):
    """Return the harness result for one value of the switch, None for unset."""
    environment = {key: value for key, value in os.environ.items() if key != SWITCH}
    if value is not None:
        environment[SWITCH] = value
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=environment)
    records = {}
    for line in result.stdout.splitlines():
        if not line.startswith(PREFIX):
            continue
        fields = dict(re.findall(r'(\w+)=(-?\w+)', line[len(PREFIX):]))
        kind = line[len(PREFIX):].split()[0].split('=')[0]
        if kind == 'scan':
            # One record for each wanted workload: key "scan<wanted>".
            records['scan' + fields['wanted']] = fields
        else:
            records[kind] = fields
    lines = [line for line in result.stderr.splitlines() if line.startswith(LINE)]
    return result, records, lines


def verify_common(records):
    """Raise AssertionError when a rule of the part without the switch breaks."""
    expected = {
        'found': {'found': '8'},
        'other_instance': {'other_instance': '-1'},
        'first_of_two': {'first_of_two': '3'},
        'last': {'last': '15'},
        'missing': {'missing': '-1'},
        'no_list': {'no_list': '-1'},
        'count': {'count': '16', 'extra': '17'},
    }
    for key, fields in expected.items():
        assert records[key] == fields, (key, records.get(key))
    # The scan of one free lane, for no wanted workload and for each workload.
    for wanted in range(-1, 16):
        scan = {'wanted': str(wanted), 'first': str(wanted if wanted >= 0 else 3), 'count': '16',
                'all': '1', 'order': '1', 'cursor': '3'}
        assert records['scan' + str(wanted)] == scan, (wanted, records.get('scan' + str(wanted)))


def verify(exe):
    """Raise AssertionError when the harness at exe breaks one rule."""
    for value in OFF_VALUES:
        result, records, lines = run(exe, value)
        assert result.returncode == 0 and not result.stderr, (value, result.returncode, result.stderr[-2000:])
        assert records['on'] == {'on': '0'}, (value, records['on'])
        # The callback is not called and gives no job list without the switch.
        assert records['none'] == {'none': '0'} and records['list'] == {'list': '0', 'calls': '0'}, (value, records)
        assert all(records[key] == {key: '0'} for key in
                   ('zero', 'replaced', 'removed', 'removed_without_context', 'null_context')), (value, records)
        assert not lines, (value, lines)
        verify_common(records)
    result, records, lines = run(exe, ON_VALUE)
    assert result.returncode == 0, (result.returncode, result.stderr[-2000:])
    assert records['on'] == {'on': '1'}, records
    assert records['none'] == {'none': '0'}, records
    assert records['list'] == {'list': '4004c580', 'calls': '1'}, records
    assert records['zero'] == {'zero': '0'}, records
    assert records['replaced'] == {'replaced': '40048500'}, records
    assert records['removed'] == {'removed': '0'} and records['removed_without_context'] == {'removed_without_context': '0'}, records
    assert records['null_context'] == {'null_context': '40044480'}, records
    verify_common(records)
    assert lines == LINES, lines
    assert [line for line in result.stderr.splitlines() if not line.startswith(LINE)] == [], result.stderr


def verify_wiring(source):
    """Raise AssertionError when the pass in source is not wired."""
    for text, count in WIRING:
        assert source.count(text) == count, (count, source.count(text), text[:70])


def syntax():
    """The production translation unit compiles."""
    command = [sys.executable, str(ROOT / 'tools/test_spurs_scheduler.py'), '--syntax',
               '--output', str(OUT / 'syntax')]
    if os.environ.get('CC'):
        command += ['--compiler', os.environ['CC']]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=900)
    (OUT / 'syntax.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-3000:]


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    header = HEADER.read_text()
    source = SOURCE.read_text()
    verify_wiring(source)
    verify(build(OUT / 'production', HARNESS))
    print('PASS: switch, callback, workload of a job list, scan order, counters, coordinator wiring')

    for name, before, after in HEADER_MUTANTS:
        assert header.count(before) == 1, (name, before)
        # The harness includes the header relative to tests/.
        mutant = OUT / 'mutant' / name
        for part in ('tests', 'libs/spurs'):
            (mutant / part).mkdir(parents=True, exist_ok=True)
        shutil.copy(HARNESS, mutant / 'tests' / HARNESS.name)
        (mutant / 'libs/spurs' / HEADER.name).write_text(header.replace(before, after))
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

    syntax()
    print('PASS: %d header mutants and %d source mutants rejected, cellSpurs.c compiles'
          % (len(HEADER_MUTANTS), len(SOURCE_MUTANTS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
