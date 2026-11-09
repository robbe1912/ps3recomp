"""Headless D3D12 producer with actual snapshot service, transport and packing."""
import argparse
import csv
import hashlib
import json
import os
import re
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
backend = ROOT / 'libs/video/rsx_d3d12_backend.c'
backend_source = backend.read_text()
submission_functions = []
for name in ('wait_for_gpu_required', 'record_batch_retire', 'submit_record_capacity'):
    match = re.search(r'^static void ' + name + r'\([^\n]*\n\{.*?^\}', backend_source, re.M | re.S)
    if match:
        submission_functions.append(match.group())
submission = '\n'.join(submission_functions)
assert 'static void submit_record_capacity(' in submission
transport = ROOT / 'runtime/memory/guest_read_sync.c'
fixture = ROOT / 'libs/video/tests/rsx_snapshot_gpu.c'
report = dict(qualification=__doc__, adapter='warp' if args.warp else 'hardware',
              fifo_scope='Fixture schedules closed boundaries; actual FIFO protocol has separate controls',
              identities={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [source, backend, transport, fixture, Path(__file__),
                                    ROOT / 'include/ps3emu/guest_read_sync.h',
                                    ROOT / 'libs/video/rsx_cpu_surface_snapshot.h',
                                    ROOT / 'libs/video/rsx_surface_readback.h',
                                    ROOT / 'libs/video/rsx_cpu_surface_profile.h',
                                    ROOT / 'libs/video/rsx_fifo_boundary.h',
                                    ROOT / 'libs/video/rsx_d3d12_fault.h']}, commands=[])

def run(command, profile=False):
    env = dict(os.environ, RSX_CPU_SURFACE_PROFILE='1' if profile else '0')
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=120)
    report['commands'].append(dict(command=list(map(str, command)), returncode=result.returncode,
                                   stdout=result.stdout, stderr=result.stderr))
    (OUT / 'result.json').write_text(json.dumps(report, indent=2))
    return result

compiler = os.environ.get('MV_CLANG') or shutil.which('clang-cl')
assert compiler, 'Prepare the Visual Studio compiler environment'

def build(name, body, record_submission=submission):
    compiled = OUT / (name + '.c')
    compiled.write_text(fixture.read_text().replace('// PRODUCTION_SURFACE', body).replace('// PRODUCTION_SUBMIT_RECORDS', record_submission))
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
def verify_trace(stderr):
    for client, renderer in re.findall(r'^\[CPUSNAPSHOT_STATE\].*client=(\d+) renderer=(\d+)', stderr, re.M):
        assert int(client) and int(client) != int(renderer), 'Snapshot requester identity missing'
    states = re.findall(r'^\[CPUSNAPSHOT_STATE\] seq=(\d+) .*$', stderr, re.M)
    completed = re.findall(r'^\[CPUSNAPSHOT_RESULT\] seq=(\d+) .*$', stderr, re.M)
    assert len(states) == 64 and sorted(states) == sorted(completed), (len(states), len(completed), OUT / 'result.json')
    assert stderr.count('[CPUSNAPSHOT_CAP] limit=64') == 1
    assert 'state=0 open=0 draws=0' in stderr
    assert 'state=1 open=1 draws=0' in stderr
    assert 'state=1 open=1 draws=1' in stderr
    assert '[CPUSNAPSHOT_RANGE]' in stderr and '[CPUSNAPSHOT_VIEW]' in stderr
    assert '[CPUSNAPSHOT_RANGE] seq=1 index=0 address=00001000 bytes=2 output=0' in stderr
    assert '[CPUSNAPSHOT_VIEW] seq=1 slot=0 address=00001000 pitch=8 height=3 format=9 gpu=1 cpu=0' in stderr
    assert '[CPUSNAPSHOT_VIEW] seq=2 slot=0 address=00001000 pitch=8 height=3 format=9 gpu=1 cpu=1' in stderr
    for sequence in (3, 4, 5, 8):
        assert f'[CPUSNAPSHOT_RESULT] seq={sequence} status=0 ' in stderr

result = run([exe, adapter])
assert result.returncode == 0, (result.stderr, OUT / 'result.json')
assert 'zero validation messages' in result.stdout
assert '[CPUSNAPSHOT_' not in result.stderr
profiled = run([exe, adapter], profile=True)
assert profiled.returncode == 0, (profiled.stderr, OUT / 'result.json')
verify_trace(profiled.stderr)
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
    'missing-requester-identity': body.replace('    request.client_thread=GetCurrentThreadId();', '    request.client_thread=0;', 1),
    'missing-record-submission': body.replace('        submit_record_capacity(NULL, 0);', '        ; /* omitted submission */', 1),
    'open-primitive-submission': body.replace('!s_d3d.current_rsx_state->in_begin_end && s_d3d.draw_count', 's_d3d.draw_count', 1),
    'missing-trace': body.replace('            cpu_surface_snapshot_trace(request);', '', 1),
    'wrong-view-pitch': body.replace('target->cpu_address,target->cpu_pitch,target->h,target->cpu_format,', 'target->cpu_address,target->cpu_pitch+1,target->h,target->cpu_format,', 1),
    'repeated-cap': body.replace('sequence>64)return;', 'sequence==UINT32_MAX)return;', 1),
    'skip-dirty-copy': body.replace('    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);', '    return;\n    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);'),
    'stale-clean-generation': body.replace('r->cpu_generation=r->gpu_generation;', 'r->cpu_generation=0;'),
    'wrong-byte-channel': body.replace('#include "rsx_surface_readback.h"', '#include "wrong-channel.h"'),
}
for name, mutated in mutants.items():
    assert mutated != body, name
    is_trace = name in ('missing-trace', 'wrong-view-pitch', 'repeated-cap', 'missing-requester-identity')
    result = run([build(name, mutated), adapter], profile=is_trace)
    if is_trace:
        assert result.returncode == 0, (name, result.stderr)
        try:
            verify_trace(result.stderr)
        except AssertionError:
            pass
        else:
            raise AssertionError(('Trace mutant accepted', name, OUT / 'result.json'))
    else:
        assert result.returncode != 0 and 'CHECK ' in result.stderr, ('Mutant accepted', name, OUT / 'result.json')
presenting = submission.replace('s_present_this_frame = 0;', 's_present_this_frame = 1;', 1)
assert presenting != submission
result = run([build('presenting-submission', body, presenting), adapter])
assert result.returncode != 0 and 'CHECK ' in result.stderr, ('Presenting submission accepted', OUT / 'result.json')
report['rejected_mutants'] = [*mutants, 'presenting-submission']
report['passed'] = True
(OUT / 'result.json').write_text(json.dumps(report, indent=2))
print('PASS actual D3D12 snapshot producer, nonpresenting record submission, visible errors, bounded trace and ten source mutants;', OUT)
