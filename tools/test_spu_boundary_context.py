#!/usr/bin/env python3
"""Authored full-context capture: missing state must fail, not become zero input."""
import json, os, re, struct, subprocess, time
from pathlib import Path
from read_spu_boundary import read_ring

ROOT = Path(__file__).resolve().parents[1]
CODE = r'''
#include "spu_boundary_trace.h"
typedef struct { uint32_t gpr[512],event_mask,dec_value; uint64_t dec_base; } state;
static state ctx; static uint8_t ls[0x40000];
static void observe(unsigned pc) {
 state before=ctx; uint8_t last=ls[0x3ffff];
#ifdef SPU_BOUNDARY_CONTEXT_V2
 if(getenv("FIXTURE_LEGACY_API"))
  spu_boundary_observe(&ctx,pc,0x12345678,ctx.gpr,ls,1);
 else spu_boundary_observe_context(&ctx,getenv("FIXTURE_BAD_SIZE")?524289u:sizeof(ctx),pc,0x12345678,ctx.gpr,ls,1);
#else
 spu_boundary_observe(&ctx,pc,0x12345678,ctx.gpr,ls,1);
#endif
 if(memcmp(&before,&ctx,sizeof(ctx)) || last!=ls[0x3ffff]) abort();
}
int main(void) {
 memset(&ctx,0xa5,sizeof(ctx)); ctx.gpr[0]=1;ctx.event_mask=0x12;
 ctx.dec_value=0x76543210;ctx.dec_base=0x123456789abcdef0ull;ls[0x3ffff]=0x34;
 observe(0x104); /* successor arriving first must not consume the bounded pair */
 observe(0x100);ctx.gpr[0]=2;ctx.event_mask=0x13;ls[0x3ffff]=0x35;observe(0x104);
 for(unsigned i=0;i<40;i++){ctx.gpr[0]=999;observe(0x100);observe(0x104);}
 return 0;
}
'''

