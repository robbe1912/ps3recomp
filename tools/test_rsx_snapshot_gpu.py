"""Headless D3D12 producer with actual snapshot service, transport and packing."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--warp', action='store_true', help='Use the real D3D12 WARP software adapter')
args = parser.parse_args()
assert os.name == 'nt', 'D3D12 validation requires Windows'
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'temp/checks' / ('snapshot-gpu-' + str(time.time_ns()))
OUT.mkdir(parents=True)
tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq ms1_recomp.exe', '/FO', 'CSV', '/NH'], text=True)
assert not any(row and row[0].lower() == 'ms1_recomp.exe' for row in csv.reader(tasks.splitlines())), 'Native host is owned'
source = ROOT / 'libs/video/rsx_cpu_surface.inc'
transport = ROOT / 'runtime/memory/guest_read_sync.c'
fixture = ROOT / 'libs/video/tests/rsx_snapshot_gpu.c'
report = dict(qualification=__doc__, adapter='warp' if args.warp else 'hardware',
              fifo_scope='Fixture schedules closed boundaries; actual FIFO protocol has separate controls',
              identities={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [source, transport, fixture, Path(__file__),
                                    ROOT / 'include/ps3emu/guest_read_sync.h',
                                    ROOT / 'libs/video/rsx_cpu_surface_snapshot.h',
                                    ROOT / 'libs/video/rsx_surface_readback.h',
                                    ROOT / 'libs/video/rsx_cpu_surface_profile.h',
                                    ROOT / 'libs/video/rsx_fifo_boundary.h',
                                    ROOT / 'libs/video/rsx_d3d12_fault.h']}, commands=[])

def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120)
    report['commands'].append(dict(command=list(map(str, command)), returncode=result.returncode,
                                   stdout=result.stdout, stderr=result.stderr))
    (OUT / 'result.json').write_text(json.dumps(report, indent=2))
    return result

compiler = os.environ.get('MV_CLANG') or shutil.which('clang-cl')
assert compiler, 'Prepare the Visual Studio compiler environment'

def build(name, body):
    compiled = OUT / (name + '.c')
    compiled.write_text(fixture.read_text().replace('// PRODUCTION_SURFACE', body))
    report.setdefault('compiled_sources', {})[name] = hashlib.sha256(compiled.read_bytes()).hexdigest()
    exe = OUT / (name + '.exe')
    includes = [ROOT / 'include', ROOT / 'libs/video']
    command = [compiler, '/nologo', '/TC', '/std:c17', '/O2', '/MDd', '/UNDEBUG',
               '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in includes],
               str(compiled), str(transport), '/Fo' + str(OUT) + os.sep, '/Fe' + str(exe),
               '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib']
    result = run(command)
    assert result.returncode == 0, (result.stderr, OUT / 'result.json')
    report.setdefault('binaries', {})[name] = hashlib.sha256(exe.read_bytes()).hexdigest()
    return exe

body = source.read_text()
exe = build('production', body)
adapter = 'warp' if args.warp else 'hardware'
result = run([exe, adapter])
assert result.returncode == 0, (result.stderr, OUT / 'result.json')
assert 'zero validation messages' in result.stdout
for mode, error in [(1, 'CPU snapshot before command submission'),
                    (2, '0x887A0005'), (3, 'unsupported guest surface layout')]:
    result = run([exe, adapter, str(mode)])
    assert result.returncode != 0 and error in result.stderr, (mode, OUT / 'result.json')
packing = (ROOT / 'libs/video/rsx_surface_readback.h').read_text()
wrong_packing = packing.replace('d[x]=s[x*4+2];', 'd[x]=s[x*4];')
assert wrong_packing != packing
(OUT / 'wrong-channel.h').write_text(wrong_packing)
report['wrong_channel_header_sha256'] = hashlib.sha256((OUT / 'wrong-channel.h').read_bytes()).hexdigest()
mutants = {
    'skip-dirty-copy': body.replace('    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);', '    return;\n    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);'),
    'stale-clean-generation': body.replace('r->cpu_generation=r->gpu_generation;', 'r->cpu_generation=0;'),
    'wrong-byte-channel': body.replace('#include "rsx_surface_readback.h"', '#include "wrong-channel.h"'),
}
for name, mutated in mutants.items():
    assert mutated != body, name
    result = run([build(name, mutated), adapter])
    assert result.returncode != 0 and 'CHECK ' in result.stderr, ('Mutant accepted', name, OUT / 'result.json')
report['passed'] = True
(OUT / 'result.json').write_text(json.dumps(report, indent=2))
print('PASS actual D3D12 snapshot producer, visible errors and three source mutants;', OUT)
