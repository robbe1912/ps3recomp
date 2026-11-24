"""Compile the production PPU wait profile header with the authored harness.

The parent sets PPU_WAIT_PROFILE; the harness never does. The check asserts
the default-off silence, the switch values, the record fields, a measured
wait, the replace rule of a nested begin, the production hook wiring, and
rejects one mutant per rule.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'runtime/syscalls/ppu_wait_profile.h'
HARNESS = ROOT / 'tests/ppu_wait_profile_harness.c'
OUT = ROOT / 'build/self-tests/ppu-wait-profile'
SWITCH = 'PPU_WAIT_PROFILE'
HOOKS = {
    'sys_cond.c': ('sys_cond_wait', 'cond_wait'),
    'sys_semaphore.c': (None, 'semaphore_wait'),
    'sys_mutex.c': ('sys_mutex_lock', 'mutex_lock'),
    'sys_event.c': ('sys_event_queue_receive', 'event_queue_receive'),
}


def compiler_command(compiler, include_dir, harness, out, exe):
    msvc_style = Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    if msvc_style:
        return [compiler, '/nologo', '/O2', '/clang:-Wno-everything', '/I' + str(include_dir), str(harness),
                '/Fo' + str(out) + os.sep, '/Fe' + str(exe)]
    return [compiler, '-std=c11', '-O2', '-I' + str(include_dir), str(harness), '-o', str(exe)]


def build(out, include_dir):
    out.mkdir(parents=True, exist_ok=True)
    compiler = (os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl')
                or shutil.which('clang') or shutil.which('cc'))
    if not compiler:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    exe = out / ('harness.exe' if os.name == 'nt' else 'harness')
    result = subprocess.run(compiler_command(compiler, include_dir, HARNESS, out, exe),
                            capture_output=True, text=True)
    (out / 'build.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe


def run(exe, mode, switch=None):
    env = dict(os.environ)
    env.pop(SWITCH, None)
    if switch is not None:
        env[SWITCH] = switch
    result = subprocess.run([str(exe), mode], capture_output=True, text=True, env=env, timeout=30)
    lines = result.stderr.splitlines()
    records = [dict(re.findall(r'(\w+)=(\w+)', line)) for line in lines
               if line.startswith('[PPU_WAIT_PROFILE]')]
    other = [line for line in lines if not line.startswith('[PPU_WAIT_PROFILE]')]
    return result, records, other


def seconds(record):
    return (int(record['finished']) - int(record['started'])) / int(record['frequency'])


def verify(exe):
    """Raise AssertionError when the harness at exe breaks one rule."""
    for name, switch in (('off', None), ('zero', '0'), ('word', 'yes')):
        result, records, other = run(exe, 'on', switch)
        assert result.returncode == 0 and result.stdout.strip() == 'brackets=1', (name, result.stdout, result.stderr)
        assert not records and not other, (name, records, other)

    result, records, other = run(exe, 'on', '1')
    assert result.returncode == 0 and not other, (result.stdout, result.stderr)
    assert len(records) == 1, records
    record = records[0]
    assert (record['tid'], record['kind'], record['id'], record['lr']) == ('7', 'cond_wait', '3', '00ABCDEF'), record
    assert int(record['frequency']) > 0 and 0 <= seconds(record) < 1.0, record

    result, records, other = run(exe, 'sleep', '1')
    assert result.returncode == 0 and not other and len(records) == 1, (result.stdout, result.stderr, records)
    assert records[0]['kind'] == 'cond_wait' and 0.030 <= seconds(records[0]) < 2.0, records[0]

    result, records, other = run(exe, 'nested', '1')
    assert result.returncode == 0 and not other and len(records) == 1, (result.stdout, result.stderr, records)
    assert (records[0]['kind'], records[0]['id']) == ('mutex_lock', '9'), records[0]


def verify_wiring():
    """Each production wait has one begin with its object id and one end."""
    for name, (wrapped, kind) in HOOKS.items():
        source = (ROOT / 'runtime/syscalls' / name).read_text()
        assert source.count('#include "ppu_wait_profile.h"') == 1, name
        begin = 'PPU_WAIT_PROFILE_BEGIN(ctx, "%s", LV2_ARG_U32(ctx, 0));' % kind
        assert source.count(begin) == 1, (name, kind)
        if wrapped:
            assert source.count('static int64_t %s_impl(ppu_context* ctx)' % wrapped) == 1, (name, wrapped)
            pattern = (r'int64_t %s\(ppu_context\* ctx\)\s*\{\s*' % wrapped
                       + re.escape(begin)
                       + r'\s*int64_t result = %s_impl\(ctx\);\s*ppu_wait_profile_end\(\);\s*return result;\s*\}' % wrapped)
        else:
            pattern = (r'int64_t sys_semaphore_wait\(ppu_context\* ctx\)\s*\{\s*' + re.escape(begin)
                       + r'\s*int64_t r = sys_semaphore_wait_impl\(ctx\);\s*ppu_wait_profile_end\(\);')
        assert len(re.findall(pattern, source)) == 1, (name, kind)


MUTANTS = [
    ('gate', 'if (!ppu_wait_profile_on()) {', 'if (0) {'),
    ('start-as-finish', 't_ppu_wait_profile.lr, t_ppu_wait_profile.start, finished,',
     't_ppu_wait_profile.lr, t_ppu_wait_profile.start, t_ppu_wait_profile.start,'),
    ('open-bracket-kept', '    t_ppu_wait_profile.active = 0;\n    finished = ppu_wait_profile_now();',
     '    finished = ppu_wait_profile_now();'),
    ('end-ignores-bracket', '    if (!t_ppu_wait_profile.active)\n        return;', '    if (0)\n        return;'),
    ('begin-never-opens', '    t_ppu_wait_profile.active = 1;', '    t_ppu_wait_profile.active = 0;'),
    ('any-value', "value[0] == '1' && value[1] == 0", 'value[0] != 0'),
    ('call-site-lost', '    t_ppu_wait_profile.lr = lr;', '    t_ppu_wait_profile.lr = 0;'),
    ('nested-begin-ignored', '    if (!ppu_wait_profile_on()) {',
     '    if (!ppu_wait_profile_on() || t_ppu_wait_profile.active) {'),
]


def main():
    verify_wiring()
    exe = build(OUT, ROOT / 'runtime/syscalls')
    verify(exe)
    header = HEADER.read_text()
    for name, anchor, replacement in MUTANTS:
        assert header.count(anchor) == 1, (name, anchor)
        mutant = OUT / 'mutant' / name
        mutant.mkdir(parents=True, exist_ok=True)
        shutil.copy(HARNESS, mutant / HARNESS.name)
        (mutant / HEADER.name).write_text(header.replace(anchor, replacement))
        bad = build(mutant, mutant)
        try:
            verify(bad)
        except AssertionError:
            continue
        raise AssertionError('mutant accepted: ' + name)
    print('PASS: default off, switch values, record fields, measured wait, nested begin; '
          + '%d mutants rejected' % len(MUTANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