def main():
    out=ROOT/'temp/checks'/('boundary-context-'+str(time.time_ns()));out.mkdir(parents=True)
    source=out/'fixture.c';source.write_text(CODE)
    env={k:v for k,v in os.environ.items() if not k.startswith('SPU_BOUNDARY_')}
    results=[]
    for language in ('c','c++'):
        dest=out/language.replace('+','p');dest.mkdir()
        exe=dest/('fixture.exe' if os.name=='nt' else 'fixture')
        cmd=([env.get('MV_CLANG','clang-cl'),'/nologo','/TC' if language=='c' else '/TP','/O2','/I'+str(ROOT/'runtime/spu'),str(source),'/Fo'+str(dest/'fixture.obj'),'/Fe'+str(exe)] if os.name=='nt' else [env.get('CC','cc') if language=='c' else env.get('CXX','c++'),'-x',language,'-std=c11' if language=='c' else '-std=c++17','-O2','-pthread','-I'+str(ROOT/'runtime/spu'),str(source),'-o',str(exe)])
        r=subprocess.run(cmd,env=env,capture_output=True,text=True);(dest/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
        runenv=dict(env,SPU_BOUNDARY_DIR=str(dest),SPU_BOUNDARY_PCS='0x100,0x104',SPU_BOUNDARY_CAPACITY='16',SPU_BOUNDARY_LS_START='0x100',SPU_BOUNDARY_LS_LENGTH='16',SPU_BOUNDARY_POLICY_ONLY='1',SPU_BOUNDARY_CONTEXT_LIMIT='2',SPU_BOUNDARY_FIRST_PC='0x100')
        r=subprocess.run([str(exe)],env=runenv,capture_output=True,text=True,timeout=10);(dest/'run.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
        rings=list(dest.glob('boundary-*.bin'));assert len(rings)==1
        d=read_ring(rings[0]);h=d['header'];rows=d['rows']
        assert h['format']==2, 'Missing actual context/full LS: legacy snapshot cannot satisfy replay input'
        assert not d['invalid_slots'] and not d['gaps'] and len(rows)==2
        assert [r['pc'] for r in rows]==[0x100,0x104]
        assert [struct.unpack_from('<I',r['context_bytes'],2048)[0] for r in rows]==[0x12,0x13]
        assert all(struct.unpack_from('<IQ',r['context_bytes'],2052)==(0x76543210,0x123456789abcdef0) for r in rows)
        assert [r['full_ls'][-1] for r in rows]==[0x34,0x35]
        assert [struct.unpack_from('<I',r['gpr'])[0] for r in rows]==[1,2]
        raw=bytearray(rings[0].read_bytes());struct.pack_into('<Q',raw,4096+h['commit_offset'],0)
        bad=dest/'torn.bin';bad.write_bytes(raw);assert read_ring(bad)['invalid_slots']==[0]
        for mode in ('unarmed','armed','invalid-limit','invalid-first','missing-size','oversized'):
            gate=dest/mode;gate.mkdir();checkenv=dict(runenv,SPU_BOUNDARY_DIR=str(gate))
            if mode in ('unarmed','armed'):
                arm=gate/'arm';checkenv['SPU_BOUNDARY_ARM_FILE']=str(arm)
                if mode=='armed':arm.write_text('observed checkpoint')
            if mode=='invalid-limit':checkenv['SPU_BOUNDARY_CONTEXT_LIMIT']='17'
            if mode=='invalid-first':checkenv['SPU_BOUNDARY_FIRST_PC']='0x108'
            if mode=='missing-size':checkenv['FIXTURE_LEGACY_API']='1'
            if mode=='oversized':checkenv['FIXTURE_BAD_SIZE']='1'
            check=subprocess.run([str(exe)],env=checkenv,capture_output=True,text=True,timeout=10)
            (gate/'run.log').write_text(check.stdout+check.stderr);assert check.returncode==0
            found=list(gate.glob('boundary-*.bin'))
            if mode=='armed':assert len(found)==1 and len(read_ring(found[0])['rows'])==2
            else:assert not found
            assert ('ERROR' in check.stderr)==(mode not in ('unarmed','armed'))
        results.append(dict(language=language,records=len(rows),context_bytes=h['context_bytes'],full_ls_bytes=h['full_ls_bytes']))
    # Actual interpreter: preserve nonzero event/decrementer inputs, and
    # capture the real context layout plus both sides of its authored store.
    dest=out/'actual';dest.mkdir()
    text=(ROOT/'runtime/spu/tests/spu_interp_selftest.c').read_text()
    text=text.replace('#include "../spu_interp.h"','#include "spu_interp.h"\n#include <stddef.h>')
    needle='    spu_context_init(&ctx, 0);'
    assert text.count(needle)==1
    text=text.replace(needle,needle+'\n    ctx.event_mask=0x135;ctx.decrementer=0x76543210;ctx.dec_base_ns=0x123456789abcdef0ull;\n    printf("LAYOUT %zu %zu %zu %zu %zu\\n",sizeof(ctx),offsetof(spu_context,event_mask),offsetof(spu_context,decrementer),offsetof(spu_context,dec_base_ns),offsetof(spu_context,ls_store));')
    test=dest/'selftest.c';test.write_text(text)
    exe=dest/('fixture.exe' if os.name=='nt' else 'fixture')
    includes=[str(ROOT/'include'),str(ROOT/'runtime/spu')]
    cmd=([env.get('MV_CLANG','clang-cl'),'/nologo','/TC','/O2','/UNDEBUG',*['/I'+x for x in includes],str(test),str(ROOT/'runtime/spu/spu_interp.c'),'/Fo'+str(dest)+'/', '/Fe'+str(exe)] if os.name=='nt' else [env.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-pthread',*['-I'+x for x in includes],str(test),str(ROOT/'runtime/spu/spu_interp.c'),'-lm','-o',str(exe)])
    r=subprocess.run(cmd,env=env,capture_output=True,text=True);(dest/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
    runenv=dict(env,SPU_BOUNDARY_DIR=str(dest),SPU_BOUNDARY_PCS='0,4,0x18,0x1c',SPU_BOUNDARY_CAPACITY='16',SPU_BOUNDARY_LS_START='0x100',SPU_BOUNDARY_LS_LENGTH='16',SPU_BOUNDARY_CONTEXT_LIMIT='4',SPU_BOUNDARY_FIRST_PC='0')
    r=subprocess.run([str(exe)],env=runenv,capture_output=True,text=True,timeout=10);(dest/'run.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
    layout=list(map(int,re.search(r'LAYOUT (\d+) (\d+) (\d+) (\d+) (\d+)',r.stdout).groups()))
    decoded=[read_ring(x) for x in dest.glob('boundary-*.bin')]
    d=next(d for d in decoded if [r['pc'] for r in d['rows']]==[0,4,0x18,0x1c])
    assert d['header']['context_bytes']==layout[0] and not d['invalid_slots'] and not d['gaps']
    for row in d['rows']:
        raw=row['context_bytes']
        assert raw[:2048]==row['gpr']
        assert raw[layout[4]:layout[4]+0x40000]==row['full_ls']
        assert struct.unpack_from('<I',raw,layout[1])[0]==0x135
        assert struct.unpack_from('<I',raw,layout[2])[0]==0x76543210
        assert struct.unpack_from('<Q',raw,layout[3])[0]==0x123456789abcdef0
    assert d['rows'][2]['full_ls'][0x100:0x104]==bytes(4)
    assert d['rows'][3]['full_ls'][0x100:0x104]==b'\0\0\0p'
    results.append(dict(mode='actual-interpreter',context_bytes=layout[0],records=4,nonzero_event_and_decrementer_preserved=True))
    (out/'result.json').write_text(json.dumps(results,indent=2))
    print('PASS actual writer: C/C++, raw non-GPR state, full LS, start-PC gate, freeze, torn rejection, no guest writes');print(out)

if __name__=='__main__':main()
