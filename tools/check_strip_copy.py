"""Check the hit copy of the indexed strip uploader of the D3D12 backend.

The check compiles three pieces into one program: the production function
upload_strip_vp_indexed of rsx_d3d12_backend.c, the same function before the change
(libs/video/tests/upload_strip_vp_indexed_reference.inc, verbatim from commit 4630c6bd) and
the real vertex fetch. On authored draws and on 64 captured indexed triangle strips it
requires, for every vertex that reaches the output buffer: the bytes of the new uploader
equal the bytes of the old uploader and the scalar decode (rsx_fetch_attrib) of the guest
bytes.

Usage: python tools/check_strip_copy.py [--compiler PATH] [--mutants] [--output DIR]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_vertex_diet import default_compiler, environment, layout, normalize

PLACE = layout(Path(__file__).resolve())
ROOT = PLACE['root']
VIDEO = PLACE['fetch'].parent            # libs/video of this tree
INCLUDE = PLACE['include']
FIXTURES = PLACE['fixtures']

# upload_strip_vp_indexed before the change, with LF line ends.
REFERENCE_SHA256 = 'ab2af05fc792a284ee14a4f21137eed68ba3515881b1b9f46d6e90a0af3815af'

COPY = 'for (u32 i = 0; i < 16; i++) out[o*16 + i] = cached_vertices[set][way][i];'

# (name, new text of the copy loop, what the mutant changes)
MUTANTS = [
    ('last-slot', 'for (u32 i = 0; i < 15; i++) out[o*16 + i] = cached_vertices[set][way][i];',
     'the sixteenth float4 of the vertex keeps its old bytes'),
    ('first-slot', 'for (u32 i = 1; i < 16; i++) out[o*16 + i] = cached_vertices[set][way][i];',
     'the first float4 of the vertex keeps its old bytes'),
    ('every-other', 'for (u32 i = 0; i < 16; i += 2) out[o*16 + i] = cached_vertices[set][way][i];',
     'every second float4 of the vertex keeps its old bytes'),
    ('other-way', 'for (u32 i = 0; i < 16; i++) out[o*16 + i] = cached_vertices[set][way ^ 1u][i];',
     'the copy reads the other cache way'),
    ('other-set', 'for (u32 i = 0; i < 16; i++) out[o*16 + i] = cached_vertices[set ^ 1u][way][i];',
     'the copy reads another cache set'),
    ('next-vertex', 'for (u32 i = 0; i < 16; i++) out[(o + 1u)*16 + i] = cached_vertices[set][way][i];',
     'the copy writes the slot of the next vertex'),
    ('reversed', 'for (u32 i = 0; i < 16; i++) out[o*16 + i] = cached_vertices[set][way][15 - i];',
     'the float4 of the vertex come out in reverse order'),
    ('low-half', 'for (u32 i = 0; i < 16; i++) out[o*16 + i] = cached_vertices[set][way][i & 7u];',
     'the upper eight float4 repeat the lower eight'),
    ('first-component', 'for (u32 i = 0; i < 16; i++) out[o*16 + i].v[0] = cached_vertices[set][way][i].v[0];',
     'only the first component of each float4 is copied'),
]


def profile_and_fetch(text):
    """The vertex profile text and read_vp_vertex of a backend file."""
    profile_start = text.index('/* RSX_VERTEX_PROFILE=1:')
    fetch_start = text.index('static void read_vp_vertex(', profile_start)
    fetch_end = text.index('\n/* VTX_POS=', fetch_start)
    return text[profile_start:fetch_end]


def uploader(text):
    """upload_strip_vp_indexed of a backend file, up to the next function."""
    start = text.index('static u32 upload_strip_vp_indexed(')
    return text[start:text.index('\nstatic ', start + 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--compiler')
    parser.add_argument('--mutants', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    out = (args.output or PLACE['out'].with_name(PLACE['out'].name.replace('rsx-vertex-diet', 'rsx-strip-copy'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = environment(out)
    compiler = args.compiler or default_compiler(env)
    msvc = Path(compiler).name.lower() in {'cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'}
    clang = Path(compiler).name.lower().startswith('clang')

    fetch_text = profile_and_fetch(normalize((VIDEO / 'rsx_d3d12_backend.c').read_bytes()).decode('utf-8'))
    function = uploader(normalize((VIDEO / 'rsx_d3d12_backend.c').read_bytes()).decode('utf-8'))
    reference = normalize((FIXTURES / 'upload_strip_vp_indexed_reference.inc').read_bytes()).decode('utf-8')
    reference_hash = hashlib.sha256(reference.encode('utf-8')).hexdigest()
    if reference_hash != REFERENCE_SHA256:
        raise SystemExit('FAIL the reference is not upload_strip_vp_indexed of commit 4630c6bd: ' + reference_hash)
    if reference.count('upload_strip_vp_indexed(') != 1:
        raise SystemExit('FAIL the reference does not hold one definition of upload_strip_vp_indexed')
    reference = reference.replace('upload_strip_vp_indexed(', 'upload_strip_vp_indexed_reference(')
    if function.count(COPY) != 1:
        raise SystemExit('FAIL the inline copy is not in upload_strip_vp_indexed exactly once')
    template = normalize((FIXTURES / 'test_strip_copy.c.in').read_bytes()).decode('utf-8')
    for name in ('@FETCH_VERTEX_FUNCTION@', '@INDEXED_STRIP_FUNCTION@', '@INDEXED_STRIP_REFERENCE@'):
        if template.count(name) != 1:
            raise SystemExit('FAIL %s is not in the template exactly once' % name)
    includes = [FIXTURES, INCLUDE, VIDEO]
    if msvc:
        flags = ['/nologo', '/TC', '/std:c17', '/O2', '/Ob2', '/MDd', '/Zi', '/UNDEBUG', '-fno-strict-aliasing',
                 '/clang:-ffp-contract=off' if clang else '/fp:strict', '-Wall', '-Wextra', '-Wno-unused-parameter',
                 '-msse4.1']
    else:
        flags = ['-std=c17', '-O2', '-UNDEBUG', '-fno-strict-aliasing', '-ffp-contract=off', '-Wall', '-Wextra',
                 '-Wno-unused-parameter', '-msse4.1']
    flags += [('/I' if msvc else '-I') + str(path) for path in includes]
    commands = []
    results = {}

    def build(name, text):
        folder = out / name
        folder.mkdir(exist_ok=True)
        source = folder / 'check.c'
        source.write_text(template.replace('@FETCH_VERTEX_FUNCTION@', fetch_text)
                          .replace('@INDEXED_STRIP_FUNCTION@', text)
                          .replace('@INDEXED_STRIP_REFERENCE@', reference), encoding='utf-8', newline='\n')
        exe = folder / ('check.exe' if msvc else 'check')
        command = [compiler] + flags + [str(source), str(VIDEO / 'rsx_vertex_fetch.c')]
        command += (['/Fe:' + str(exe), '/Fo' + str(folder) + os.sep, '/link', '/OPT:REF'] if msvc
                    else ['-o', str(exe)])
        commands.append(command)
        built = subprocess.run(command, capture_output=True, text=True, cwd=folder, env=env, timeout=600)
        (folder / (Path(compiler).name + '.log')).write_text(
            ' '.join(command) + '\n' + built.stdout + built.stderr, encoding='utf-8')
        if built.returncode:
            sys.stdout.write(built.stdout[-4000:] + built.stderr[-4000:])
            raise SystemExit('FAIL build step exited %d: %s' % (built.returncode, name))
        return exe

    def execute(exe):
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
        return ran.returncode, ran.stdout + ran.stderr

    code, output = execute(build('production', function))
    print(output.strip())
    results['production'] = dict(exit=code, passed=code == 0 and 'PASS ' in output)
    if not results['production']['passed']:
        raise SystemExit('FAIL the uploader with the inline copy did not pass')
    if args.mutants:
        for name, new, what in MUTANTS:
            code, output = execute(build('mutant-' + name, function.replace(COPY, new)))
            results['mutant ' + name] = dict(exit=code, rejected=code != 0 or 'PASS ' not in output)
            print(('REJECTED ' if results['mutant ' + name]['rejected'] else 'FAIL ') + 'mutant %s: %s' % (name, what))
            if not results['mutant ' + name]['rejected']:
                raise SystemExit('FAIL mutant %s passed' % name)
    inputs = [VIDEO / name for name in ('rsx_d3d12_backend.c', 'rsx_vertex_fetch.c', 'rsx_vertex_fetch.h',
                                        'rsx_commands.h', 'rsx_index_strip.h', 'rsx_upload_buffer.h')]
    inputs += [FIXTURES / name for name in ('upload_strip_vp_indexed_reference.inc', 'test_strip_copy.c.in',
                                            'rsx_vertex_diet_capture.h')]
    inputs.append(Path(__file__).resolve())
    (out / 'inputs.json').write_text(json.dumps(
        {str(path): hashlib.sha256(normalize(path.read_bytes())).hexdigest() for path in inputs}, indent=2),
        encoding='utf-8')
    (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('Evidence:', out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
