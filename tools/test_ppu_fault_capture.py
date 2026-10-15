#!/usr/bin/env python3
"""Authored live Windows tests of the actual one-event PPU snapshot header."""
import hashlib,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if sys.platform!='win32':raise SystemExit('Windows snapshot fixture requires Windows')
out=ROOT/'temp/checks'/('ppu-fault-'+str(time.time_ns()));out.mkdir(parents=True)
source=out/'fixture.cpp'
source.write_text(r"""
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>
#include "runtime/ppu/ppu_context.h"
#include "runtime/diag/ppu_fault_capture.h"
extern void other(const ppu_context*,const unsigned char*,uint32_t(*)(uint32_t));
static uint32_t normalize(uint32_t ea){return ea==256?512:ea;}
int main(){
 SetErrorMode(SEM_NOGPFAULTERRORBOX);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
 unsigned char* vm=(unsigned char*)VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 if(!vm)return 2;for(unsigned i=0;i<4096;i++)vm[i]=(unsigned char)(i/256+17);
 unsigned char original[4096];memcpy(original,vm,4096);DWORD old;
 if(!VirtualProtect(vm+4096,4096,PAGE_NOACCESS,&old))return 3;
 ppu_context ctx={};ctx.gpr[0]=0xffffffff;ctx.gpr[3]=256;ctx.gpr[4]=4096;
 ctx.ctr=0x1234;ctx.lr=0x5678;ctx.thread_id=9;ppu_context before=ctx;
 std::vector<std::thread> workers;
 for(int i=0;i<16;i++)workers.emplace_back([&,i](){
   if(i&1)other(&ctx,vm,normalize);else ppu_fault_capture(&ctx,vm,normalize);
 });
 for(auto& t:workers)t.join();
 if(memcmp(&ctx,&before,sizeof ctx)||memcmp(vm,original,4096))return 4;
 puts("UNCHANGED");return 0;
}
""",encoding='utf-8')
second=out/'other.cpp';second.write_text('#include "runtime/ppu/ppu_context.h"\n#include "runtime/diag/ppu_fault_capture.h"\nvoid other(const ppu_context* c,const unsigned char* v,uint32_t(*n)(uint32_t)){ppu_fault_capture(c,v,n);}\n')
compiler=os.environ.get('MV_CLANG','clang-cl')
cmd=[compiler,'/nologo','/O2','/std:c++17','/EHsc','/I'+str(ROOT),str(source),str(second),'/Fe'+str(out/'fixture.exe'),'/Fo'+str(out)+'\\']
with (out/'compile.log').open('wb') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=60,cwd=out)
reports={}
for mode in ['disabled','enabled','error']:
 env=dict(os.environ);env.pop('PPU_FAULT_CAPTURE_DIR',None)
 dest=out/mode
 if mode!='error':dest.mkdir()
 if mode!='disabled':env['PPU_FAULT_CAPTURE_DIR']=str(dest)
 p=subprocess.run([out/'fixture.exe'],env=env,capture_output=True,text=True,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW)
 (out/(mode+'.log')).write_text(p.stdout+p.stderr)
 if mode=='error':assert p.returncode and '[ppu-fault-capture] FATAL create output' in p.stderr
 else:
  assert p.returncode==0 and p.stdout.strip()=='UNCHANGED'
  if mode=='disabled':assert not list(dest.iterdir())
  else:
   j=json.loads((dest/'fault.json').read_text());assert j['complete'] and j['target']==0x1234 and j['lr']==0x5678 and j['guest_tid']==9
   assert (dest/'fault-context.bin').stat().st_size==j['context_size']==1104
   rs={r['label']:r for r in j['regions']};assert len(rs)==34
   assert rs['r03-raw']['ea']==256 and rs['r03-effective']['ea']==512
   assert (dest/rs['r03-raw']['file']).read_bytes()==bytes([18])*256
   assert (dest/rs['r03-effective']['file']).read_bytes()==bytes([19])*256
   assert not rs['r04-raw']['complete'] and rs['r04-raw']['error'] and rs['r04-raw']['read']==0
   assert not rs['r00-raw']['complete'] and rs['r00-raw']['error']==534
   assert p.stderr.count('one event saved')==1
   # Exclusive files may not be overwritten by another invocation.
   again=subprocess.run([out/'fixture.exe'],env=env,capture_output=True,text=True,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW)
   assert again.returncode and '[ppu-fault-capture] FATAL create output' in again.stderr
 reports[mode]=dict(returncode=p.returncode,pass_=True)
(out/'result.json').write_text(json.dumps(dict(tests=reports,header_sha256=hashlib.sha256((ROOT/'runtime/diag/ppu_fault_capture.h').read_bytes()).hexdigest()),indent=2))
print('PASS disabled/enabled, 16 threads/two TUs, one-event cap, bytes/alias/read errors, immutable context/VM, explicit I/O errors:',out)
