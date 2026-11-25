#!/usr/bin/env python3
"""Exercise the accessor probe gate and the SPU_LANE_TRACE switch with the real runtime.

One process per environment: the gate reads the environment once. The lifted
bodies come from the lifter. Each build compiles a private copy of the runtime,
so a mutant changes only that copy. --mutants adds the four planted defects."""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path);ap.add_argument('--mutants',action='store_true');args=ap.parse_args()
out=(args.output or root/'temp/checks'/('spu-ls-gate-'+str(time.time_ns()))).resolve();out.mkdir(parents=True,exist_ok=True)
fixture=root/'runtime/spu/tests/test_spu_ls_gate.c'
TREES=('runtime/spu','runtime/platform','runtime/memory','runtime/diag','runtime/ppu','libs/spurs','runtime/ps3_log.h','include')
names=['spu_channels.c','spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c','spu_float_simd.c','spu_task_lifecycle.c']
EXTRA=['runtime/platform/win32_compat.c','runtime/memory/guest_read_sync.c','runtime/diag/spu_execution_observer.c']
SILENT=['[spurs-pm] poll','[wws-res]','[wws-code]','[spu-lowread]','[spu-SMC]','[spu-watch']
# (name, file, before, after): one planted defect each. Every pair must hit once.
MUTANTS=[
 ('gate-inverted','runtime/spu/spu_context.h',
  '    if (__builtin_expect(g_spu_ls_gate != 0, 0))\n        return spu_ls_read_probes(ctx, lsa);',
  '    if (__builtin_expect(g_spu_ls_gate == 0, 0))\n        return spu_ls_read_probes(ctx, lsa);'),
 ('mask-lost','runtime/spu/spu_context.h',
  '    return spu_ls_load16(&ctx->ls[lsa & SPU_LS_MASK & ~0xFu]);',
  '    return spu_ls_load16(&ctx->ls[lsa]);'),
 ('probe-lost-when-armed','runtime/spu/spu_context.h',
  '    if (g_spu_ls_gate & SPU_LS_GATE_SMC)\n        spu_ls_write_probe_smc(ctx, lsa, p);',
  '    if (0)\n        spu_ls_write_probe_smc(ctx, lsa, p);'),
 ('poll-line-unconditional','runtime/spu/spu_channels.c',
  '            if (spu_lane_trace() && (n <= 4 || (n % 4096) == 0))',
  '            if (n <= 4 || (n % 4096) == 0)'),
]
def lifted():
    sys.path.insert(0,str(root/'tools'))
    from spu_lifter import SPULifter,LiftedFunction
    from spu_disasm import spu_decode
    def i10(op,rt,ra,imm10):return (op<<24)|((imm10&0x3FF)<<14)|(ra<<7)|rt
    def ri16(op9,rt,imm16):return (op9<<23)|((imm16&0xFFFF)<<7)|rt
    IL,ILHU,IOHL,AI,LQD,STQD=0b010000001,0b010000010,0b011000001,0x1C,0x34,0x24
    # ILHU clears the low halfword, IOHL sets it: ilhu+iohl builds a 32-bit value.
    edge=[ri16(IL,3,0),ri16(ILHU,4,4),i10(AI,5,4,-16),ri16(IL,6,0),ri16(IOHL,6,0x1235),
          i10(AI,7,5,0x45),i10(AI,8,6,5),ri16(ILHU,10,0x1122),ri16(IOHL,10,0x3344),
          ri16(ILHU,12,0x5566),ri16(IOHL,12,0x7788),i10(LQD,3,3,0),i10(LQD,4,4,0),
          i10(LQD,5,5,0),i10(LQD,6,6,0),i10(STQD,10,7,0),i10(STQD,12,8,0),
          ri16(IL,0,0x240),0x35000000]
    poll=[ri16(IL,2,0x9D0),0x35200100,ri16(IL,5,0x1320),ri16(IL,6,0xDF0),ri16(IL,10,0x5A5A),
          i10(STQD,10,5,0),i10(LQD,9,6,0),ri16(IL,0,0x240),0x35000000]
    def body(words,pc):
        lines=[]
        for word in words:
            lines+=SPULifter()._translate(spu_decode(word,pc),LiftedFunction(name='probe',start_addr=pc,end_addr=pc+4)).split('\n')
            pc+=4
        return '\n'.join(lines)
    return ('/* Lifted by tools/test_spu_ls_gate.py. Do not edit. */\n'
            'static void lifted_edge_job(spu_context* c) { spu_context* ctx = c;\n'+body(edge,0x4000)+'\n}\n'
            'static void lifted_poll_job(spu_context* c) { spu_context* ctx = c;\n'+body(poll,0x5000)+'\n}\n')
def stage(label,changes=()):
    src=out/label/'src'
    for rel in TREES:
        if (root/rel).is_dir():shutil.copytree(root/rel,src/rel,ignore=shutil.ignore_patterns('out_*','rpcs3_oracle','gcc_*.txt','__pycache__'))
        else:(src/rel).parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/rel,src/rel)
    for rel,before,after in changes:
        path=src/rel;text=path.read_text()
        assert text.count(before)==1,f'{label}: {rel}: {text.count(before)} hits for {before[:40]!r}'
        path.write_text(text.replace(before,after))
    (src/'runtime/spu/tests/spu_ls_gate_call.h').write_text(lifted())
    return src
