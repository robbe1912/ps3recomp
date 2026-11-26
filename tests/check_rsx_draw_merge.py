#!/usr/bin/env python3
"""Draw merge check: the VP pass with RSX_DRAW_MERGE=1 against the same pass
without it.

1. Compiles the production VP pass, the draw merge include, the record layout
   and the PSO key helpers of rsx_d3d12_backend.c with an authored D3D12
   command list, PSO store and record generator (rsx_draw_merge_fixture.c.in).
   The generator writes batches with state changes, clears, records of the
   legacy pass, missing targets, depth reads, other topologies, vertex gaps
   and reduced viewports.
2. For each batch, the commands with the merge must give each guest draw the
   state and the vertex bytes it has without the merge. Python drops the
   commands that set the current value and compares the two line lists. The
   draw count with the merge must equal the runs of the model of the
   generator, so a lost merge is a failure too. The [RSXPHASE] line must hold
   the same numbers.
3. RSX_DRAW_MERGE unset, 0, or 1 with VP_PROBE gives the commands without the
   merge.
4. Mutants of the backend and of the include must fail.
No game input; no performance claim.

Usage: python tests/check_rsx_draw_merge.py [--cc clang-cl] [--batches N]
       [--stop-file PATH]
--stop-file: the check ends with exit code 3 when the file appears."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = 'libs/video/rsx_d3d12_backend.c'
INCLUDE = 'libs/video/rsx_d3d12_draw_merge.inc'
FIXTURE = 'tests/rsx_draw_merge_fixture.c.in'
MAX_DRAWS = 48
HELPERS = ('dr_num_rts', 'tex3d_on', 'tex_unit_volume', 'dr_cube_mask', 'dr_alpha_key', 'dr_unnorm',
           'dr_rt_dxgi', 'vp_select_draw_pso', 'subvp_note')
DEFINE = re.compile(r'(?m)^#define (?:PHASE_DRAW_\w+|VP_CB_STRIDE|VP_FPCB_STRIDE|VP_FPCB_MAX_CONSTS|'
                    r'VP_VERT_STRIDE|RSX_DSK_STENCIL_EN) [^\n]*')
LOOP_START = '    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {'
REPORT = '[RSXPHASE] tid=4242 draw merge: frames=1 runs=%d draws=%d -> %d'
SETTERS = ('rootsig', 'pso', 'topo', 'vb', 'heaps', 'rt', 'vp', 'sc', 'stencil', 'cbv0', 'cbv2')
SUSPICIOUS = re.compile(r'\A(?:drawbad|drawgap|drawpso|cbvbad|vbbad|barrierbad|rtbad|vpbad|scbad|psofull|'
                        r'keyfull)\b| partial$')
ON = {'RSX_DRAW_MERGE': '1'}
PROBE = {'RSX_DRAW_MERGE': '1', 'VP_PROBE': '1'}

MUTANTS = (
    ('lost vertex', INCLUDE,
     'count += s_d3d.draws[t].vertex_count;',
     'count += s_d3d.draws[t].vertex_count - 3u;'),
    ('changed order: the last record of a run draws again', BACKEND,
     'd += merge_n - 1;   /* RSX_DRAW_MERGE: the joined records need no visit */',
     'd += merge_n > 1 ? merge_n - 2 : 0;'),
    ('strip joined without restart', INCLUDE,
     'if (!t->is_vp || t->is_clear || t->topology != RSX_TOPOLOGY_TRIANGLES ||\n        !t->vertex_count',
     'if (!t->is_vp || t->is_clear ||\n        !t->vertex_count'),
    ('vertex gap joined', INCLUDE,
     '!t->vertex_count || t->vb_byte_offset != run->vb_end)',
     '!t->vertex_count)'),
    ('run across a stencil change', INCLUDE,
     't->vp_h != h->vp_h || t->stencil_ref != h->stencil_ref)',
     't->vp_h != h->vp_h)'),
    ('run across a target change', INCLUDE,
     'if (t->rt_off != h->rt_off || t->rt_mrt[0]',
     'if (t->rt_mrt[0]'),
    ('run across a depth read', INCLUDE,
     'if (t->depth_sample_mask || t->depth_alias_mask || t->fp_addr != h->fp_addr)',
     'if (t->depth_alias_mask || t->fp_addr != h->fp_addr)'),
    ('run across a pipeline change', INCLUDE,
     'if (pso != run->pso || sampler_mask',
     'if (sampler_mask'),
    ('run across an SRV change', INCLUDE,
     '    if (s_draw_srv_base[index] != s_draw_srv_base[run->head_index]) return 0;\n', ''),
    ('run across a sampler change', INCLUDE,
     'memcmp(&t->sampler[u], &h->sampler[u], sizeof h->sampler[u])) return 0;',
     '0) return 0;'),
    ('run across a b0 change', INCLUDE,
     'if (memcmp(b0h, b0t, PHASE_DRAW_B0_BYTES)) return 0;',
     'if (b0h == b0t) return 0;'),
    ('run across a b1 change', INCLUDE,
     'if (memcmp(b1h, b1t, bytes)) return 0;',
     'if (b1h == b1t) return 0;'),
    ('clear inside a run', INCLUDE,
     'if (!draw_merge_tail(&run, t)) break;',
     'if (s_d3d.draws[t].is_clear) continue;\n        if (!draw_merge_tail(&run, t)) break;'),
    ('changed primitive type: a record of the legacy pass joins', INCLUDE,
     'if (!t->is_vp || t->is_clear ||',
     'if (t->is_clear ||'),
    ('vertex range rule broken: no record joins', INCLUDE,
     '(u64)run.head->vertex_count * VP_VERT_STRIDE;',
     '(u64)run.head->vertex_count;'),
    ('head rule removed', BACKEND,
     'if (merge_on && draw_merge_head_ok(dr))',
     'if (merge_on)'),
    ('switch on when unset', INCLUDE,
     's_draw_merge_mode = e && atoi(e) > 0 && !rsx_env_once("VP_PROBE") ? 2 : 1;',
     's_draw_merge_mode = 2;'),
    ('switch ignores VP_PROBE', INCLUDE,
     'e && atoi(e) > 0 && !rsx_env_once("VP_PROBE") ? 2 : 1;',
     'e && atoi(e) > 0 ? 2 : 1;'),
    ('statistics count removed', BACKEND,
     '                if (merge_n > 1) draw_merge_count(merge_n);\n', ''),
)


class Stopped(Exception):
    """The stop file appeared."""


class CompileError(RuntimeError):
    """The fixture does not compile."""


def balanced(source, start, name):
    depth = 0
    for at in range(start, len(source)):
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise RuntimeError('unbalanced ' + name)


def definition(source, name):
    match = re.search(r'(?m)^(?:static\s+)?[A-Za-z_][A-Za-z_0-9 \t\*]*?\b' + re.escape(name) + r'\s*\(', source)
    if not match:
        raise RuntimeError('missing function ' + name)
    return balanced(source, match.start(), name)


def read(name):
    return (ROOT / name).read_text(encoding='utf-8')


def expand(backend, include):
    """The fixture with the production text in place of its markers."""
    at = backend.index('typedef struct {')
    end = backend.index('} D3D12DrawRecord;', at) + len('} D3D12DrawRecord;')
    loop = balanced(backend, backend.index(LOOP_START), 'VP pass')
    parts = (('@DEFINES@', '\n'.join(line.split('/*')[0].rstrip() for line in DEFINE.findall(backend))),
             ('@RECORD@', backend[at:end]), ('@HELPERS@', '\n\n'.join(definition(backend, name) for name in HELPERS)),
             ('@MERGE@', include), ('@LOOP@', loop))
    text = read(FIXTURE)
    for marker, replacement in parts:
        if text.count(marker) != 1:
            raise RuntimeError('marker %s is not unique in the fixture' % marker)
        text = text.replace(marker, replacement)
    return text


def execute(command, stop_file, env=None):
    """Runs a command. A stop file that appears ends the command and its children."""
    if stop_file is not None and stop_file.exists():
        raise Stopped(str(stop_file))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               errors='replace', env=env)
    killed = False
    while True:
        try:
            out, err = process.communicate(timeout=0.5)
            break
        except subprocess.TimeoutExpired:
            if stop_file is not None and stop_file.exists() and not killed:
                killed = True
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
    if killed:
        raise Stopped(str(stop_file))
    return process.returncode, out, err


def build(cc, text, out, name, stop_file):
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(text, encoding='utf-8')
    if 'clang-cl' in cc:
        command = [cc, '/nologo', '/std:c11', '/I', str(ROOT / 'include'), '/I', str(ROOT / 'libs/video'),
                   '/DMAX_DRAWS=%d' % MAX_DRAWS, str(src), '/Fe' + str(exe), '/Fo' + str(out / (name + '.obj'))]
    else:
        command = [cc, '-std=c11', '-I', str(ROOT / 'include'), '-I', str(ROOT / 'libs/video'),
                   '-DMAX_DRAWS=%d' % MAX_DRAWS, str(src), '-o', str(exe)]
    code, out_text, err = execute(command, stop_file)
    if code or not exe.exists():
        raise CompileError('%s does not compile:\n%s' % (name, (out_text + err)[-6000:]))
    return exe


def blocks(text):
    """The lines of each batch, from `batch <seed>` up to the next batch."""
    out, current = [], None
    for line in text.splitlines():
        word = line.split(' ')[0]
        if word == 'batch':
            current = (int(line.split(' ')[1]), [])
            out.append(current)
        if current is not None:
            current[1].append(line)
    return out


def run(exe, first, count, switches, stop_file):
    env = dict(os.environ)
    for name in ('RSX_DRAW_MERGE', 'VP_PROBE'):
        env.pop(name, None)
    env.update(switches)
    code, out, err = execute([str(exe), str(first), str(count)], stop_file, env)
    if code or not out.rstrip().endswith('done %d' % count):
        raise RuntimeError('fixture ended with %d\n%s' % (code, (out + err)[-2000:]))
    if 'FAIL' in err:
        raise RuntimeError(err[-2000:])
    return blocks(out)


def lines_of(batches):
    return [lines for _, lines in batches]


def reduce(lines):
    """Drops the commands that set the current value again."""
    state, out = {}, []
    for line in lines:
        parts = line.split(' ')
        if parts[0] in ('draw', 'batchend', '[RSXPHASE]'):
            continue
        if parts[0] == 'table':
            key = 'table ' + parts[1]
        else:
            key = parts[0] if parts[0] in SETTERS else None
        if key is not None:
            if state.get(key) == line:
                continue
            state[key] = line
        out.append(line)
    return out


def model(lines):
    """Draws without the merge, draws with the merge (the runs of the model),
    and the runs with two or more records."""
    draws = runs = merged = length = 0
    previous = None
    for line in lines:
        if not line.startswith('rec '):
            continue
        kind, first, count, key = line.split(' ')[2:6]
        first, count = int(first), int(count)
        if kind != 'draw':
            previous, length = None, 0
            draws += kind == 'single'
            runs += kind == 'single'
            continue
        draws += 1
        if previous and previous[0] == key and previous[1] + previous[2] == first:
            length += 1
        else:
            merged += length >= 2
            runs += 1
            length = 1
        previous = (key, first, count)
    return draws, runs, merged + (length >= 2)


def draws_of(lines):
    return sum(line.startswith('draw ') for line in lines)


def audit(off, on, label):
    """Failures of the merge of each batch against the pass without it."""
    failures = []
    if len(off) != len(on):
        return ['%s: %d batches without the merge, %d with it' % (label, len(off), len(on))]
    for (seed, a), (_, b) in zip(off, on):
        draws, runs, merged = model(a)
        where = '%s: batch %u' % (label, seed)
        if draws_of(a) != draws:
            failures.append('%s: %d draws without the merge, the model has %d' % (where, draws_of(a), draws))
        if draws_of(b) != runs:
            failures.append('%s: %d draws with the merge, the model has %d runs (%d draws, %d merged runs)'
                            % (where, draws_of(b), runs, draws, merged))
        report = [line for line in b if line.startswith('[RSXPHASE]')]
        wanted = [REPORT % (merged, draws, runs)] if merged else []
        if report != wanted:
            failures.append('%s: report %s, the model wants %s' % (where, report, wanted))
        left, right = reduce(a), reduce(b)
        if left != right:
            at = 0
            while at < min(len(left), len(right)) and left[at] == right[at]:
                at += 1
            failures.append('%s: line %d differs\n    without: %s\n    with:    %s'
                            % (where, at, left[at] if at < len(left) else '<end>',
                               right[at] if at < len(right) else '<end>'))
        for line in left + right:
            if SUSPICIOUS.search(line):
                failures.append('%s: suspicious line %s' % (where, line))
                break
    return failures[:6]


def audit_switch(reference, other, label):
    """The pass must be the reference pass when the switch does not merge."""
    if lines_of(reference) != lines_of(other):
        return ['%s: the commands differ from the pass with RSX_DRAW_MERGE unset' % label]
    return []


def main(argv=None):
    parser = argparse.ArgumentParser(description='Draw merge check.')
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang-cl'), help='C compiler')
    parser.add_argument('--batches', type=int, default=240, help='batches per run')
    parser.add_argument('--stop-file', type=Path, help='the check ends when this file appears')
    args = parser.parse_args(argv)
    failures, rejected = [], []
    totals = [0, 0, 0]
    out = Path(tempfile.mkdtemp(prefix='rsx_draw_merge_'))
    try:
        backend, include = read(BACKEND), read(INCLUDE)
        base = build(args.cc, expand(backend, include), out, 'base', args.stop_file)
        off = run(base, 0, args.batches, {}, args.stop_file)
        on = run(base, 0, args.batches, ON, args.stop_file)
        failures += audit(off, on, 'merge')
        for (_, lines_off), (_, lines_on) in zip(off, on):
            totals[0] += model(lines_off)[0]
            totals[1] += draws_of(lines_on)
            totals[2] += model(lines_off)[2]
        if totals[2] == 0 or totals[1] >= totals[0]:
            failures.append('the batches have no merged run: %d draws, %d with the merge' % (totals[0], totals[1]))
        failures += audit_switch(off, run(base, 0, args.batches, {'RSX_DRAW_MERGE': '0'}, args.stop_file),
                                 'RSX_DRAW_MERGE=0')
        failures += audit_switch(off, run(base, 0, args.batches, PROBE, args.stop_file),
                                 'RSX_DRAW_MERGE=1 with VP_PROBE=1')
        print('batches=%d draws=%d with_merge=%d merged_runs=%d' % (args.batches, totals[0], totals[1], totals[2]))
        half = max(args.batches // 2, 1)
        for number, (name, path, old, new) in enumerate(MUTANTS):
            changed = [backend, include]
            which = 0 if path == BACKEND else 1
            if changed[which].count(old) != 1:
                failures.append('mutant %s: its text is not in %s once' % (name, path))
                continue
            changed[which] = changed[which].replace(old, new)
            try:
                exe = build(args.cc, expand(changed[0], changed[1]), out, 'mutant_%d' % number, args.stop_file)
            except CompileError as error:
                failures.append('mutant %s: %s' % (name, error))
                continue
            found = audit(run(exe, 0, half, {}, args.stop_file), run(exe, 0, half, ON, args.stop_file), 'merge')
            found += audit_switch(off[:half], run(exe, 0, half, {}, args.stop_file), 'RSX_DRAW_MERGE unset')
            found += audit_switch(off[:half], run(exe, 0, half, PROBE, args.stop_file),
                                  'RSX_DRAW_MERGE=1 with VP_PROBE=1')
            if found:
                rejected.append(name)
                print('rejected: %s: %s' % (name, found[0][:150]))
            else:
                failures.append('mutant accepted: %s' % name)
    except Stopped as stop:
        print('STOPPED: %s appeared' % stop)
        return 3
    except (RuntimeError, ValueError) as error:
        failures.append(str(error))
    for failure in failures:
        print('FAIL %s' % failure)
    print('draw merge checks=%d failures=%d' % (1 + len(MUTANTS), len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
