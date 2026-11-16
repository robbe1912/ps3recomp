"""Compile the production prompt CPU surface service against an authored queue model, plus mutants."""
import os
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
MARK='/* Prompt service between batches.'
def main():
    source=(ROOT/'libs/video/rsx_cpu_surface.inc').read_text()
    out=ROOT/'build/cpu-surface-prompt-tests';out.mkdir(parents=True,exist_ok=True)
    fixture=(ROOT/'libs/video/tests/test_cpu_surface_prompt.c').read_text()
    declarations=source[source.index('typedef struct CpuSurfaceRequest'):source.index('static ID3D12CommandAllocator')]
    helper=source[source.index('static int s_cpu_surface_profile;'):source.index('static void cpu_surface_fence_wait')]
    assert source.count(MARK)==1
    prompt=source[source.index(MARK):source.index('static void cpu_surface_init')]
    capture=source[source.index('static void cpu_surface_capture'):]
    assert 'cpu_surface_service_ready();' in capture and 'cpu_surface_pending_add(draw->cpu_address[i],draw->cpu_pitch[i],draw->rt_h)' in capture,'recorded draws must register pending targets, then serve'
    init=source[source.index('static void cpu_surface_init'):source.index('static void cpu_surface_retire')]
    assert 'RSX_CPU_SURFACE_PROMPT' in init and 'cpu_surface_batch_submitted();' in init
    backend=(ROOT/'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
    body=backend[backend.index('static void render_frame(void)'):]
    body=body[:body.index('\nstatic ')]
    assert body.index('submit_recorded_commands();')<body.index('cpu_surface_batch_submitted();')<body.index('cpu_surface_frame_complete();'),'pending writes reset only after the batch is submitted'
    def compose(block, template):
        text=template
        for marker,value in [('DECLARATIONS',declarations),('HELPERS',helper),('PROMPT',block)]:
            text=text.replace('/* PRODUCTION_'+marker+' */',value)
        return text
    compiler=os.environ.get('MV_CLANG') or os.environ.get('CC') or ('clang-cl' if os.name=='nt' else 'cc')
    results={}
    def build(name,block,template=fixture):
        folder=out/name;folder.mkdir(exist_ok=True)
        c=folder/'check.c';c.write_text(compose(block,template),encoding="utf-8");exe=folder/('check.exe' if os.name=='nt' else 'check')
        if Path(compiler).name.lower() in {'cl','cl.exe','clang-cl','clang-cl.exe'}:cmd=[compiler,'/nologo','/O2','/UNDEBUG','/I'+str(ROOT/'libs/video'),'/I'+str(ROOT/'include'),'/std:c17',str(c),'/Fo'+str(folder)+os.sep,'/Fe'+str(exe)]
        else:cmd=[compiler,'-std=c11','-O2','-UNDEBUG','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(ROOT/'libs/video'),'-I'+str(ROOT/'include'),str(c),'-o',str(exe),'-pthread']
        (folder/'command.json').write_text(json.dumps(cmd,indent=2))
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(folder/'build.log').write_text(r.stdout+r.stderr)
        assert r.returncode==0,r.stdout+r.stderr
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60);(folder/'run.log').write_text(r.stdout+r.stderr)
        results[name]={"build":0,"run":r.returncode}
        return r
    r=build('actual',prompt);print(r.stdout+r.stderr,end='')
    if r.returncode:return r.returncode
    threads=(ROOT/'libs/video/tests/test_cpu_surface_prompt_threads.c').read_text()
    r=build('threads',prompt,threads);print(r.stdout+r.stderr,end='')
    if r.returncode:return r.returncode
    mutants=[('adjacent-below-overlaps','(u64)address+bytes>s_cpu_surface_pending[i].begin','(u64)address+bytes>=s_cpu_surface_pending[i].begin'),
             ('adjacent-above-overlaps','if((u64)address<s_cpu_surface_pending[i].end','if((u64)address<=s_cpu_surface_pending[i].end'),
             ('no-submit-reset','    s_cpu_surface_pending_count=0;s_cpu_surface_pending_overflow=0;\n','    (void)0;\n'),
             ('stale-tail','    s_cpu_surface_tail=kept;\n','    (void)kept;\n'),
             ('serves-during-replay','    if(s_cpu_surface_recording)return;',''),
             ('ignores-overflow','    if(s_cpu_surface_pending_overflow)return 1;\n','')]
    mutants.append(('bypasses-snapshot-barrier','        if(request->snapshot_ranges){kept=s_cpu_surface_tail;break;}',''))
    for name,before,after in mutants:
        assert prompt.count(before)==1,(name,before)
        bad=build(name,prompt.replace(before,after))
        assert bad.returncode!=0 and 'FAIL' in bad.stderr,(name,bad.returncode,bad.stdout,bad.stderr)
        print('PASS mutant '+name+' rejected: '+bad.stderr.strip().splitlines()[-1])
    assert 'atomic_load_explicit(&s_cpu_surface_head,memory_order_acquire)' in prompt
    (out/'result.json').write_text(json.dumps({'commands':results,'mutants':[m[0] for m in mutants],'concurrent_requests':256},indent=2))
    print('PASS: prompt CPU surface service and wiring ('+str(len(mutants))+' mutants rejected)');return 0
if __name__=='__main__':sys.exit(main())
