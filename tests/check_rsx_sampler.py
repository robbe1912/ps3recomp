"""Game-free D3D12 pixel readback for the production guest sampler decoder."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    curated = (ROOT / 'vendor/ps3recomp').is_dir()
    parser.add_argument('--vendor', type=Path, default=ROOT / 'vendor/ps3recomp' if curated else ROOT)
    parser.add_argument('--device', choices=('warp', 'hardware'), default='warp')
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('D3D regression requires Windows')
    env = os.environ.copy()
    if curated:
        from project import build_environment
        env = build_environment()
    compiler = env.get('MV_CLANG') or shutil.which('clang-cl', path=env.get('PATH'))
    if not compiler:
        raise SystemExit('clang-cl and a Windows SDK developer environment are required')
    source = args.vendor.resolve()
    fixture = ROOT / ('scripts/fixtures' if curated else 'tests') / 'rsx_sampler_d3d12_gpu.cpp'
    out = ROOT / 'temp/checks' / ('rsx-sampler-' + datetime.datetime.now(
        datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ'))
    out.mkdir(parents=True)
    exe = out / 'gpu.exe'
    common = [compiler, '/nologo', '/D_CRT_SECURE_NO_WARNINGS',
              '/I' + str(source / 'include'), '/I' + str(source / 'libs/video')]
    commands = [common + ['/std:c++17', '/EHsc', str(fixture),
                          '/Fe' + str(exe), '/link', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib']]
    with (out / 'build.log').open('wb') as log:
        for command in commands:
            built = subprocess.run(command, env=env, cwd=out, stdout=log,
                                   stderr=subprocess.STDOUT, timeout=120)
            if built.returncode:
                print((out / 'build.log').read_text(errors='replace'))
                return built.returncode
    paths = [fixture, source / 'libs/video/rsx_d3d12_sampler.h',
             exe]
    result = dict(game_inputs=False, device=args.device,
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, cases=[])
    for name, arguments, expected in [('unpatched', ['--unpatched'], 1),
                                      (args.device, ['--hardware'] if args.device == 'hardware' else [], 0)]:
        run = subprocess.run([str(exe)] + arguments, capture_output=True, timeout=60)
        output = run.stdout + run.stderr
        (out / (name + '.log')).write_bytes(output)
        print(output.decode(errors='replace').strip())
        result['cases'].append(dict(name=name, returncode=run.returncode, expected=expected))
    (out / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('Evidence:', out)
    return int(any(c['returncode'] != c['expected'] for c in result['cases']))


if __name__ == '__main__':
    raise SystemExit(main())