def build(src,label,changes):
    target=out/label;spu=src/'runtime/spu'
    compiler=(os.environ.get('MV_CLANG') or shutil.which('clang-cl')) if os.name=='nt' else (os.environ.get('CC') or shutil.which('cc'))
    assert compiler,'supported C compiler required'
    identity=subprocess.run([compiler,'--version'],capture_output=True,text=True,timeout=10)
    assert identity.returncode==0,'compiler identification failed: '+identity.stderr
    clang='clang' in identity.stdout.lower()
    sources=[spu/'tests/test_spu_ls_gate.c',*[spu/n for n in names],*[src/n for n in EXTRA]]
    if clang:sources.append(spu/'spu_dispatch_mt.c')
    includes=[spu/'tests',src/'include',spu,src/'runtime/platform']
    (target/'obj').mkdir()
    binary=target/('test.exe' if os.name=='nt' else 'test')
    if os.name=='nt':
        command=[compiler,'/nologo','/TC','/std:c17','/O1','/UNDEBUG','/MDd','/D_CRT_SECURE_NO_WARNINGS',
                 *['/I'+str(p) for p in includes],*map(str,sources),'/Fo'+str(target/'obj')+os.sep,
                 '/Fe'+str(binary),'/link','Synchronization.lib']
    else:
        command=[compiler,'-std=gnu17','-O1','-UNDEBUG','-D_CRT_SECURE_NO_WARNINGS',*['-I'+str(p) for p in includes],
                 *map(str,sources),'-pthread','-lm','-o',str(binary)]
    started=time.time()
    result=subprocess.run(command,capture_output=True,text=True,timeout=600)
    (target/'build.log').write_text(result.stdout+result.stderr)
    if result.returncode:print(result.stdout+result.stderr,flush=True)
    assert result.returncode==0,'compile failed: '+str(target/'build.log')
    entry={'label':label,'compiler':identity.stdout.splitlines()[0],'seconds':round(time.time()-started,1),
           'mutated_sha256':{rel:hashlib.sha256((src/rel).read_bytes()).hexdigest() for rel,_,_ in changes},
           'executable_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
    return binary,entry
def run(binary,entry,mode,env_extra,expect,absent):
    env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
    env.update(env_extra)
    result=subprocess.run([str(binary),mode],cwd=binary.parent,env=env,capture_output=True,text=True,timeout=60)
    ok=result.returncode==0 and 'PASS' in result.stdout and all(k in result.stderr for k in expect) \
       and not any(k in result.stderr for k in absent)
    entry.setdefault('runs',[]).append({'mode':mode,'env':env_extra,'returncode':result.returncode,
        'ok':ok,'stdout':result.stdout,'stderr':result.stderr})
    return ok,result
# mode, environment additions, line kinds that must print, line kinds that must not print
CASES=[('edge',{},[],SILENT),
       ('edge',{'SPU_LANE_TRACE':'0'},[],SILENT),
       ('policy',{},[],SILENT),
       ('policy',{'SPU_LANE_TRACE':'1'},['[spurs-pm] poll #1','[wws-res]','[wws-code]'],[]),
       ('edge',{'SPU_LS_LOWREAD':'1'},['[spu-lowread] img=77 read LS 0x000'],[]),
       ('edge',{'SPU_SMC_WATCH':'77'},['[spu-SMC] img=77 WROTE CODE @0x01230'],[]),
       ('edge',{'SPU_LS_WATCH':'0x1230'},['[spu-watch'],[])]
report={'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
        'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'lifter_sha256':hashlib.sha256((root/'tools/spu_lifter.py').read_bytes()).hexdigest(),'variants':[]}
variants=[('production',())]
if args.mutants:variants+=[(name,[(rel,before,after)]) for name,rel,before,after in MUTANTS]
for name,changes in variants:
    src=stage(name,changes)
    binary,entry=build(src,name,changes)
    report['variants'].append(entry)
    for mode,env_extra,expect,absent in CASES:
        ok,result=run(binary,entry,mode,env_extra,expect,absent)
        if not changes:assert ok,(name,mode,env_extra,result.stdout,result.stderr)
        elif not ok:entry.setdefault('killed_by',[]).append(mode)
        print(('PASS ' if ok else 'FAIL ')+name+' '+mode+' '+str(env_extra),flush=True)
    if changes:
        entry['killed']=bool(entry.get('killed_by'))
        print(('KILLED ' if entry['killed'] else 'SURVIVED ')+name+' '+str(entry.get('killed_by')),flush=True)
    (out/'result.json').write_text(json.dumps(report,indent=2))
mutants=[v for v in report['variants'] if v['mutated_sha256']]
assert all(v['killed'] for v in mutants),'a mutant survived'
print('PASS: '+str(len(CASES))+' environment cases; '+str(len(mutants))+' mutants rejected')
print('Evidence: '+str(out/'result.json'))
