"""Compile the production deform painter, mip row, sampler and shadow comparison next to a model of the backend names."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / 'libs/video/rsx_deform_surface.inc'
FIXTURE = ROOT / 'tests/fixtures/rsx_deform_surface.c'
PIXEL = '/* ---- the pixel operation of a paint draw (section B)'
CAPTURE = ' * A. Capture: the inputs of the paint draws.'
BUFFER = ' * B. The CPU buffer of a paint target.'
INIT = '/* Backend init, after cpu_surface_init'
HOOK = '/* Hook of the backend: after cpu_surface_frame_complete()'
CAPTURE_HOOK = '/* Hook of the backend: after the clear record fields are set'
LIMITS = '#ifndef RSX_DEFORM_TARGETS_MAX'
GUEST_INDEX = 'static u32 deform_guest_index(const rsx_state* st, u32 k)'
LINE = re.compile(r'\[DEFORM_SHADOW\] frame=(\d+) target=([0-9A-F]+) texels=(\d+) equal=(\d+) diff1=(\d+) '
                  r'diffmax=(\d+) first=([0-9A-F]+) cpu=(\d+) gpu=(\d+) op_fp16=(\d+) op_fused=(\d+) op_f32=(\d+)'
                  r'(?: dump=(\d+))?')
DONE = '[DEFORM_SHADOW] done target=00A80500 frames=3 texels=304 diff=2 diffmax=7'
WHEEL = '[DEFORM_WHEEL] frame=101 x=10 y=30 level=bilinear cpu=01020304 gpu=05060708'
# (name, production text, changed text). Each changed text must fail the fixture.
MUTANTS = [
    ('pixel-op-keeps-float-brush',
     '    float h = deform_f16((float)((double)brush / 255.0 - 0.25));\n'
     '    float product = (float)((double)h * ((double)a / 255.0));',
     '    float h = (float)((double)brush / 255.0 - 0.25);\n'
     '    float product = (float)((double)h * ((double)a / 255.0));'),
    ('f32-op-bias', '((double)brush / 255.0 - 0.25) * ((double)a / 255.0) + (double)dest / 255.0;',
     '((double)brush / 255.0 - 0.5) * ((double)a / 255.0) + (double)dest / 255.0;'),
    ('viewport-y-not-flipped', 'y = rect[1] + (1.0 - (double)p[1]) * 0.5 * (double)rect[3];',
     'y = rect[1] + ((double)p[1] + 1.0) * 0.5 * (double)rect[3];'),
    ('top-edge-dropped', 'int on_ab = b.y < a.y || (b.y == a.y && b.x > a.x);', 'int on_ab = 0;'),
    ('shared-edge-painted-twice', 'int on_ab = b.y < a.y || (b.y == a.y && b.x > a.x);',
     'int on_ab = 1;'),
    ('mip-row-rounds-down', 'row[x] = (u8)((a[0] + a[1] + b[0] + b[1] + 2) / 4);',
     'row[x] = (u8)((a[0] + a[1] + b[0] + b[1]) / 4);'),
    ('sampler-ignores-mip-row',
     '            else if (at >= mip_begin && at < mip_end) out[count++] = mip[at - mip_begin];\n', ''),
    ('serves-during-update', 'if (!sequence || (sequence & 1) || !deform_surface_holds(s, address, bytes)) continue;',
     'if (!sequence || !deform_surface_holds(s, address, bytes)) continue;'),
    ('brush-nearest-off-by-one', '(int)floorf(u * (float)b->width),',
     '(int)floorf(u * (float)b->width) + 1,'),
    ('brush-bilinear-truncates', '+ 32768u) >> 16;', ') >> 16;'),
    ('painter-writes-past-last-row', '    if (maxy > clip[3]) maxy = clip[3];\n', ''),
    ('holds-adjacent-below', '(u64)address >= begin && (u64)address + bytes <= end)',
     '(u64)address >= begin && (u64)address + bytes < end)'),
    ('shadow-serves-the-gpu-path', 'return deform_on() && s_deform.buffer && !s_deform.shadow;',
     'return deform_on() && s_deform.buffer;'),
    ('buffer-on-without-switch', 's_deform.buffer = deform_switch("RSX_DEFORM_BUFFER");',
     's_deform.buffer = 1;'),
    ('shadow-painter-writes-guest-memory',
     '    return s->ops[0] ? s->ops[s_deform.op] : vm_base + s->guest_address;',
     '    return vm_base + s->guest_address;'),
    ('compare-twice-a-frame',
     '    if (frame == s->compare_frame) return;      /* at most one compare a frame */\n', ''),
    ('compared-columns-stay',
     '    for (u32 y = 0; y < s->height; y++) deform_columns_reset(s, y);\n    s->paint_n = 0;',
     '    s->paint_n = 0;'),
    ('private-copies-start-at-zero',
     '        if (s->ops[k]) memcpy(s->ops[k], vm_base + s->guest_address, bytes);',
     '        if (s->ops[k]) memset(s->ops[k], 0, bytes);'),
    ('private-mip-starts-at-zero',
     '        memcpy(mip, vm_base + s->mip_address, s->mip_width);',
     '        memset(mip, 0, s->mip_width);'),
    ('dump-notes-past-budget',
     '                if (s->paints && s_deform_dumped < s_deform_dump_frames)\n',
     '                if (s->paints)\n'),
    ('first-call-reads-off', '            st = s_deform.any ? 2 : 0;\n', ''),
    ('dump-count-off-by-one',
     '                            (u8)(paints > 255 ? 255 : paints), last ? last->dest : 0,',
     '                            (u8)(paints > 255 ? 255 : paints + 1), last ? last->dest : 0,'),
    ('dump-dest-is-brush',
     '                            (u8)(paints > 255 ? 255 : paints), last ? last->dest : 0,',
     '                            (u8)(paints > 255 ? 255 : paints), last ? last->brush : 0,'),
    ('dump-unbounded',
     '    if (s_deform_dump_file && diff.texels != diff.equal && s_deform_dumped < s_deform_dump_frames) {',
     '    if (s_deform_dump_file && diff.texels != diff.equal) {'),
]


def function(source, start):
    """One production function, from its definition to its closing brace."""
    assert source.count(start) == 1, start
    first = source.index(start)
    depth, end = 1, source.index('{', first) + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[first:end]


def production(source):
    """The production text of the fixture: the pixel operations and the switches,
    then the index decode, the buffer, the comparison, the reads and the batch
    hook. The capture needs the shader decompilers and stays out."""
    for marker in (LIMITS, PIXEL, CAPTURE, BUFFER, INIT, HOOK, CAPTURE_HOOK, GUEST_INDEX):
        assert source.count(marker) == 1, marker
    capture = source.rindex('/*', 0, source.index(CAPTURE))
    buffer = source.rindex('/*', 0, source.index(BUFFER))
    return (source[source.index(PIXEL):capture],
            function(source, GUEST_INDEX) + '\n\n' + source[buffer:source.index(INIT)]
            + source[source.index(HOOK):source.index(CAPTURE_HOOK)])


def main():
    source = INC.read_text(encoding='utf-8')
    out = ROOT / 'build/self-tests/rsx-deform-surface'
    out.mkdir(parents=True, exist_ok=True)
    compiler = (os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl')
                or shutil.which('clang') or shutil.which('cc'))
    if not compiler:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    name = Path(compiler).name.lower()
    msvc = name in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    layout = ROOT / 'libs/video/rsx_texture_layout.c'
    includes = [ROOT / 'libs/video', ROOT / 'include']
    fixture = FIXTURE.read_text(encoding='utf-8')
    limits = source[source.index(LIMITS):source.index(PIXEL)]
    assert fixture.count('/* PRODUCTION_PIXEL */') == 1 and fixture.count('/* PRODUCTION_BUFFER */') == 1

    def run(label, texts):
        """The run of the composed program, or None and the build error."""
        folder = out / label
        folder.mkdir(exist_ok=True)
        c = folder / 'check.c'
        c.write_text(limits + fixture.replace('/* PRODUCTION_PIXEL */', texts[0]).replace('/* PRODUCTION_BUFFER */', texts[1]),
                     encoding='utf-8')
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        if msvc:
            cmd = [compiler, '/nologo', '/O2', '/std:c11']
            if 'clang' in name:
                cmd += ['/experimental:c11atomics', '/clang:-std=c17']
            cmd += ['/I' + str(p) for p in includes]
            cmd += [str(c), str(layout), '/Fo' + str(folder) + os.sep, '/Fe' + str(exe)]
        else:
            cmd = [compiler, '-std=c11', '-O2'] + ['-I' + str(p) for p in includes]
            cmd += [str(c), str(layout), '-o', str(exe)]
        build = subprocess.run(cmd, capture_output=True, text=True)
        (folder / 'build.log').write_text(build.stdout + build.stderr, encoding='utf-8')
        if build.returncode:
            return None, 'build failed: ' + (build.stdout + build.stderr).strip()[-800:]
        done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600,
                              env=dict(os.environ, RSX_DEFORM_DUMP_FILE=str(folder / 'dump.bin')))
        (folder / 'run.log').write_text(done.stdout + done.stderr, encoding='utf-8')
        return done, None

    result, error = run('actual', production(source))
    if error:
        print(error)
        return 1
    print((result.stdout + result.stderr).strip()[-6000:])
    if result.returncode:
        return 1
    for line in ('PASS off:', 'PASS formula:', 'PASS on:', 'PASS shadow:', 'PASS dump:'):
        assert line in result.stdout, line
    assert '[DEFORM] target address=00001000 offset=00A80500: 4096x4096, pitch 4096' in result.stdout
    lines = [m.groups() for m in LINE.finditer(result.stdout)]
    first, second, third = [g for g in lines if g[12] is None]
    frames = [g for g in lines if g[12] is None]
    dumped = [g for g in lines if g[12] is not None]
    assert len(frames) == 3 and len(dumped) == 4, lines
    assert [g[12] for g in dumped] == ['1', '1', '2', '2'] and dumped[1][3:5] == ('100', '0'), dumped
    assert first[:7] == ('101', '00A80500', '104', '102', '1', '7', '0001F00A'), first
    assert first[7:10] == ('0', '1', '102') and first[9:12] == ('102', '102', '102') and first[12] is None, first
    assert second[:7] == ('102', '00A80500', '100', '100', '0', '0', '00000000') and second[12] is None, second
    assert third[:7] == ('104', '00A80500', '100', '100', '0', '0', '00000000') and third[12] is None, third
    print('PASS lines: [DEFORM_SHADOW] frames, totals, [DEFORM_WHEEL], bound target')

    survived = []
    for label, before, after in MUTANTS:
        assert source.count(before) == 1, label
        result, error = run('mutant-' + label, production(source.replace(before, after)))
        if error:
            print('PASS mutant ' + label + ' rejected: ' + error.strip().splitlines()[-1][:200])
        elif result.returncode == 0:
            print('FAIL mutant survived: ' + label)
            survived.append(label)
        else:
            reason = [line for line in result.stderr.splitlines() if line.startswith('FAIL')]
            print('PASS mutant ' + label + ' rejected: ' + (reason[-1] if reason else 'exit %d' % result.returncode))
    assert not survived, 'mutants survived: ' + ', '.join(survived)
    print('PASS: deform pixel operations, painter, mip row, sampler and shadow comparison (%d mutants rejected)'
          % len(MUTANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
