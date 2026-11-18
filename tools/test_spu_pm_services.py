#!/usr/bin/env python3
"""Game-free checks for native policy-module services (SPU_PM_NATIVE_SVC).

Builds the actual SPU runtime (interpreter, drain, owned gate, channels) with
an authored fixture: a policy module whose resident job calls a policy
service with bisl. The lifted job call and service bodies are the lifter's own
output. Every mode compares all GPRs, LS, status and stop code with a fully
interpreted run and checks how often the compiled service ran plus the
runtime's counters. Source mutants of the admission, scope and verifier
logic must each fail. No game data is read and no speed is claimed."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT
SPU = SDK / 'runtime/spu'
FIXTURE = ROOT / 'runtime/spu/tests/test_spu_pm_services.c'
HANDOFF = ROOT / 'runtime/spu/tests/test_spu_interpreter_handoff.c'
NAMES = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
         'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c',
         'spu_float_simd.c', 'spu_task_lifecycle.c']
EXTRA = ['runtime/platform/win32_compat.c', 'runtime/memory/guest_read_sync.c', 'runtime/diag/spu_execution_observer.c',
         'runtime/diag/loadtime_trace.c', 'runtime/platform/thread_name.c', 'runtime/syscalls/spu_ea_alias.c']
# mode -> (SPU_PM_NATIVE_SVC, SPU_PM_SVC_VERIFY)
MODES = {
    'off': (None, None), 'native': ('1', None), 'verify': ('1', '1'), 'verify-mismatch': ('1', '1'),
    'text-differs': ('1', None), 'fp-mismatch': ('1', None), 'tail-jump': ('1', None), 'short-span': ('1', None),
    'scope': ('1', None), 'wrong-call-opcode': ('1', None),
    'gate': ('1', None), 'scoped-job': ('1', None),
}
# Each mutant must make at least one mode fail.
MUTANTS = {
    'unchecked-call-opcode': [('job_call = (instruction & 0xffe0007fu) == 0x35200000u;', 'job_call = (instruction != 0);')],
    'no-text-compare': [('if (memcmp(ctx->ls + lo, s_svc.text + (lo - s_svc.ls_base), n) != 0) {', 'if (0) {')],
    'no-fingerprint': [('ok = h == s_svc.text_fnv;', 'ok = 1;')],
    'no-job-call-check': [('if (!job_call) { InterlockedIncrement(&s_svc_not_job_call); return 0; }', '')],
    'scope-unbounded': [('    t_svc.ctx = NULL;\n    return 0;\n}', '    return 1;\n}'),
                        ('if (t_svc.ctx == ctx && t_svc.ret == (return_pc & SPU_LS_MASK) && ctx->host_depth == t_svc.depth)\n        t_svc.ctx = NULL;', '(void)ctx; (void)return_pc;')],
    'gate-bypass': [('if (!spu_native_owned_can_run(ctx, pc)) { t_svc = saved; return 0; }', '')],
    'verify-blind': [('if (!bad) InterlockedIncrement(&s_svc_vok);', 'bad = 0; if (!bad) InterlockedIncrement(&s_svc_vok);')],
    'replay-unfed': [('if (o) r = o->val; else memset(&r, 0, sizeof r);', 'memset(&r, 0, sizeof r);')],
    'registry-ignored': [('return g_spu_pm_svc_on && spu_pm_svc_can_run(ctx,pc);', 'return 0;')],
    'service-ignores-image-scope': [('    if (image) return 0;\n    return g_spu_pm_svc_on',
                                     '    return g_spu_pm_svc_on')],
}


def lifted_header(target):
    sys.path.insert(0, str(SDK / 'tools'))
    from spu_lifter import SPULifter, LiftedFunction
    from spu_disasm import spu_decode

    def tr(word, pc, start, end, name):
        return SPULifter()._translate(spu_decode(word, pc), LiftedFunction(name=name, start_addr=start, end_addr=end))
    lines = ['#define PM_JOB_CALL ' + tr(0x35200100, 0x4004, 0x4000, 0x4014, 'job'),
             '#define PM_JOB_TAIL ' + tr(0x35000100, 0x4004, 0x4000, 0x4008, 'job')]
    for pc, word in [(0x300, 0x21A00B03), (0x304, 0x01A00605), (0x308, 0x1C01C183), (0x30C, 0x35000000)]:
        lines.append('#define PM_SVC_%X ' % pc + tr(word, pc, 0x300, 0x310, 'service'))
    text = '\n'.join(lines) + '\n'
    (target / 'spu_pm_services_call.h').write_text(text, encoding='utf-8')
    return text


def build(out, label, mutations=()):
    target = out / label
    target.mkdir()
    header_text = lifted_header(target)
    handoff = HANDOFF.read_text(encoding='utf-8').replace('#include "../spu_', '#include "spu_')
    (target / 'spu_interpreter_handoff.c').write_text(handoff, encoding='utf-8')
    sources = [FIXTURE, *[SPU / n for n in NAMES], *[SDK / p for p in EXTRA]]
    header = (SPU / 'spu_native_owned.h').read_text(encoding='utf-8')
    if mutations:
        text = (SPU / 'spu_channels.c').read_text(encoding='utf-8')
        for old, new in mutations:
            hits = text.count(old) + header.count(old)
            assert hits == 1, (label, old, hits)
            text = text.replace(old, new)
            header = header.replace(old, new)
        mutated = target / 'spu_channels.c'
        mutated.write_text(text, encoding='utf-8')
        sources[sources.index(SPU / 'spu_channels.c')] = mutated
    (target / 'spu_native_owned.h').write_text(header, encoding='utf-8')
    if os.name == 'nt':
        env = os.environ.copy()
        compiler = env.get('MV_CLANG') or shutil.which('clang-cl')
        if not compiler:
            raise SystemExit('clang-cl plus the Visual Studio developer environment is required')
    else:
        env = os.environ.copy()
        compiler = env.get('CC') or shutil.which('cc')
    clang = 'clang' in subprocess.run([compiler, '--version'], capture_output=True, text=True, timeout=10).stdout.lower()
    if clang:
        sources.append(SPU / 'spu_dispatch_mt.c')
    includes = [target, SDK / 'include', SPU, SDK / 'runtime/platform']
    binary = target / ('test.exe' if os.name == 'nt' else 'test')
    if os.name == 'nt':
        command = [compiler, '/nologo', '/TC', '/std:c17', '/O1', '/UNDEBUG', '/MDd', '/D_CRT_SECURE_NO_WARNINGS',
                   *['/I' + str(p) for p in includes], *map(str, sources), '/Fo' + str(target) + os.sep,
                   '/Fe' + str(binary), '/link', 'Synchronization.lib']
    else:
        command = [compiler, '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG', *['-I' + str(p) for p in includes],
                   *map(str, sources), '-pthread', '-lm', '-o', str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300, env=env)
    (target / 'build.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    entry = dict(label=label, returncode=result.returncode, mutations=[list(m) for m in mutations],
                 lifted_sha256=hashlib.sha256(header_text.encode()).hexdigest(),
                 sources={str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in sources}, runs=[])
    if result.returncode:
        print('\n'.join([x for x in (result.stdout + result.stderr).splitlines() if 'error' in x.lower()][-12:]))
    return binary, entry


def run(binary, mode, native=None, verify=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    env['SPU_NATIVE_OWNED_JOBS'] = '1'
    env['SPU_INTERP_UNLIFTED'] = '1'
    env['SPU_PM_INTERP'] = '1'
    if mode == 'scoped-job':
        env['SPU_NATIVE_OWNED_IMAGE'] = '25'
    if native is not None:
        env['SPU_PM_NATIVE_SVC'] = native
    if verify is not None:
        env['SPU_PM_SVC_VERIFY'] = verify
    r = subprocess.run([str(binary), mode], cwd=binary.parent, env=env, capture_output=True, text=True, timeout=30)
    return dict(mode=mode, native=native, verify=verify, returncode=r.returncode, stdout=r.stdout.strip(),
                stderr=r.stderr.strip()[-1500:])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--production-only', action='store_true', help='Skip the source mutants')
    args = ap.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    out = (args.output or ROOT / 'temp/checks' / ('spu-pm-services-' + stamp)).resolve()
    out.mkdir(parents=True)
    report = dict(game_inputs=False, fixture_sha256=hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
                  checker_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), variants=[], mutants={})
    ok = True
    binary, entry = build(out, 'production')
    report['variants'].append(entry)
    if entry['returncode']:
        ok = False
    else:
        for mode, (native, verify) in MODES.items():
            r = run(binary, mode, native, verify)
            entry['runs'].append(r)
            passed = r['returncode'] == 0 and 'PASS' in r['stdout']
            print(('PASS ' if passed else 'FAIL ') + mode + ('' if passed else ': ' + (r['stderr'] or r['stdout'])))
            ok &= passed
        # The switch is read strictly: unset and 0 are off, 1 is on, anything else aborts.
        modes = {str(v): run(binary, 'env', v) for v in (None, '0', '1', '2')}
        entry['switch'] = modes
        switch_ok = (modes['None']['stdout'] == '0' and modes['0']['stdout'] == '0' and
                     modes['1']['stdout'] == '1' and modes['2']['returncode'] != 0)
        print(('PASS' if switch_ok else 'FAIL') + ' SPU_PM_NATIVE_SVC unset/0/1/2')
        ok &= switch_ok
    for name, mutations in ({} if args.production_only else MUTANTS).items():
        mbin, mentry = build(out, 'mutant-' + name, mutations)
        failed = []
        if mentry['returncode'] == 0:
            for mode, (native, verify) in MODES.items():
                r = run(mbin, mode, native, verify)
                mentry['runs'].append(r)
                if r['returncode'] != 0:
                    failed.append(mode)
        killed = mentry['returncode'] == 0 and bool(failed)
        report['mutants'][name] = dict(killed=killed, failing_modes=failed, build=mentry['returncode'])
        print(f'mutant {name}: ' + (f'killed by {", ".join(failed)}' if killed else 'SURVIVED'))
        ok &= killed
    (out / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(('PASS ' if ok else 'FAIL ') + str(out / 'result.json'))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
