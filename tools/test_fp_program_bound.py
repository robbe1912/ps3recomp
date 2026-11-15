"""Exercise D3D12's actual fragment bound with authored long programs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='clang-cl' if os.name == 'nt' else 'cc')
    args = parser.parse_args()
    video = ROOT / 'libs/video'
    backend = (video / 'rsx_d3d12_backend.c').read_text(encoding='utf-8')
    consumers = re.findall(
        r'rsx_fp_(?:program_size|code_hash|decompile|extract_consts)\([^,]+,\s*(\w+)', backend)
    assert len(consumers) == 5, consumers
    assert len(set(consumers)) == 1, consumers
    selected = consumers[0]
    definition = re.search(r'^#define ' + selected + r' (\d+)u?$', backend, re.M)
    bound = int(definition[1]) if definition else int(selected)
    out = ROOT / 'build/fp-program-bound-tests'
    out.mkdir(parents=True, exist_ok=True)
    executable = out / ('fixture.exe' if os.name == 'nt' else 'fixture')
    fixture = video / 'tests/test_fp_program_bound.c'
    decompiler = video / 'rsx_fp_decompiler.c'
    compiler = shutil.which(args.compiler) or args.compiler
    includes = [video, ROOT / 'include']
    command = [compiler]
    if os.name == 'nt':
        command += ['/nologo', '/std:c17', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
                    f'/DFP_PROGRAM_BOUND={bound}', *('/I' + str(p) for p in includes),
                    str(fixture), str(decompiler), '/Fe' + str(executable),
                    '/Fo' + str(out) + os.sep, '/link', 'd3dcompiler.lib']
    else:
        command += ['-std=c17', '-O2', '-D_POSIX_C_SOURCE=200809L',
                    f'-DFP_PROGRAM_BOUND={bound}', *('-I' + str(p) for p in includes),
                    str(fixture), str(decompiler), '-lm', '-o', str(executable)]
    report = dict(bound=bound, consumers=consumers, commands=[], game_inputs=False,
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                  backend_sha256=hashlib.sha256((video / 'rsx_d3d12_backend.c').read_bytes()).hexdigest())
    for label, invocation in [('build', command), ('production', [str(executable)]),
                               ('old-4k-control', [str(executable), '4096'])]:
        result = subprocess.run(invocation, capture_output=True, text=True, timeout=180)
        (out / (label + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        report['commands'].append(dict(label=label, command=invocation, returncode=result.returncode))
        (out / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        if label == 'old-4k-control':
            assert result.returncode and 'rsx_fp_program_size(program, bound) == length' in result.stderr
        else:
            assert result.returncode == 0, result.stdout + result.stderr
        print(label, result.returncode)
    print('PASS actual backend bound, long programs, and old 4 KiB control')


if __name__ == '__main__':
    main()
