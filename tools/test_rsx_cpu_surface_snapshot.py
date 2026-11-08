"""Compile the actual CPU-surface snapshot ownership helper and reject mutations."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'temp' / 'checks' / ('cpu-surface-snapshot-' + str(time.time_ns()))
OUT.mkdir(parents=True)
header = ROOT / 'libs/video/rsx_cpu_surface_snapshot.h'
fixture = ROOT / 'libs/video/tests/rsx_cpu_surface_snapshot.c'
compiler = os.environ.get('MV_CLANG') or shutil.which('clang-cl') or shutil.which('clang') or shutil.which('cc')
assert compiler, 'Prepare the compiler environment first'
msvc_style = Path(compiler).name.lower().startswith('clang-cl')
report = {'qualification': __doc__, 'source_sha256': hashlib.sha256(header.read_bytes()).hexdigest(), 'commands': []}
def build_and_run(name, source, success):
    directory = OUT / name; directory.mkdir()
    (directory / header.name).write_text(source)
    exe = directory / ('check.exe' if os.name == 'nt' else 'check')
    if msvc_style: command = [compiler, '/nologo', '/std:c17', '/O2', '/UNDEBUG', '/I'+str(directory), '/I'+str(ROOT/'include'), str(fixture), '/Fe'+str(exe)]
    else: command = [compiler, '-std=c17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-I', directory, '-I', ROOT/'include', fixture, '-o', exe]
    built = subprocess.run(command, text=True, capture_output=True, timeout=90)
    report['commands'].append({'command': list(map(str, command)), 'returncode': built.returncode, 'stdout': built.stdout, 'stderr': built.stderr})
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([exe], text=True, capture_output=True, timeout=90)
    report['commands'].append({'command': [str(exe)], 'returncode': ran.returncode, 'stdout': ran.stdout, 'stderr': ran.stderr})
    (OUT/'result.json').write_text(json.dumps(report, indent=2))
    assert (ran.returncode == 0) == success, (name, ran.stdout, ran.stderr)
source = header.read_text()
build_and_run('production', source, True)
mutants = {
    'alias-admission': source.replace('if (matched != UINT32_MAX)\n                    return 0;', 'if (matched != UINT32_MAX)\n                    break;'),
    'edge-rejection': source.replace('end <= view_end', 'end < view_end'),
    'view-overflow-admission': source.replace(' || view_end > UINT64_C(0x100000000)', ''),
    'mixed-view-admission': source.replace('if (i && matched != bindings[0].view_index)\n            return 0;', 'if (i && matched != bindings[0].view_index)\n            break;'),
}
for name, mutant in mutants.items():
    assert mutant != source
    build_and_run(name, mutant, False)
print('PASS actual CPU-surface snapshot binding and four ownership mutants:', OUT)
