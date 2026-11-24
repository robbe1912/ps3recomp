"""Check the SPURS lane count override.

Compiles the production header (libs/spurs/spurs_lane_count.h) with an
authored harness and asserts the effective lane count for each kind of
SPURS_LANES value. Checks the wiring in libs/spurs/cellSpurs.c: the override
comes after the validity check of the value of the game, before the instance
gets its count, and the log line shows both counts. Rejects one mutant per
rule.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'libs/spurs/spurs_lane_count.h'
SOURCE = ROOT / 'libs/spurs/cellSpurs.c'
HARNESS = ROOT / 'tests/spurs_lane_count_harness.c'
OUT = ROOT / 'build/self-tests/spurs-lane-count'

# (setting, requested) -> effective count. None = no setting.
CASES = [
    ((None, 5), 5),
    (('', 5), 5),
    (('6', 5), 6),
    (('4', 5), 4),
    (('1', 5), 1),
    (('06', 5), 6),
    (('6', 2), 6),
    (('0', 5), 5),
    (('7', 5), 5),
    (('12', 5), 5),
    (('x', 5), 5),
    (('5x', 5), 5),
    (('-1', 5), 5),
    (('+6', 5), 5),
    (('6 ', 5), 5),
    (('4294967297', 5), 5),
    (('99999999999999999999', 5), 5),
]
# Each text must occur in libs/spurs/cellSpurs.c exactly count times.
WIRING = [
    ('#include "spurs_lane_count.h"', 1),
    ('nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));', 1),
    ('getenv("SPURS_LANES")', 1),
    ('    if (!nspus || nspus > 6) return CELL_SPURS_CORE_ERROR_INVAL;\n', 1),
    ('    const uint32_t requested_lanes = nspus;\n'
     '    nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));\n', 1),
    ('    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;\n', 1),
    ('nSpus=%u requested=%u', 1),
    ('si->prefix, spurs_ea, si->nspus, requested_lanes);', 1),
]
HEADER_MUTANTS = [
    # (name, old, new)
    ('accept-zero', 'return n >= 1u && n <= SPURS_LANES_MAX ? n : requested;',
     'return n <= SPURS_LANES_MAX ? n : requested;'),
    ('accept-seven', '#define SPURS_LANES_MAX 6u', '#define SPURS_LANES_MAX 7u'),
    ('ignore-setting', 'if (!setting || !*setting) return requested;', 'return requested;'),
    ('no-guard-in-loop', "if (*p < '0' || *p > '9' || n > SPURS_LANES_MAX) return requested;",
     "if (*p < '0' || *p > '9') return requested;"),
    ('digit-off-by-one', "n = n * 10u + (uint32_t)(*p - '0');", "n = n * 10u + (uint32_t)(*p - '0' + 1);"),
    ('no-final-clamp', 'return n >= 1u && n <= SPURS_LANES_MAX ? n : requested;', 'return n ? n : requested;'),
]
SOURCE_MUTANTS = [
    ('include-lost', '#include "spurs_lane_count.h"   /* SPURS_LANES: optional lane count override */\n', ''),
    ('override-lost', 'nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));\n', ''),
    ('override-before-validity-check',
     '    if (!nspus || nspus > 6) return CELL_SPURS_CORE_ERROR_INVAL;\n',
     '    nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));\n'
     '    if (!nspus || nspus > 6) return CELL_SPURS_CORE_ERROR_INVAL;\n'),
    ('count-written-first',
     '    nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));\n'
     '    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;\n',
     '    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;\n'
     '    nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));\n'),
    ('log-without-requested-count', 'nSpus=%u requested=%u', 'nSpus=%u'),
    ('log-prints-count-twice', 'si->prefix, spurs_ea, si->nspus, requested_lanes);',
     'si->prefix, spurs_ea, si->nspus, si->nspus);'),
]


def compiler_command(compiler, include_dir, harness, exe):
    msvc_style = Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    if msvc_style:
        return [compiler, '/nologo', '/O2', '/clang:-Wno-everything', '/I' + str(include_dir), str(harness),
                '/Fe' + str(exe)]
    return [compiler, '-std=c11', '-O2', '-Wno-everything', '-I' + str(include_dir), str(harness), '-o', str(exe)]


def build(header, harness, out, tag):
    out.mkdir(parents=True, exist_ok=True)
    compiler = (os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl')
                or shutil.which('clang') or shutil.which('cc'))
    if not compiler:
        print('FAIL: no C compiler; set CC')
        return None, None
    include_dir = out / (tag.replace('/', '_') + '-include')
    include_dir.mkdir(exist_ok=True)
    shutil.copyfile(header, include_dir / header.name)
    exe = include_dir / ('lanes' + '.exe' if os.name == 'nt' else '')
    result = subprocess.run(compiler_command(compiler, include_dir, harness, exe), capture_output=True, text=True,
                            timeout=180)
    if result.returncode:
        print(result.stdout + result.stderr)
        return None, None
    return exe, compiler


def count(exe, setting, requested):
    env = dict(os.environ)
    result = subprocess.run([str(exe), '-' if setting is None else setting, str(requested)], capture_output=True,
                            text=True, timeout=30, env=env)
    assert result.returncode == 0, result.stderr
    return int(result.stdout.strip())


def verify(exe):
    for (setting, requested), expected in CASES:
        got = count(exe, setting, requested)
        assert got == expected, f'{setting!r} {requested}: {got}, expected {expected}'


def verify_wiring(text):
    lines = text.split('\n')
    for needle, times in WIRING:
        found = text.count(needle)
        assert found == times, f'{needle!r}: {found} occurrences, expected {times}'
    invalid = lines.index('    if (!nspus || nspus > 6) return CELL_SPURS_CORE_ERROR_INVAL;')
    override = lines.index('    nspus = spurs_lane_count(nspus, getenv("SPURS_LANES"));')
    stored = lines.index('    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;')
    assert invalid < override < stored, (invalid, override, stored)
    assert stored - override == 1, stored - override


def main():
    harness = OUT / 'spurs_lane_count_harness.c'
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HARNESS, harness)
    header_text = HEADER.read_text(encoding='utf-8')
    source_text = SOURCE.read_text(encoding='utf-8')

    exe, compiler = build(HEADER, harness, OUT, 'production')
    if not exe:
        return 1
    verify(exe)

    for name, old, new in HEADER_MUTANTS:
        assert header_text.count(old) == 1, name
        mutant = OUT / 'mutant' / name / HEADER.name
        mutant.parent.mkdir(parents=True, exist_ok=True)
        mutant.write_text(header_text.replace(old, new), encoding='utf-8')
        exe_m, _ = build(mutant, harness, OUT, 'mutant/' + name)
        if not exe_m:
            continue
        try:
            verify(exe_m)
        except AssertionError:
            continue
        print(f'FAIL: header mutant survived: {name}')
        return 1

    verify_wiring(source_text)
    for name, old, new in SOURCE_MUTANTS:
        assert source_text.count(old) == 1, name
        changed = source_text.replace(old, new)
        try:
            verify_wiring(changed)
        except AssertionError:
            continue
        print(f'FAIL: source mutant survived: {name}')
        return 1
    print(f'PASS: {len(CASES)} lane count cases, {len(HEADER_MUTANTS)} header mutants, '
          f'{len(SOURCE_MUTANTS)} source mutants rejected (compiler {Path(compiler).name})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
