"""Compile authored jobmod header/worker-ownership checks and negative controls."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'runtime/spu/spu_jobmod_identity.h'
FIXTURE = ROOT / 'runtime/spu/tests/test_spu_jobmod_identity.c'
OUT = ROOT / 'build/spu-jobmod-identity-tests'
MUTANTS = {
    'shared-selector': ('static SPU_JOBMOD_THREAD_LOCAL int', 'static int'),
    'shared-log-counter': ('static SPU_JOBMOD_THREAD_LOCAL unsigned', 'static unsigned'),
}


def build_and_run(name, header, compiler):
    folder = OUT / name
    tests = folder / 'tests'
    tests.mkdir(parents=True, exist_ok=True)
    (folder / SOURCE.name).write_text(header, encoding='utf-8')
    shutil.copyfile(FIXTURE, tests / FIXTURE.name)
    executable = tests / ('fixture.exe' if os.name == 'nt' else 'fixture')
    if Path(compiler).stem.lower() in ('cl', 'clang-cl'):
        command = [compiler, '/nologo', '/O2', '/W3', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
                   str(tests / FIXTURE.name), '/Fo' + str(tests) + os.sep, '/Fe' + str(executable)]
    else:
        command = [compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                   str(tests / FIXTURE.name), '-o', str(executable)]
    build = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (folder / 'build.log').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        return {'build': build.returncode, 'output': (build.stdout + build.stderr)[-2000:]}
    run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
    (folder / 'run.log').write_text(run.stdout + run.stderr, encoding='utf-8')
    modes = {}
    for value in (None, '0', '1', '2'):
        environment = dict(os.environ)
        environment.pop('SPU_JOBMOD_HEADER_FNV', None)
        if value is not None:
            environment['SPU_JOBMOD_HEADER_FNV'] = value
        result = subprocess.run([str(executable), 'env'], env=environment,
                                capture_output=True, text=True, timeout=30)
        modes[str(value)] = {'returncode': result.returncode, 'stdout': result.stdout.strip()}
    return {'build': 0, 'returncode': run.returncode, 'output': (run.stdout + run.stderr).strip(), 'modes': modes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CC') or shutil.which('clang-cl' if os.name == 'nt' else 'cc'))
    parser.add_argument('--mutants', action='store_true')
    arguments = parser.parse_args()
    header = SOURCE.read_text(encoding='utf-8')
    result = build_and_run('production', header, arguments.compiler)
    print(result['output'])
    modes = result.get('modes', {})
    passed = result.get('returncode') == 0
    passed &= all(modes.get(value, {}).get('stdout') == expected for value, expected in [('None', '0'), ('0', '0'), ('1', '1')])
    passed &= modes.get('2', {}).get('returncode') not in (None, 0)
    report = {'game_inputs': False, 'production': result, 'mutants': {}}
    if arguments.mutants:
        for name, (before, after) in MUTANTS.items():
            assert header.count(before) == 1, name
            result = build_and_run(name, header.replace(before, after), arguments.compiler)
            rejected = result.get('build') == 0 and result.get('returncode') not in (None, 0)
            report['mutants'][name] = {'rejected': rejected, 'result': result}
            print(f'{name}: {"rejected" if rejected else "FAILED"} {result["output"]}')
            passed &= rejected
    (OUT / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
