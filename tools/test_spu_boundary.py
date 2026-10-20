#!/usr/bin/env python3
"""Compile and run actual boundary writer; check wrapping, filtering and torn slots."""
import hashlib,json,os,struct,subprocess,sys,time
from pathlib import Path
from read_spu_boundary import read_ring
ROOT=Path(__file__).resolve().parents[1]
CODE=r'''
#include "spu_boundary_trace.h"
static uint8_t ls[0x40000];static uint32_t regs[512];static int owners[34];
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg){
#else
static void* worker(void* arg){
#endif
 uint32_t g[512]={0};unsigned base=(unsigned)(uintptr_t)arg;
 for(unsigned i=1;i<=40;i++){g[0]=base+i;spu_boundary_observe(&owners[0],0x100,0x12345678,g,ls,1);}return 0;
}
int main(int argc,char**argv){
 if(argc>1&&!strcmp(argv[1],"threads")){
#ifdef _WIN32
 HANDLE t[2];t[0]=CreateThread(NULL,0,worker,(void*)1000,0,NULL);t[1]=CreateThread(NULL,0,worker,(void*)2000,0,NULL);WaitForMultipleObjects(2,t,TRUE,INFINITE);CloseHandle(t[0]);CloseHandle(t[1]);
#else
 pthread_t t[2];pthread_create(t,NULL,worker,(void*)1000);pthread_create(t+1,NULL,worker,(void*)2000);pthread_join(t[0],NULL);pthread_join(t[1],NULL);
#endif
 }else{
  spu_boundary_observe(&owners[2],0x100,0,regs,ls,0);
  for(unsigned i=1;i<=40;i++){regs[0]=i;ls[0x100]=(uint8_t)i;spu_boundary_observe(&owners[0],0x100,0x12345678,regs,ls,1);spu_boundary_observe(&owners[0],0x104,0xffffffff,regs,ls,1);}
  for(unsigned i=1;i<=5;i++){regs[0]=1000+i;spu_boundary_observe(&owners[1],0x100,0x12345678,regs,ls,1);}
 }
 return 0;
}
'''
def main():
    out=ROOT/'temp/checks'/('spu-boundary-'+str(time.time_ns()));out.mkdir(parents=True)
    env={k:v for k,v in os.environ.items() if not k.startswith('SPU_BOUNDARY_')}
    source=out/'fixture.c';source.write_text(CODE)
    results=[]
    for language in ('c','c++'):
        exe=out/('fixture-'+language.replace('+','p')+('.exe' if os.name=='nt' else ''))
        cmd=([env.get('MV_CLANG','clang-cl'),'/nologo','/TC' if language=='c' else '/TP','/O1','/I'+str(ROOT/'runtime/spu'),str(source),'/Fo'+str(out/(exe.stem+'.obj')),'/Fe'+str(exe)] if os.name=='nt' else [env.get('CC','cc') if language=='c' else env.get('CXX','c++'),'-x',language,'-std=c11' if language=='c' else '-std=c++17','-O1','-pthread','-I'+str(ROOT/'runtime/spu'),str(source),'-o',str(exe)])
        build=subprocess.run(cmd,capture_output=True,text=True);(out/(exe.stem+'-build.log')).write_text(build.stdout+build.stderr)
        assert build.returncode==0,build.stdout+build.stderr
        for mode in ('disabled','normal','threads','invalid','mismatch','word'):
            dest=out/(exe.stem+'-'+mode);dest.mkdir()
            runenv=dict(env)
            if mode!='disabled':runenv.update(SPU_BOUNDARY_DIR=str(dest),SPU_BOUNDARY_PCS='0x100,' if mode=='invalid' else '0x100',SPU_BOUNDARY_CAPACITY='16',SPU_BOUNDARY_LS_START='0x100',SPU_BOUNDARY_LS_LENGTH='16',SPU_BOUNDARY_POLICY_ONLY='1')
            if mode=='mismatch':runenv.update(SPU_BOUNDARY_MATCH_LS='0x100',SPU_BOUNDARY_MATCH_BE32='0xffffffff')
            if mode=='word':runenv.update(SPU_BOUNDARY_MATCH_LS='0x100',SPU_BOUNDARY_MATCH_BE32='0x12000000')
            run=subprocess.run([str(exe),mode],env=runenv,capture_output=True,text=True,timeout=20)
            (dest/'run.log').write_text(run.stdout+run.stderr);assert run.returncode==0
            rings=list(dest.glob('boundary-*.bin'))
            if mode in ('disabled','invalid','mismatch'):
                assert not rings
                assert ('ERROR' in run.stderr)==(mode=='invalid')
                continue
            assert 'ERROR' not in run.stderr
            decoded=[read_ring(p) for p in rings];assert all(not d['invalid_slots'] and not d['gaps'] for d in decoded)
            if mode=='word':
                assert len(decoded)==1 and len(decoded[0]['rows'])==1
                assert struct.unpack_from('<I',decoded[0]['rows'][0]['gpr'])[0]==18
                assert decoded[0]['header']['match_enabled']==1 and decoded[0]['header']['match_value']==0x12000000
            elif mode=='threads':
                assert len(decoded)==1 and [r['sequence'] for r in decoded[0]['rows']]==list(range(65,81))
                assert all(1001<=struct.unpack_from('<I',r['gpr'])[0]<=1040 or 2001<=struct.unpack_from('<I',r['gpr'])[0]<=2040 for r in decoded[0]['rows'])
            else:
                assert sorted(len(d['rows']) for d in decoded)==[5,16]
                d=next(d for d in decoded if len(d['rows'])==16)
                assert d['overwritten_before_first']==24
                assert [r['sequence'] for r in d['rows']]==list(range(25,41))
                for r in d['rows']:
                    assert r['pc']==0x100 and r['instruction']==0x12345678 and struct.unpack_from('<I',r['gpr'])[0]==r['sequence'] and r['ls'][0]==r['sequence']
                raw=bytearray(rings[decoded.index(d)].read_bytes());h=d['header'];offset=4096+((40-1)%16)*h['record_size']+h['commit_offset'];struct.pack_into('<Q',raw,offset,0)
                bad=dest/'torn.bin';bad.write_bytes(raw);assert read_ring(bad)['invalid_slots']==[(40-1)%16]
            results.append(dict(language=language,mode=mode,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()))
    # Exercise the real interpreter with the existing authored CPU program.
    # Removing just the hook still executes that program but loses the required
    # boundary evidence: a red-capable check of the actual integration seam.
    actual=ROOT/'runtime/spu/spu_interp.c';text=actual.read_bytes()
    hook=b'    spu_boundary_observe_context(ctx, sizeof(*ctx), pc, insn, ctx->gpr, ctx->ls, ctx->policy_mode);'
    assert text.count(hook)==1
    for baseline in (True,False):
        name='actual-without-hook' if baseline else 'actual-interpreter'
        dest=out/name;dest.mkdir();src=dest/'spu_interp.c';src.write_bytes(text.replace(hook,b'',1) if baseline else text)
        exe=dest/('fixture.exe' if os.name=='nt' else 'fixture')
        includes=['-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu')]
        test=ROOT/'runtime/spu/tests/spu_interp_selftest.c'
        cmd=([env.get('MV_CLANG','clang-cl'),'/nologo','/TC','/O1','/UNDEBUG',*['/I'+x[2:] for x in includes],str(test),str(src),'/Fo'+str(dest)+'/', '/Fe'+str(exe)] if os.name=='nt' else [env.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-pthread',*includes,str(test),str(src),'-lm','-o',str(exe)])
        build=subprocess.run(cmd,capture_output=True,text=True);(dest/'build.log').write_text(build.stdout+build.stderr);assert build.returncode==0,build.stdout+build.stderr
        runenv=dict(env,SPU_BOUNDARY_DIR=str(dest),SPU_BOUNDARY_PCS='0,4,0x18,0x1c',SPU_BOUNDARY_CAPACITY='16',SPU_BOUNDARY_LS_START='0x100',SPU_BOUNDARY_LS_LENGTH='16')
        run=subprocess.run([str(exe)],env=runenv,capture_output=True,text=True,timeout=20);(dest/'run.log').write_text(run.stdout+run.stderr);assert run.returncode==0,run.stdout+run.stderr
        rings=list(dest.glob('boundary-*.bin'))
        if baseline:assert not rings
        else:
            decoded=[read_ring(p) for p in rings]
            assert all(not d['invalid_slots'] and not d['gaps'] for d in decoded)
            candidates=[d for d in decoded if [r['pc'] for r in d['rows']]==[0,4,0x18,0x1c]]
            assert len(candidates)==1;rows=candidates[0]['rows']
            assert [r['pc'] for r in rows]==[0,4,0x18,0x1c]
            assert struct.unpack_from('<I',rows[0]['gpr'],2*16)[0]==0
            assert struct.unpack_from('<I',rows[1]['gpr'],2*16)[0]==5
            assert struct.unpack_from('<I',rows[2]['gpr'],4*16)[0]==112
            assert rows[2]['ls']==bytes(16) and rows[3]['ls'][:4]==b'\0\0\0p'
        results.append(dict(mode=name,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),architectural_program_passed=True,required_boundary_evidence=not baseline))
    (out/'result.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256((ROOT/'runtime/spu/spu_boundary_trace.h').read_bytes()).hexdigest(),interpreter_sha256=hashlib.sha256(text).hexdigest(),results=results,scope='Actual diagnostic writer and real interpreter; authored state, CPU program and threads; no game inputs'),indent=2))
    print('PASS boundary ring C/C++, filtering, wrapping, concurrency, torn rejection; actual interpreter hook red/green and before/after LS store');print(out)
if __name__=='__main__':main()
