#!/usr/bin/env python3
"""SPU EA alias page table: synthetic contract and compiled mutants.

Builds runtime/syscalls/tests/test_spu_ea_alias.c against the production
runtime/syscalls/spu_ea_alias.c, which must answer every probe exactly like
the previous two-scan implementation. With --mutants, each mutated copy of
the production file must build and then fail the contract. Concurrent readers
also check completed full/mixed-page registrations, and type assertions reject
non-atomic page/count publication. Four concurrent writers must respect the
internal blocking lock and preserve every registration. No game input; no
performance claim."""
import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/syscalls/spu_ea_alias.c'
FIXTURE = ROOT / 'runtime/syscalls/tests/test_spu_ea_alias.c'
OUT = ROOT / 'build/spu-ea-alias-tests'

MUTANTS = [
    ('no-writer-lock', '    spu_alias_record_lock();', '    if (0) spu_alias_record_lock();'),
    ('tail-gap-as-full', '    return reach >= page_end ? 2 : 1;\n', '    return 2;\n'),
    ('hole-as-full', 'if (start[k] > reach) return 1;', 'if (start[k] > reach) return 2;'),
    ('unsorted-union', 'while (k > 0 && start[k - 1] > a)', 'while (0 && k > 0 && start[k - 1] > a)'),
    ('wrapped-end-covers', 'b = (uint32_t)(s_alias[i].addr + s_alias[i].size);',
     'b = (uint64_t)s_alias[i].addr + s_alias[i].size;'),
    ('image-not-refreshed', '    for (int s = 0; s < 2; s++) {\n', '    for (int s = 0; s < 1; s++) {\n'),
    ('no-refresh-on-record', '        spu_alias_pages_refresh(addr, size);', '        if (0) spu_alias_pages_refresh(addr, size);'),
    ('no-mixed-fallback', '    else state = SPU_ALIAS_PAGE_MIXED;\n', '    else state = SPU_ALIAS_PAGE_IDENTITY;\n'),
    ('self-cover-ignored', 'if (self == 2 || prim == 0)', 'if (prim == 0)'),
]


def build_and_run(name, source_text, compiler):
    # Mirror the tree so the fixture's #include "../spu_ea_alias.c" resolves.
    folder = OUT / name
    tests = folder / 'tests'
    tests.mkdir(parents=True, exist_ok=True)
    (folder / 'spu_ea_alias.c').write_text(source_text, encoding='utf-8')
    shutil.copyfile(FIXTURE, tests / FIXTURE.name)
    exe = tests / ('fixture.exe' if os.name == 'nt' else 'fixture')
    src = str(tests / FIXTURE.name)
    if Path(compiler).stem.lower() in ('clang-cl', 'cl'):
        cmd = [compiler, '/nologo', '/O2', '/W3', '/WX', '/D_CRT_SECURE_NO_WARNINGS', src,
               '/Fo' + str(tests) + os.sep, '/Fe' + str(exe)]
    else:
        cmd = [compiler, '-std=gnu17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', src, '-o', str(exe)]
    b = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    (folder / 'build.log').write_text(' '.join(cmd) + '\n' + b.stdout + b.stderr, encoding='utf-8')
    if b.returncode:
        return dict(build=b.returncode, log=(b.stdout + b.stderr)[-1500:])
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    (folder / 'run.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    return dict(build=0, returncode=r.returncode, output=(r.stdout + r.stderr).strip()[-600:])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', default=os.environ.get('CC') or shutil.which('clang-cl' if os.name == 'nt' else 'cc'))
    p.add_argument('--mutants', action='store_true')
    args = p.parse_args()
    assert args.compiler, 'Set CC or --compiler'
    OUT.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text(encoding='utf-8')
    report = dict(game_inputs=False)
    prod = build_and_run('production', source, args.compiler)
    report['production'] = prod
    print(prod.get('output') or prod.get('log'))
    ok = prod.get('build') == 0 and prod.get('returncode') == 0
    if args.mutants:
        report['mutants'] = {}
        for name, before, after in MUTANTS:
            assert source.count(before) == 1, name
            mutated = source.replace(before, after)
            if name == 'no-writer-lock':
                mutated = mutated.replace('    spu_alias_record_unlock();', '    if (0) spu_alias_record_unlock();')
            rec = build_and_run('mutant-' + name, mutated, args.compiler)
            killed = rec.get('build') == 0 and rec.get('returncode') not in (0, None)
            report['mutants'][name] = dict(killed=killed, record=rec)
            state = 'killed' if killed else 'BUILD FAILED' if rec.get('build') else 'SURVIVED'
            print(f'mutant {name}: {state}' + (f' ({rec["output"].splitlines()[-1]})' if killed else ''))
            ok &= killed
    (OUT / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
