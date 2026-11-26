#!/usr/bin/env python3
"""Exercise the SPU_NATIVE_OWNED_MISS_LOG counter with the real runtime.

One process per environment: the switch reads the environment once. Each
build compiles a private copy of the runtime, so a mutant changes only that
copy. --mutants adds the planted defects (eight)."""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path);ap.add_argument('--mutants',action='store_true')
ap.add_argument('--only',nargs='*',help='build these variants only (names)');args=ap.parse_args()
out=(args.output or root/'temp/checks'/('spu-interp-count-'+str(time.time_ns()))).resolve();out.mkdir(parents=True,exist_ok=True)
fixture=root/'runtime/spu/tests/test_spu_interp_count.c'
TREES=('runtime/spu','runtime/platform','runtime/memory','runtime/diag','runtime/ppu','libs/spurs','runtime/ps3_log.h','include',
       'runtime/syscalls/sys_semaphore.c','runtime/syscalls/sys_semaphore.h','runtime/syscalls/sys_timer.h','runtime/syscalls/lv2_syscall_table.h')
names=['spu_channels.c','spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c','spu_float_simd.c','spu_task_lifecycle.c']
EXTRA=['runtime/platform/win32_compat.c','runtime/memory/guest_read_sync.c','runtime/diag/spu_execution_observer.c','runtime/syscalls/sys_semaphore.c']
GATE='runtime/spu/spu_native_owned.h';SITE='runtime/spu/spu_channels.c'
ON={'SPU_NATIVE_OWNED_MISS_LOG':'1'}
# (name, file, before, after): one planted defect each.
MUTANTS=[
 ('gate-inverted',GATE,'    return __builtin_expect(g_spu_miss_log >= 0, 1) ? g_spu_miss_log : spu_miss_log_init();',
  '    return __builtin_expect(g_spu_miss_log >= 0, 1) ? !g_spu_miss_log : !spu_miss_log_init();'),
 ('counter-not-reset',GATE,'    memset(im->cause, 0, sizeof im->cause);','    (void)im;'),
 ('wrong-lane',GATE,'spu_miss_count(&g_spu_miss, spu_miss_lane(), ctx->image_id,','spu_miss_count(&g_spu_miss, 0, ctx->image_id,'),
 ('wrong-cause-class',GATE,'    return spu_lifted_lookup(ctx,ctx->pc) ? 0 : SPU_MISS_NO_ENTRY;','    return spu_lifted_lookup(ctx,ctx->pc) ? 0 : SPU_STOP_NO_OWNER;'),
 ('line-when-off',GATE,'    g_spu_miss_log = (e && *e && strcmp(e, "0")) ? 1 : 0;','    g_spu_miss_log = 1;'),
 ('stop-reason-drift',GATE,'    if (g_spu_smc_watch > 0) return SPU_STOP_SMC_WATCH;\n    if (spu_owned_observed())','    if (g_spu_smc_watch > 0) return SPU_STOP_NO_OWNER;\n    if (spu_owned_observed())'),
 ('period-lost',GATE,'    if (now_ms - s->last_ms < SPU_MISS_PERIOD_MS) return 0;','    if (0) return 0;'),
 ('image-key-lost',GATE,'    spu_miss_image* im = &s->image[spu_miss_slot(s, image)];','    spu_miss_image* im = &s->image[spu_miss_slot(s, 0)];'),
 ('count-call-lost',SITE,'        if (miss) { spu_miss_hit(ctx, miss); spu_native_owned_interpret(ctx); return; }',
  '        if (miss) { spu_native_owned_interpret(ctx); return; }'),
]
stubs=root/'runtime/spu/tests/spu_interp_count_stubs.h'
def stage(label,changes=()):
    src=out/label/'src'
    shutil.rmtree(out/label,ignore_errors=True)
    for rel in TREES:
        if (root/rel).is_dir():shutil.copytree(root/rel,src/rel,ignore=shutil.ignore_patterns('out_*','rpcs3_oracle','gcc_*.txt','__pycache__'))
        else:(src/rel).parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/rel,src/rel)
    shutil.copy2(stubs,src/'runtime/spu/tests/spu_interp_count_stubs.h')
    for rel,before,after in changes:
        path=src/rel;text=path.read_bytes().decode().replace('\r\n','\n')
        assert text.count(before)==1,f'{label}: {rel}: {text.count(before)} hits for {before[:40]!r}'
        path.write_bytes(text.replace(before,after).encode())
    return src
