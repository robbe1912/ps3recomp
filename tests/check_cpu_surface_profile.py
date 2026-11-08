"""Compile production CPU surface request/service against an authored clock/queue model."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
    match=re.search(r'static (?:void|u64) '+name+r'\([^)]*\)\s*\{',source)
    if not match:raise RuntimeError('Missing production function '+name)
    depth=1;end=match.end()
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]
def main():
    source=(ROOT/'libs/video/rsx_cpu_surface.inc').read_text()
    out=ROOT/'build/self-tests/cpu-surface-profile';out.mkdir(parents=True,exist_ok=True)
    fixture='#include "ps3emu/guest_read_sync.h"\n'+(ROOT/'tests/test_cpu_surface_profile.c').read_text()
    fixture=fixture.replace('static void cpu_surface_service(void);','static void cpu_surface_service(int snapshot_boundary);')
    declarations=source[source.index('typedef struct CpuSurfaceRequest'):source.index('static ID3D12CommandAllocator')]
    declarations+='\nstatic int s_cpu_surface_live;\nstatic int cpu_surface_snapshot(const VmReadSnapshotRange *ranges,u32 count,void *staging,u32 output_bytes){(void)ranges;(void)count;(void)staging;(void)output_bytes;return 0;}\n'
    helper=source[source.index('static int s_cpu_surface_profile;'):source.index('static void cpu_surface_fence_wait')]
    for marker,value in [('DECLARATIONS',declarations),('HELPERS',helper),('SERVICE',function(source,'cpu_surface_service')),('COMPLETION',function(source,'cpu_surface_frame_complete')),('REQUEST',function(source,'cpu_surface_request'))]:
        fixture=fixture.replace('/* PRODUCTION_'+marker+' */',value)
    c=out/'check.c';c.write_text(fixture);exe=out/('check.exe' if os.name=='nt' else 'check')
    compiler=os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl') or shutil.which('clang') or shutil.which('cc')
    if not compiler:raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    msvc_style=Path(compiler).name.lower() in ('cl','cl.exe','clang-cl','clang-cl.exe')
    if msvc_style:cmd=[compiler,'/nologo','/O2','/UNDEBUG','/I'+str(ROOT/'libs/video'),'/I'+str(ROOT/'include'),str(c),'/Fo'+str(out) + os.sep,'/Fe'+str(exe)]
    else:cmd=[compiler,'-std=c11','-O2','-UNDEBUG','-Wall','-Wextra','-Werror','-I'+str(ROOT/'libs/video'),'-I'+str(ROOT/'include'),str(c),'-o',str(exe)]
    build=subprocess.run(cmd,capture_output=True,text=True);(out/'build.log').write_text(build.stdout+build.stderr)
    if build.returncode:print(build.stdout+build.stderr);return build.returncode
    run=subprocess.run([str(exe)],capture_output=True,text=True);(out/'run.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr)
    if run.returncode:return run.returncode
    records=[dict(re.findall(r'(\w+)=([0-9A-F]+)',line)) for line in run.stderr.splitlines() if line.startswith('[CPUSURFACE_PROFILE]')]
    assert len(records)==2
    for p in records:
        n={k:int(v,16) if k=='address' else int(v) for k,v in p.items()}
        assert n['total']==n['queue']+n['service']+n['return']
        assert n['queued_at']<=n['started_at']<=n['finished_at']<=n['returned_at']
        assert n['client'] in [1,2] and n['renderer']==2
        assert n['service']==sum(n[k] for k in ['allocation','prior_fence','commands','copy_fence','packing','other'])
        assert [n[k] for k in ['matched','hits','copies','host_bytes','guest_bytes']]==[2,1,1,256,8]
    failed=subprocess.run([str(exe),'clock-failure'],capture_output=True,text=True)
    (out/'clock-failure.log').write_text(failed.stdout+failed.stderr)
    assert failed.returncode==2 and 'profile clock' in failed.stderr
    ordering=subprocess.run([str(exe),'unsubmitted'],capture_output=True,text=True)
    (out/'unsubmitted.log').write_text(ordering.stdout+ordering.stderr)
    assert ordering.returncode==2 and 'CPU service before command submission' in ordering.stderr
    mutant=out/'mutant';mutant.mkdir(exist_ok=True)
    header=(ROOT/'libs/video/rsx_cpu_surface_profile.h').read_text()
    assert header.count('if(stages[i]>remaining)return 0;')==1
    (mutant/'rsx_cpu_surface_profile.h').write_text(header.replace('if(stages[i]>remaining)return 0;','if(stages[i]>remaining)return 1;'))
    changed=list(cmd)
    if msvc_style:
        changed.insert(1,'/I'+str(mutant));changed=[('/Fe'+str(mutant/'check.exe')) if x.startswith('/Fe') else x for x in changed]
    else:
        changed[1:1]=['-I'+str(mutant)];changed[-1]=str(mutant/'check')
    build=subprocess.run(changed,capture_output=True,text=True)
    (mutant/'build.log').write_text(build.stdout+build.stderr)
    assert build.returncode==0,build.stdout+build.stderr
    bad=subprocess.run([str(mutant/('check.exe' if os.name=='nt' else 'check'))],capture_output=True,text=True)
    (mutant/'run.log').write_text(bad.stdout+bad.stderr)
    assert bad.returncode==1 and 'overlapping/over-accounted stages rejected' in bad.stderr
    completion=function(source,'cpu_surface_frame_complete')
    assert completion.count('cpu_surface_service(0);')==1
    absent=out/'missing-service.c';absent.write_text(fixture.replace(completion,completion.replace('cpu_surface_service(0);','(void)&cpu_surface_service; /* deliberate missing completion */')))
    missing=out/('missing-service.exe' if os.name=='nt' else 'missing-service')
    changed=[str(absent) if x==str(c) else x for x in cmd]
    if msvc_style:changed=[('/Fe'+str(missing)) if x.startswith('/Fe') else x for x in changed]
    else:changed[-1]=str(missing)
    build=subprocess.run(changed,capture_output=True,text=True);(out/'missing-service-build.log').write_text(build.stdout+build.stderr)
    assert build.returncode==0,build.stdout+build.stderr
    bad=subprocess.run([str(missing)],capture_output=True,text=True);(out/'missing-service-run.log').write_text(bad.stdout+bad.stderr)
    assert bad.returncode==2 and 'queued request not released at completed batch' in bad.stderr
    print('PASS: submitted pixel/waiter completion, premature-order/missing-service and clock/accounting controls');return 0
if __name__=='__main__':sys.exit(main())
