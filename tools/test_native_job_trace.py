"""Compile and execute the real bounded recorder with authored concurrent events."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default=os.environ.get('CC') or
                    (shutil.which('clang-cl') or shutil.which('cl') if os.name == 'nt' else None) or
                    shutil.which('cc'))
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
assert args.compiler, 'Provide a C17 compiler using CC or --compiler'
msvc = Path(args.compiler).stem.lower() in ('cl', 'clang-cl')
out = ROOT / 'build/native-job-trace-tests'
out.mkdir(parents=True, exist_ok=True)
production = ROOT / 'runtime/diag/native_job_trace.c'
fixture = ROOT / 'tests/test_native_job_trace.c'
env = dict(os.environ)

def execute(command, directory, label, timeout=60):
    (directory / (label + '-command.json')).write_text(json.dumps(command, indent=2))
    result = subprocess.run(command, cwd=directory, env=env, capture_output=True,
                            text=True, timeout=timeout)
    (directory / (label + '.log')).write_text(result.stdout + result.stderr)
    return result

def compile_and_run(directory, implementation=None):
    directory.mkdir(parents=True, exist_ok=True)
    exe = directory / ('fixture.exe' if os.name == 'nt' else 'fixture')
    command = [args.compiler, str(fixture)]
    if msvc:
        command += ['/nologo', '/std:c17', '/O2', '/UNDEBUG', '/W4', '/WX',
                    '/I' + str(ROOT / 'include'), '/Fo' + str(directory) + os.sep,
                    '/Fe' + str(exe)]
        if implementation:
            command += ['/DJOB_TRACE_IMPLEMENTATION="' + implementation.as_posix() + '"']
    else:
        command += ['-std=c17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), '-pthread', '-o', str(exe)]
        if implementation:
            command += ['-DJOB_TRACE_IMPLEMENTATION="' + implementation.as_posix() + '"']
    built = execute(command, directory, 'build')
    assert built.returncode == 0, built.stdout + built.stderr
    return execute([str(exe)], directory, 'run', timeout=20)

# Compile the uninstrumented production TU independently of the fixture seam.
obj = out / ('production.obj' if msvc else 'production.o')
command = [args.compiler, str(production)]
command += (['/nologo', '/std:c17', '/O2', '/W4', '/WX', '/c', '/I' + str(ROOT/'include'), '/Fo'+str(obj)]
            if msvc else ['-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', '-c',
                          '-I'+str(ROOT/'include'), '-o', str(obj)])
built = execute(command, out, 'production-build')
assert built.returncode == 0, built.stdout + built.stderr

# The game-side consumer is C++; no C atomics or implementation layout leak.
cpp = out / 'public-header.cpp'
cpp.write_text('#include "ps3emu/native_job_trace.h"\n'
               'ps3_job_trace_event event{};\n'
               'int consumer(ps3_job_trace* trace) { return ps3_job_trace_read(trace, 0, &event); }\n')
command = [args.compiler, str(cpp)]
command += (['/nologo', '/std:c++17', '/W4', '/WX', '/c', '/I'+str(ROOT/'include'), '/Fo'+str(out/'header.obj')]
            if msvc else ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-c',
                          '-I'+str(ROOT/'include'), '-o', str(out/'header.o')])
built = execute(command, out, 'header-build')
assert built.returncode == 0, built.stdout + built.stderr
result = compile_and_run(out)
assert result.returncode == 0, result.stdout + result.stderr
assert '16384 concurrent authored events' in result.stdout
print(result.stdout, end='')

rejected = []
if args.mutants:
    text = production.read_text()
    store = '    atomic_store_explicit(&record->sequence, index + 1, memory_order_release);'
    seam = '    PS3_JOB_TRACE_BEFORE_COPY(trace, index);'
    drop = '        atomic_fetch_add_explicit(&trace->dropped, 1, memory_order_relaxed);'
    full = '    if (index >= trace->capacity) {\n' + drop + '\n        return PS3_JOB_TRACE_FULL;\n    }'
    clock = '    if (!event->tick) record->event.flags |= PS3_JOB_TRACE_CLOCK_INVALID;'
    for needle in (store, seam, drop, full, clock):
        assert text.count(needle) == 1
    overwrite = text.replace(full, '    int overflow = index >= trace->capacity;\n'
                             '    if (overflow) {\n' + drop + '\n        index %= trace->capacity;\n    }')
    overwrite = overwrite.replace('    return PS3_JOB_TRACE_RECORDED;',
                                   '    return overflow ? PS3_JOB_TRACE_FULL : PS3_JOB_TRACE_RECORDED;')
    variants = {
        'premature-publication': text.replace(store, '').replace(seam, store + '\n' + seam),
        'hidden-loss': text.replace(drop, ''),
        'overwrite-records': overwrite,
        'hidden-invalid-clock': text.replace(clock, ''),
    }
    for name, variant in variants.items():
        directory = out / name
        directory.mkdir(parents=True, exist_ok=True)
        implementation = directory / 'implementation.c'
        implementation.write_text(variant)
        result = compile_and_run(directory, implementation)
        assert result.returncode != 0 and 'assert' in (result.stdout + result.stderr).lower(), (
            name + ' must fail a real fixture assertion, not compilation, timeout or infrastructure')
        rejected.append(name)
        print('PASS rejected ' + name + ' by actual recorder fixture assertion')

manifest = {
    'compiler': args.compiler,
    'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (production, fixture, ROOT/'include/ps3emu/native_job_trace.h')},
    'authored_events': 16384, 'rejected_mutants': rejected,
    'scope': 'Recorder storage and publication only; no game binding, scheduler timing, native capture, profiler overhead or frame-time improvement measured.'
}
(out / 'result.json').write_text(json.dumps(manifest, indent=2))
print('PASS actual production recorder; no game payloads or performance claim')