def build(src,label,changes):
    target=out/label;spu=src/'runtime/spu'
    compiler=(os.environ.get('MV_CLANG') or shutil.which('clang-cl')) if os.name=='nt' else (os.environ.get('CC') or shutil.which('cc'))
    assert compiler,'supported C compiler required'
    identity=subprocess.run([compiler,'--version'],capture_output=True,text=True,timeout=10)
    assert identity.returncode==0,'compiler identification failed: '+identity.stderr
    clang='clang' in identity.stdout.lower()
    sources=[fixture,*[spu/n for n in names],*[src/n for n in EXTRA]]
    if clang:sources.append(spu/'spu_dispatch_mt.c')
    includes=[spu/'tests',src/'include',spu,src/'runtime/platform',src/'runtime/syscalls']
    (target/'obj').mkdir(exist_ok=True)
    binary=target/('test.exe' if os.name=='nt' else 'test')
    if os.name=='nt':
        command=[compiler,'/nologo','/TC','/std:c17','/O1','/UNDEBUG','/MDd','/D_CRT_SECURE_NO_WARNINGS','/DSPU_INTERP_COUNT_STUBS=1',
                 *['/I'+str(p) for p in includes],*map(str,sources),'/Fo'+str(target/'obj')+os.sep,
                 '/Fe'+str(binary),'/link','Synchronization.lib']
    else:
        command=[compiler,'-std=gnu17','-O1','-UNDEBUG','-D_CRT_SECURE_NO_WARNINGS','-DSPU_INTERP_COUNT_STUBS=1',*['-I'+str(p) for p in includes],
                 *map(str,sources),'-pthread','-lm','-o',str(binary)]
    started=time.time()
    result=subprocess.run(command,capture_output=True,text=True,timeout=600)
    (target/'build.log').write_text(result.stdout+result.stderr)
    if result.returncode:print((result.stdout+result.stderr)[-4000:],flush=True)
    assert result.returncode==0,'compile failed: '+str(target/'build.log')
    return binary,{'label':label,'compiler':identity.stdout.splitlines()[0],'seconds':round(time.time()-started,1),
           'mutated_sha256':{rel:hashlib.sha256((src/rel).read_bytes()).hexdigest() for rel,_,_ in changes},
           'executable_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
L='[spu-miss] lane='
# mode, environment additions, (line, count) that must print
CASES=[('unit',{},[]),
       ('smc',ON,[(L+'0 img=77 gate stop smc watch=1',1)]),
       ('owner',ON,[(L+'0 img=77 gate stop no code owner=1',1)]),
       ('entry',ON,[(L+'0 img=77 no lifted entry 0x04000=1',1)]),
       ('lanes',ON,[(L+'0 img=77 gate stop smc watch=1',1),(L+'1 img=77 gate stop smc watch=1',1)]),
       ('smc',{},[]),('entry',{},[]),('smc',{'SPU_NATIVE_OWNED_MISS_LOG':'0'},[])]
report={'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
        'stubs_sha256':hashlib.sha256(stubs.read_bytes()).hexdigest(),
        'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'variants':[]}
def run(binary,mode,env_extra,expect):
    env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
    env['SPU_NATIVE_OWNED_JOBS']='1';env.update(env_extra)
    result=subprocess.run([str(binary),mode],cwd=binary.parent,env=env,capture_output=True,text=True,timeout=60)
    lines=result.stderr.splitlines()
    ok=result.returncode==0 and 'PASS' in result.stdout \
       and all(result.stderr.count(k)==n for k,n in expect) and not any(k.startswith('[spu-miss]') and k not in dict(expect) for k in lines)
    return ok,{'mode':mode,'env':env_extra,'returncode':result.returncode,'ok':ok,'stdout':result.stdout,'stderr':result.stderr}
variants=[('production',())]
if args.mutants:variants+=[(name,[(rel,before,after)]) for name,rel,before,after in MUTANTS]
if args.only:variants=[v for v in variants if v[0] in args.only]
for name,changes in variants:
    src=stage(name,changes)
    binary,entry=build(src,name,changes)
    report['variants'].append(entry)
    for mode,env_extra,expect in CASES:
        ok,result=run(binary,mode,env_extra,expect)
        entry.setdefault('runs',[]).append(result)
        if not changes:assert ok,(name,mode,env_extra,result['stdout'],result['stderr'])
        elif not ok:entry.setdefault('killed_by',[]).append(mode+str(env_extra))
        print(('PASS ' if ok else 'FAIL ')+name+' '+mode+' '+str(env_extra),flush=True)
        (out/'result.json').write_text(json.dumps(report,indent=2))
    if changes:
        entry['killed']=bool(entry.get('killed_by'))
        print(('KILLED ' if entry['killed'] else 'SURVIVED ')+name+' '+str(entry.get('killed_by')),flush=True)
mutants=[v for v in report['variants'] if v['mutated_sha256']]
assert all(v['killed'] for v in mutants),'a mutant survived'
print('PASS: %d environment cases; %d mutants rejected'%(len(CASES),len(mutants)))
print('Evidence: '+str(out/'result.json'))
