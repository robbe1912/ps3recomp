#!/usr/bin/env python3
"""Build the actual interpreter and authored leaf tests; reject unsafe mutants."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default=os.environ.get('CC') or shutil.which('clang-cl' if os.name=='nt' else 'cc'))
args = parser.parse_args()
assert args.compiler, 'Set CC or --compiler'
out = root / 'build/native-leaf-tests'
out.mkdir(parents=True, exist_ok=True)
env = {k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}

import spu_owned_options
# The production owned-job option readers, extracted verbatim from spu_channels.c.
owned_options=out/'owned-options.c'
owned_options.write_text(spu_owned_options.source(root))

def build(name, source, readers=True, expect_link=True):
    directory=out/name
    directory.mkdir(exist_ok=True)
    exe=directory/('fixture.exe' if os.name=='nt' else 'fixture')
    command=[args.compiler, str(source), str(spu/'spu_float_simd.c'), str(spu/'tests/spu_native_leaf_test.c'),
             *([str(owned_options)] if readers else [])]
    if os.name=='nt':
        command+=['/nologo','/std:c17','/O2','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                  '/I'+str(root/'include'),'/I'+str(spu),'/Fo'+str(directory)+os.sep,'/Fe'+str(exe)]
    else:
        command+=['-std=gnu17','-O2','-UNDEBUG','-I'+str(root/'include'),'-I'+str(spu),'-lpthread','-lm','-o',str(exe)]
    (directory/'command.json').write_text(json.dumps(command,indent=2))
    r=subprocess.run(command,cwd=root,env=env,capture_output=True,text=True)
    (directory/'build.log').write_text(r.stdout+r.stderr)
    if not expect_link: return r
    assert r.returncode==0, r.stdout+r.stderr
    return exe

def run(exe, mode='normal', extra=None):
    r=subprocess.run([str(exe),mode],cwd=exe.parent,env=dict(env,**(extra or {})),capture_output=True,text=True,timeout=20)
    (exe.parent/(mode+'.log')).write_text(r.stdout+r.stderr)
    return r

# Count real runtime environment reads without replacing getenv results.
source=(spu/'spu_interp.c').read_text()
reader=r'''
static atomic_uint leaf_test_env_reads;
unsigned leaf_test_environment_reads(void) { return atomic_load(&leaf_test_env_reads); }
static char* leaf_test_getenv(const char* name) {
    static const char* options[]={"SPU_LS_LOWREAD","SPU_LS_WATCH","SPU_SMC_WATCH",
        "SPU_PCHIST","SPU_TRACE_CALLS","SPU_BOUNDARY_DIR","SPU_CALLTRACE"};
    for(unsigned i=0;i<sizeof(options)/sizeof(options[0]);i++)
        if (!strcmp(name,options[i])) { atomic_fetch_add(&leaf_test_env_reads,1);break; }
    return getenv(name);
}
#define getenv leaf_test_getenv
'''
def instrument(contents):
    anchor='/* Work descriptor for the next SYS_SPU_THREAD_STOP_RECEIVE_EVENT service'
    assert contents.count(anchor)==1
    return contents.replace(anchor,reader+'\n'+anchor)
production=out/'instrumented-interpreter.c'
production.write_text(instrument(source))
# Historical control: without the real readers the interpreter does not link
# (macOS reported it first; GNU ld and link.exe fail the same way).
r=build('missing-owned-readers',production,readers=False,expect_link=False)
assert r.returncode!=0 and 'spu_native_owned_enabled' in r.stdout+r.stderr,r.stdout+r.stderr
exe=build('production',production)
for mode in ['normal','empty','lookups','cold_workers','prefreeze_trace']:
    r=run(exe,mode);assert r.returncode==0,r.stdout+r.stderr
for mode,message in [('bad','continuation/status'),('plan','invalid plan'),('ambiguous','ambiguous guards')]:
    r=run(exe,mode)
    assert r.returncode==86 and '[spu-native-leaf] invalid execution: '+message in r.stderr,r.stdout+r.stderr
for variable in ['SPU_LS_LOWREAD','SPU_LS_WATCH','SPU_SMC_WATCH','SPU_PCHIST','SPU_TRACE_CALLS',
                 'SPU_BOUNDARY_DIR','SPU_CALLTRACE','YDKJ_SPU_TRACE','YDKJ_CRI_GATE1TRACE','SPU_STEPCAP']:
    r=run(exe,'instrumented',{variable:'0' if variable=='SPU_CALLTRACE' else '100', **({'NATIVE_TEST_STARTUP_OPTION':variable} if variable.startswith(('SPU_LS_','SPU_SMC_','SPU_PCHIST','SPU_TRACE_CALLS','SPU_BOUNDARY_DIR','SPU_CALLTRACE')) else {})})
    assert r.returncode==0,r.stdout+r.stderr
    if variable=='SPU_CALLTRACE': assert '[SPU-TRACE] halt at LS 0x00200' in r.stderr,r.stderr

source=(spu/'spu_interp.c').read_text()
for name,before,after in [
    ('hot-environment-scan','!native_instrumented)', '!native_instrumented && !getenv("SPU_LS_LOWREAD"))'),
    ('missing-seam','if (native_count && ctx->pc<0x40000','if (0 && ctx->pc<0x40000'),
    ('stop-bypass','(stop && stop>=leaf->begin && stop<leaf->end)','0'),
    ('zero-stop-sentinel','(stop && stop>=leaf->begin && stop<leaf->end)','(stop>=leaf->begin && stop<leaf->end)'),
    ('budget-bypass','if (plan.instructions>remaining) continue;','/* omitted budget admission */'),
]:
    assert source.count(before)==1,name
    path=out/(name+'.c');path.write_text(instrument(source.replace(before,after)))
    r=run(build(name,path),'lookups' if name=='hot-environment-scan' else 'normal')
    message='FAIL no warmed native-option environment reads' if name=='hot-environment-scan' else 'FAIL real host execution count'
    assert r.returncode==1 and message in r.stderr,r.stdout+r.stderr
print('PASS actual interpreter native-leaf state/stop/budget/guard/FP/thread tests with the production owned-job readers; missing-reader link control and five unsafe dispatch/environment mutants rejected')
