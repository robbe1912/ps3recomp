#!/usr/bin/env python3
"""Bytes of the prepared vertex fetch against the scalar path before it, on captured
draws. Compiled mutants must fail.

Usage:
  python tools/check_vertex_diet.py [--compiler PATH] [--mutants] [--benchmark] [--output DIR]

Fork layout (script in tools/): libs/video/rsx_vertex_fetch.c, fixtures in libs/video/tests/.
Game layout (script in scripts/): vendor/ps3recomp/libs/video/rsx_vertex_fetch.c, fixtures in
scripts/fixtures/. The fixtures are rsx_vertex_fetch_reference.c (the file before the vector
decode, verbatim), test_vertex_diet.c and rsx_vertex_diet_capture.h.

The exit code is 0 only when the vector build and the scalar build pass with the same digest,
and (with --mutants) every compiled mutant fails.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# rsx_vertex_fetch.c before the vector decode, with LF line ends.
REFERENCE_SHA256 = 'b9b06f545ee6481add85b35aa124272e3a9172215b02490af4aa235979539ea6'
# Digest of all output bytes of the passing build (test_vertex_diet.c prints it).
EXPECTED_DIGEST = 'b659d6a9a0b55e1f'

RENAMES = {
    'rsx_vertex_fetch_prepare': 'ref_vertex_fetch_prepare',
    'rsx_fetch_vertex_prepared': 'ref_fetch_vertex_prepared',
    'rsx_fetch_attrib': 'ref_fetch_attrib',
    'rsx_rd_bef': 'ref_rd_bef',
    'rsx_rd_half_be': 'ref_rd_half_be',
}

# (name, old text, new text, what the mutant changes)
MUTANTS = [
    ('half-denormal',
     '    value = _mm_or_si128(_mm_andnot_si128(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()), value), sign);\n',
     '',
     'a denormal half keeps the normal formula instead of a signed zero'),
    ('half-infinity',
     '    value = _mm_add_epi32(value, _mm_and_si128(_mm_cmpeq_epi32(exponent, exponent_mask), bias));',
     '    value = _mm_add_epi32(value, _mm_and_si128(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7C01)), bias));',
     'infinity and NaN keep exponent 143'),
    ('half-byte-order',
     '    h = _mm_shuffle_epi8(h, _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 6, 7, 4, 5, 2, 3, 0, 1));',
     '    h = _mm_shuffle_epi8(h, _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 4, 5, 2, 3, 6, 7));',
     'the halves stay in guest byte order'),
    ('float-byte-order',
     '    return _mm_shuffle_epi8(words, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));',
     '    return words;',
     'the float32 words stay in guest byte order'),
    ('size-mask',
     '    { ~0u, ~0u, 0u, 0u },',
     '    { ~0u, ~0u, ~0u, 0u },',
     'size 2 writes a third component over the register value'),
    ('u8-reciprocal',
     '_mm_div_ps(rsx_vf_u8(p, n), _mm_set1_ps(255.0f))',
     '_mm_mul_ps(rsx_vf_u8(p, n), _mm_set1_ps(1.0f / 255.0f))',
     'the u8 scale multiplies by a reciprocal instead of one division'),
    ('s16-reciprocal',
     '_mm_div_ps(rsx_vf_s16(p, n), _mm_set1_ps(32767.0f))',
     '_mm_mul_ps(rsx_vf_s16(p, n), _mm_set1_ps(1.0f / 32767.0f))',
     'the s16 scale multiplies by a reciprocal instead of one division'),
    ('cmp-z-scale',
     '_mm_set_ps(1.0f, 511.0f, 1023.0f, 1023.0f)',
     '_mm_set_ps(1.0f, 1023.0f, 1023.0f, 1023.0f)',
     'the CMP z scale divides by 1023'),
    ('keep-lane',
     '_mm_andnot_ps(supply, _mm_loadu_ps(keep))',
     '_mm_andnot_ps(supply, _mm_setzero_ps())',
     'a component the array does not supply becomes zero'),
    ('constants-slot',
     '    for (u32 i = 0; i < 16; i++)\n        _mm_storeu_ps(out[i].v, _mm_loadu_ps(constants[i].v));',
     '    for (u32 i = 0; i < 15; i++)\n        _mm_storeu_ps(out[i].v, _mm_loadu_ps(constants[i].v));',
     'slot 15 keeps stale bytes'),
    ('float-page-crossing',
     '            if (rsx_vf_in_page(p, 16)) {',
     '            if (1) {',
     'a float32 load reads past the end of the page'),
    ('half-page-crossing',
     '            if (rsx_vf_in_page(p, 8)) {',
     '            if (1) {',
     'a half load reads past the end of the page'),
    ('frequency-mode',
     '            ei = s->modulo ? (vi % s->frequency) : (vi / s->frequency);',
     '            ei = s->modulo ? (vi / s->frequency) : (vi % s->frequency);',
     'modulo and divide instancing are swapped'),
]


def normalize(data):
    return data.replace(b'\r\n', b'\n')


def layout(script):
    root = script.resolve().parents[1]
    game = root / 'vendor/ps3recomp/libs/video/rsx_vertex_fetch.c'
    if game.is_file():
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        return dict(root=root, fetch=game, include=root / 'vendor/ps3recomp/include',
                    fixtures=root / 'scripts/fixtures', out=root / 'temp/checks' / ('rsx-vertex-diet-' + stamp))
    return dict(root=root, fetch=root / 'libs/video/rsx_vertex_fetch.c', include=root / 'include',
                fixtures=root / 'libs/video/tests', out=root / 'build/vertex-diet-check')


def environment(out):
    env = dict(os.environ)
    if os.name != 'nt' or ('Windows Kits' in env.get('INCLUDE', '') and env.get('LIB')):
        return env
    program_files = env.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = Path(program_files) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                       'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                       '-property', 'installationPath'], text=True).strip()
    task = out / 'vcvars.cmd'
    task.write_text('@echo off\r\ncall "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\n'
                    'if errorlevel 1 exit /b 1\r\nset\r\n' % install, encoding='utf-8')
    output = subprocess.check_output(['cmd', '/d', '/c', str(task)], text=True)
    for line in output.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key] = value
    return env


def default_compiler(env):
    if env.get('MV_CLANG'):
        return env['MV_CLANG']
    found = shutil.which('clang-cl' if os.name == 'nt' else 'cc')
    if found:
        return found
    if os.name == 'nt':
        for install in Path(os.environ.get('ProgramFiles', r'C:\Program Files')).glob(
                'Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-cl.exe'):
            return str(install)
    raise SystemExit('Set --compiler')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--compiler')
    parser.add_argument('--mutants', action='store_true')
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    place = layout(Path(__file__))
    out = (args.output or place['out']).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = environment(out)
    compiler = args.compiler or default_compiler(env)
    msvc = Path(compiler).name.lower() in {'cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'}
    clang = Path(compiler).name.lower().startswith('clang')
    fetch_text = normalize(place['fetch'].read_bytes()).decode('utf-8')
    reference = place['fixtures'] / 'rsx_vertex_fetch_reference.c'
    reference_hash = hashlib.sha256(normalize(reference.read_bytes())).hexdigest()
    if reference_hash != REFERENCE_SHA256:
        raise SystemExit('FAIL the reference copy is not rsx_vertex_fetch.c before the vector decode: ' + reference_hash)
    includes = [place['fetch'].parent, place['include']]
    commands = []

    def run(command, folder):
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True, cwd=folder, env=env)
        (folder / (Path(command[0]).name + '.log')).write_text(
            ' '.join(command) + '\n' + result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            sys.stdout.write(result.stdout + result.stderr)
            raise SystemExit('FAIL build step exited %d: %s' % (result.returncode, command[-1]))
        return result.stdout + result.stderr

    def flags(vector):
        if msvc:
            contract = '/clang:-ffp-contract=off' if clang else '/fp:strict'
            base = ['/nologo', '/TC', '/std:c17', '/O2', '/Ob2', '/MDd', '/Zi', '/UNDEBUG',
                    '-fno-strict-aliasing', contract, '-Wall', '-Wextra', '-Wno-unused-parameter']
        else:
            base = ['-std=c17', '-O2', '-UNDEBUG', '-fno-strict-aliasing', '-ffp-contract=off',
                    '-Wall', '-Wextra', '-Wno-unused-parameter']
        if vector:
            base.append('-msse4.1')
        return base + [('/I' if msvc else '-I') + str(path) for path in includes]

    def compile(name, source, defines=None, vector=True):
        folder = out / name
        folder.mkdir(exist_ok=True)
        obj = folder / (source.stem + '.obj' if msvc else source.stem + '.o')
        base = [flag for flag in flags(vector) if not flag.startswith(('/I', '-I'))]
        for key, value in (defines or {}).items():
            base.append(('/D' if msvc else '-D') + key + '=' + value)
        base += [('/I' if msvc else '-I') + str(path) for path in includes]
        command = [compiler] + base + (['/c', str(source), '/Fo' + str(obj)] if msvc
                                       else ['-c', str(source), '-o', str(obj)])
        run(command, folder)
        return obj

    def link(name, objects):
        folder = out / name
        exe = folder / ('test.exe' if msvc else 'test')
        command = [compiler] + [str(path) for path in objects] + (
            ['/nologo', '/Fe:' + str(exe), '/link', '/OPT:REF'] if msvc else ['-o', str(exe), '-pthread'])
        run(command, folder)
        return exe

    def execute(exe, extra=()):
        result = subprocess.run([str(exe)] + list(extra), capture_output=True, text=True, env=env,
                                cwd=exe.parent, timeout=600)
        return result.returncode, result.stdout + result.stderr

    (out / 'inputs.json').write_text(json.dumps({
        'fetch_sha256_lf': hashlib.sha256(fetch_text.encode('utf-8')).hexdigest(),
        'reference_sha256_lf': reference_hash,
        'fixtures': {name: hashlib.sha256(normalize((place['fixtures'] / name).read_bytes())).hexdigest()
                     for name in ('rsx_vertex_fetch_reference.c', 'test_vertex_diet.c', 'rsx_vertex_diet_capture.h')},
        'compiler': compiler,
    }, indent=2), encoding='utf-8')

    reference_obj = compile('reference', reference, RENAMES)
    harness_obj = compile('harness', place['fixtures'] / 'test_vertex_diet.c')
    results = {'builds': {}, 'mutants': []}
    digests = []
    for name, vector in (('vector', True), ('scalar', False)):
        folder = out / name
        folder.mkdir(exist_ok=True)
        source = folder / 'rsx_vertex_fetch.c'
        source.write_text(fetch_text, encoding='utf-8', newline='\n')
        exe = link(name, [compile(name, source, vector=vector), reference_obj, harness_obj])
        extra = ['benchmark'] if (args.benchmark and vector) else []
        code, output = execute(exe, extra)
        results['builds'][name] = {'exit': code, 'output': output}
        print(output, end='')
        if code != 0 or 'PASS ' not in output:
            raise SystemExit('FAIL the %s build of rsx_vertex_fetch.c did not pass' % name)
        found = re.search(r'digest=([0-9a-f]{16})', output)
        if not found:
            raise SystemExit('FAIL the %s build printed no digest' % name)
        digests.append(found.group(1))
    if digests[0] != digests[1]:
        raise SystemExit('FAIL the vector build and the scalar build print different digests: ' + ' '.join(digests))
    if EXPECTED_DIGEST and digests[0] != EXPECTED_DIGEST:
        raise SystemExit('FAIL digest %s is not %s' % (digests[0], EXPECTED_DIGEST))
    print('PASS vector and scalar builds: same bytes as the scalar path before the change; digest ' + digests[0])

    if args.mutants:
        for name, old, new, what in MUTANTS:
            if fetch_text.count(old) != 1:
                raise SystemExit('FAIL mutant %s: its text is in the file %d times' % (name, fetch_text.count(old)))
            folder = out / ('mutant-' + name)
            folder.mkdir(exist_ok=True)
            source = folder / 'rsx_vertex_fetch.c'
            source.write_text(fetch_text.replace(old, new, 1), encoding='utf-8', newline='\n')
            exe = link('mutant-' + name, [compile('mutant-' + name, source), reference_obj, harness_obj])
            code, output = execute(exe)
            reason = next((line for line in output.splitlines() if line.startswith('FAIL')), None)
            results['mutants'].append({'name': name, 'exit': code, 'reason': reason})
            if code == 0:
                raise SystemExit('FAIL compiled mutant %s passed: %s' % (name, what))
            print('PASS compiled mutant %s rejected (%s): %s' % (name, what, reason or ('exit %d' % code)))

    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
    print('Evidence:', out)


if __name__ == '__main__':
    main()
