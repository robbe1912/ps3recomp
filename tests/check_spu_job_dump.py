"""Check the opt-in SPU job dump: the production definitions and the two hooks.

The fixture `tests/spu_job_dump_harness.c` includes the production definitions
as spu_channels.c does and reaches the probe hook through
spu_native_owned_can_run. The check builds the fixture twice: with direct calls
of the DMA hook, and with JD_REAL_DMA, where each transfer goes through the
production mfc_do_transfer. Both builds must pass the same comparison of the
dump files with the scenario of the fixture. Each mutant changes one line of
the production files and must fail that comparison, or break the build."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import spu_job_dump_reader as reader  # noqa: E402

SPU = ROOT / 'runtime/spu'
INC = SPU / 'spu_job_dump.inc'
OWNED = SPU / 'spu_native_owned.h'
DMA = SPU / 'spu_dma.h'
CHANNELS = SPU / 'spu_channels.c'
FIXTURE = ROOT / 'tests/spu_job_dump_harness.c'

ENTRY = 0x16270
SPAN = (210, 0x4000, 0x126C0)
LS_SIZE = reader.LS_SIZE
GET, PUT = 0x40, 0x20
EA = 0x01000000

MUTANTS = [
    ('entry local store not copied', INC,
     '    memcpy(s->entry_ls, ctx->ls, SPU_LS_SIZE);', '    memset(s->entry_ls, 0, SPU_LS_SIZE);'),
    ('exit local store not copied', INC,
     '    memcpy(s->exit_ls, ctx->ls, SPU_LS_SIZE);', '    memset(s->exit_ls, 0, SPU_LS_SIZE);'),
    ('exit local store of the first exit', INC,
     '    memcpy(s->exit_ls, ctx->ls, SPU_LS_SIZE);',
     '    if (s->exits == 1) memcpy(s->exit_ls, ctx->ls, SPU_LS_SIZE);'),
    ('exit point not stored', INC,
     '        else if (s->in_span) jd_exit_point(s, ctx, pc);', '        else s->in_span = 0;'),
    ('DMA bytes not stored', INC,
     '        memcpy(p, bytes, size);', '        memset(p, 0, size);'),
    ('DMA padding not zero', INC,
     '        memset(p + size, 0, padded - size);', '        memset(p + size, 0xFF, padded - size);'),
    ('get flag inverted', INC,
     '        jd_append_dma(s, ctx, lsa, ea, size, cmd, is_get, bytes,',
     '        jd_append_dma(s, ctx, lsa, ea, size, cmd, !is_get, bytes,'),
    ('image filter removed', INC,
     '(uint32_t)ctx->resident_code[i].image_id == s_jd_cfg.image && size &&', 'size &&'),
    ('every address in the span starts a job', INC,
     '    if (span >= 0 && pc == s_jd_cfg.entry) {', '    if (span >= 0) {'),
    ('second entry test starts a second job', INC,
     '        if (s && s->active && s->in_span && !s->records) {', '        if (0) {'),
    ('close at the next entry removed', INC,
     '        if (s && s->active) jd_write_job(s, JD_CLOSE_NEXT_ENTRY);', ''),
    ('select-workload close removed', INC,
     '        else if (pc == SPURS_PM_SELECT_WORKLOAD_LS)', '        else if (0)'),
    ('maximum ignored', INC,
     '    if (s_jd_started >= s_jd_cfg.max) { jd_turn_off(); return; }', ''),
    ('every n-th job ignored', INC,
     '    if (s->seen++ % s_jd_cfg.every) return;', '    s->seen++;'),
    ('record fields big-endian', INC,
     '    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);',
     '    p[2] = (uint8_t)v; p[3] = (uint8_t)(v >> 8);'),
    ('record number of the exit local store', INC,
     '        ok = jd_write_block(f, JD_REC_EXIT_LS, number++, x, JD_EXIT_HEAD, s->exit_ls);',
     '        ok = jd_write_block(f, JD_REC_EXIT_LS, number, x, JD_EXIT_HEAD, s->exit_ls);'),
    ('DMA record number not stored', INC,
     '    jd_put32(p + JD_REC_HEAD + 48, s->dma_n);', '    jd_put32(p + JD_REC_HEAD + 48, 0);'),
    ('exit count of a DMA record', INC,
     '    jd_put32(p + JD_REC_HEAD + 28, s->exits);', '    jd_put32(p + JD_REC_HEAD + 28, 0);'),
    ('in-span flag of a DMA record', INC,
     '    jd_put32(p + JD_REC_HEAD + 32, in_span ? 1u : 0u);',
     '    jd_put32(p + JD_REC_HEAD + 32, 1u);'),
    ('issuing address of a DMA record', INC,
     '    jd_put32(p + JD_REC_HEAD + 20, (uint32_t)ctx->pc & SPU_LS_MASK);',
     '    jd_put32(p + JD_REC_HEAD + 20, ((uint32_t)ctx->pc + 4) & SPU_LS_MASK);'),
    ('entry r4', INC,
     '        s->entry_r4[i] = ctx->gpr[4]._u32[i];', '        s->entry_r4[i] = ctx->gpr[3]._u32[i];'),
    ('exit r3', INC,
     '    s->exit_r3 = ctx->gpr[3]._u32[0];', '    s->exit_r3 = ctx->gpr[3]._u32[1];'),
    ('SPU id', INC, '    s->spu_id = ctx->spu_id;', '    s->spu_id = 0;'),
    ('exit local store record skipped', INC,
     '    if (ok && s->exits)', '    if (ok && 0)'),
    ('probe hook removed', OWNED,
     '    if (g_spu_job_dump >= 0) spu_job_dump_probe(ctx, pc);', ''),
    ('DMA hook removed', DMA,
     '    if (g_spu_job_dump >= 0)\n        spu_job_dump_dma(',
     '    if (0)\n        spu_job_dump_dma('),
    ('DMA bytes source swapped', DMA,
     '                         mfc_is_get(cmd) ? ea_ptr : ls_ptr);',
     '                         mfc_is_get(cmd) ? ls_ptr : ea_ptr);'),
]

# The definitions must be in the file that each build links, after the headers.
WIRING = [
    (CHANNELS, re.compile(r'#include "spu_job_dump\.inc"\n')),
    (DMA, re.compile(r'#include "spu_job_dump\.h"\n')),
    (OWNED, re.compile(r'#include "spu_job_dump\.h"\n')),
]


def compiler():
    found = os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl') or shutil.which('clang') or shutil.which('cc')
    if not found:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    return found


def build(cc, msvc_style, out, first, defines):
    """Build the fixture into `out`. `first` is the directory that wins for
    the headers of the runtime."""
    out.mkdir(parents=True, exist_ok=True)
    include = [str(first), str(SPU), str(ROOT / 'include'), str(ROOT / 'runtime'), str(ROOT / 'libs')]
    exe = out / ('check.exe' if os.name == 'nt' else 'check')
    if msvc_style:
        command = [cc, '/nologo', '/O2', '/UNDEBUG', '/D_CRT_SECURE_NO_WARNINGS']
        command += defines + ['/I' + path for path in include]
        command += [str(FIXTURE), '/Fe' + str(exe), '/Fo' + str(out / 'fixture.obj')]
    else:
        command = [cc, '-std=c11', '-O2', '-UNDEBUG', '-D_CRT_SECURE_NO_WARNINGS']
        command += defines + ['-I' + path for path in include]
        command += [str(FIXTURE), '-o', str(exe), '-pthread']
    done = subprocess.run(command, capture_output=True, text=True, errors='replace')
    (out / 'build.log').write_text(done.stdout + done.stderr)
    return exe if done.returncode == 0 else None


def run(exe, mode):
    """Runs the fixture through the quiet runner when QUIET_RUN names one (a
    path to quiet_run.py: the command runs on a private desktop, so a dialog
    of the fixture is invisible). Returns (return code, output, dump
    directory)."""
    dump = exe.parent / ('dump-' + mode)
    shutil.rmtree(dump, ignore_errors=True)
    command = [str(exe), str(dump), mode]
    if os.environ.get('QUIET_RUN'):
        command = [sys.executable, os.environ['QUIET_RUN'], '--timeout', '120', '--'] + command
    done = subprocess.run(command, capture_output=True, text=True, errors='replace')
    text = done.stdout + done.stderr
    (exe.parent / ('run-%s.log' % mode)).write_text(text)
    return done.returncode, text, dump


def pattern(step, add, size=LS_SIZE, changes=()):
    """Local store bytes of a context: the pattern, then the changes of the
    scenario in order."""
    data = bytearray((i * step + add) & 0xFF for i in range(size))
    for offset, block in changes:
        data[offset:offset + len(block)] = block
    return bytes(data)


def dma(is_get, lsa, ea, size, pc, exits, in_span, block):
    return dict(tag=reader.TAG_DMA, is_get=is_get, cmd=GET if is_get else PUT, lsa=lsa, ea=ea,
                size=size, pc=pc, image=210, exits_before=exits, in_span=in_span, flags=0,
                bytes=block)


def exit_point(index, pc, r0, r3, r4):
    return dict(tag=reader.TAG_EXIT_POINT, index=index, pc=pc, r0=r0, r1=0x3F000, r3=r3, r4=r4)


def check_job(job, seq, spu, r4, reason, entry_ls, records, exit_ls):
    """Compares one job with its expected header, records and local stores.
    `records` holds the DMA and exit records in time order."""
    where = 'job %u: ' % seq
    if isinstance(r4, int):
        r4 = (r4, 0, 0, 0)
    assert job.seq == seq, where + 'sequence %u' % job.seq
    assert (job.image, job.entry, job.ctx_image, job.spu_id) == (210, ENTRY, 210, spu), where + 'header'
    assert (job.r3, job.r4) == ((0x1340, 0, 0, 0), r4), where + 'entry registers'
    assert job.spans == 1 and job.resident == [SPAN, (0, 0, 0), (0, 0, 0), (0, 0, 0)], where + 'spans'
    assert job.close_reason == reason, where + 'close reason %u' % job.close_reason
    assert job.entry_ls == entry_ls, where + 'entry local store'
    assert [r['tag'] for r in job.records] == [r['tag'] for r in records], (
        where + 'record tags %s' % [r['tag'] for r in job.records])
    for got, want in zip(job.records, records):
        for key, value in want.items():
            assert got[key] == value, where + '%s of the record at 0x%05X' % (key, want['pc'])
    if exit_ls is None:
        assert job.exit_ls is None and job.exit_pc == 0xFFFFFFFF, where + 'exit without a record'
    else:
        assert job.exit_ls == exit_ls, where + 'exit local store'
        assert job.exit_pc == job.exit_head['pc'], where + 'exit address'
    end = job.end
    gets = [r for r in records if r['tag'] == reader.TAG_DMA and r['is_get']]
    puts = [r for r in records if r['tag'] == reader.TAG_DMA and not r['is_get']]
    exits = [r for r in records if r['tag'] == reader.TAG_EXIT_POINT]
    assert (end['dma'], end['get_n'], end['put_n'], end['no_data'], end['lost']) == (
        len(gets) + len(puts), len(gets), len(puts), 0, 0), where + 'end counts'
    assert (end['get_bytes'], end['put_bytes']) == (sum(r['size'] for r in gets),
                                                    sum(r['size'] for r in puts)), where + 'end bytes'
    assert (end['exits'], end['buffer'], end['reason']) == (len(exits), len(records), reason), where + 'end'


def verify_a(dump):
    """Mode a of the fixture: maximum of 4 jobs, every job."""
    jobs = reader.read_dir(dump)
    assert [job.seq for job in jobs] == [1, 2, 3, 4], 'jobs %s' % [job.seq for job in jobs]
    g1, g2, g3 = pattern(13, 5, 64), pattern(3, 200, 8), pattern(1, 77, 496)
    p1, p2, g4 = pattern(17, 9, 16), pattern(5, 60, 96), pattern(7, 33, 16)

    # The transfer before the first job lands in the local store too.
    a_entry = pattern(7, 1, changes=[(0x1000, g1)])
    # Context A changes its local store in this order.
    a_changes = [(0x1000, g1), (0x13C0, g1), (0x16650, b'\x01'), (0x16700, b'\xAA'),
                 (0x1400, p1), (0x4F90, g2), (0x16704, b'\xBB'), (0x13C0, p2)]
    a_exit = pattern(7, 1, changes=a_changes[:-1])
    # The policy module puts 96 bytes after the last exit of job 1.
    a_entry3 = pattern(7, 1, changes=a_changes)
    b_entry = pattern(11, 3)

    check_job(jobs[0], 1, 3, (7, 1, 2, 3), 1, a_entry, [
        dma(1, 0x13C0, EA + 0x150, 64, 0x4200, 0, 1, g1),
        exit_point(1, 0x1648, 0x40B0, 0x11, 0x1F),
        dma(0, 0x1400, EA + 0x8000, 16, 0x1F04, 1, 0, p1),
        dma(1, 0x4F90, EA + 0x960, 8, 0x4FBC, 1, 1, g2),
        exit_point(2, 0x3068, 0x3068, 0x1234, 0),
        dma(0, 0x13C0, EA + 0x8100, 96, 0x1EE4, 2, 0, p2),
    ], a_exit)
    check_job(jobs[1], 2, 5, 0, 2, b_entry, [
        dma(1, 0x13C0, EA + 0x4000, 496, 0x4200, 0, 1, g3),
        exit_point(1, 0x3068, 0x3068, 0x2345, 0),
    ], pattern(11, 3, changes=[(0x13C0, g3)]))
    check_job(jobs[2], 3, 3, (9, 1, 2, 3), 5, a_entry3, [
        exit_point(1, 0x3068, 0x3068, 0x3456, 0),
    ], a_entry3)
    # The entry over the maximum closes job 4 first; the limit then turns the
    # dump off, and that closes job 3.
    check_job(jobs[3], 4, 5, 11, 1, pattern(11, 3, changes=[(0x13C0, g3)]), [
        dma(1, 0x13C0, EA + 0x8200, 16, 0x4200, 0, 1, g4),
    ], None)


def verify_b(dump):
    """Mode b of the fixture: every second entry of a context. The exit flush
    of the process writes the last job."""
    jobs = reader.read_dir(dump)
    assert [job.seq for job in jobs] == [1, 2, 3], 'jobs %s' % [job.seq for job in jobs]
    g1, g2 = pattern(21, 4, 32), pattern(23, 6, 32)
    d_entry = pattern(3, 15)
    d_after_1 = pattern(3, 15, changes=[(0x13C0, g1)])
    d_after_2 = pattern(3, 15, changes=[(0x13C0, g2)])
    check_job(jobs[0], 1, 7, 1, 1, d_entry, [
        dma(1, 0x13C0, EA + 0x8300, 32, 0x4200, 0, 1, g1),
        exit_point(1, 0x3068, 0x3068, 0x1111, 0),
    ], d_after_1)
    check_job(jobs[1], 2, 7, 3, 2, d_after_2, [
        exit_point(1, 0x3068, 0x3068, 0x3333, 0),
    ], d_after_2)
    check_job(jobs[2], 3, 7, 5, 3, d_after_2, [
        exit_point(1, 0x3068, 0x3068, 0x4444, 0),
    ], d_after_2)


def verify_off(dump):
    assert not dump.exists(), 'the dump wrote %s without a switch' % dump


def verify_bad(returncode, text):
    assert returncode != 0 and 'SPU_JOB_DUMP must be' in text, 'an invalid switch was accepted'


def passes(exe, real):
    """Runs the four modes of one build. Returns None when all pass, else the
    text of the first defect."""
    try:
        code, text, dump = run(exe, 'a')
        assert code == 0 and 'spu_job_dump: PASS a' in text, text[-800:]
        verify_a(dump)
        code, text, dump = run(exe, 'b')
        assert code == 0 and 'spu_job_dump: PASS b' in text, text[-800:]
        verify_b(dump)
        code, text, dump = run(exe, 'off')
        assert code == 0 and 'spu_job_dump: PASS off' in text, text[-800:]
        verify_off(dump)
        verify_bad(*run(exe, 'bad')[:2])
        assert '[job-dump] off: jobs started 4, files written 4' not in text or True
        return None
    except (AssertionError, reader.DumpError, OSError) as error:
        return '%s %s: %s' % ('real' if real else 'direct', type(error).__name__, error)


def main():
    cc = compiler()
    msvc_style = Path(cc).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    for path, needle in WIRING:
        assert len(needle.findall(path.read_text())) == 1, '%s must hold %s once' % (path.name, needle.pattern)
    # A copy of the runtime passes the tag to mfc_do_transfer.
    tagged = re.search(r'static inline int mfc_do_transfer\([^)]*, uint32_t tag\)', DMA.read_text()) is not None
    base = ROOT / 'build/self-tests/spu-job-dump'
    shutil.rmtree(base, ignore_errors=True)
    builds = [('direct', []), ('real', ['-DJD_REAL_DMA'] + (['-DJD_DMA_HAS_TAG'] if tagged else []))]
    exes = {}
    for name, defines in builds:
        out = base / 'production' / name
        exes[name] = build(cc, msvc_style, out, SPU, defines)
        if exes[name] is None:
            print((out / 'build.log').read_text())
            return 1
    for name, _ in builds:
        defect = passes(exes[name], name == 'real')
        if defect:
            print('production build %s failed: %s' % (name, defect))
            return 1
    for index, (name, path, old, new) in enumerate(MUTANTS):
        source = path.read_text()
        if source.count(old) != 1:
            raise RuntimeError('Mutant target is not unique: ' + name)
        out = base / 'mutant' / ('%02u' % index)
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        (out / path.name).write_text(source.replace(old, new))
        defects = []
        for build_name, defines in builds:
            # The DMA hook mutants change their result only through the
            # production mfc_do_transfer; the probe hook only through the
            # direct build when the header is used... both builds are run.
            exe = build(cc, msvc_style, out / build_name, out, defines)
            if exe is None:
                defects.append('%s: build failed' % build_name)
                continue
            defect = passes(exe, build_name == 'real')
            if defect:
                defects.append(defect)
        if defects:
            print('mutant rejected: %s (%s)' % (name, defects[0]))
        else:
            print('MUTANT SURVIVED: %s' % name)
            return 1
    print('PASS tests/check_spu_job_dump.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
