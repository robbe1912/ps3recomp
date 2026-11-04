"""Compile production registry/rejoin lookup against authored ownership cases."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = args.source or ROOT/'runtime/spu/spu_channels.c'
    text = source.read_text()
    registry = text[text.index('typedef void (*spu_fn)(spu_context*);'):
                    text.index('/* ---- Swappable code overlays')]
    out = ROOT/'temp/checks'/('resident-lookup-'+str(time.time_ns()))
    out.mkdir(parents=True)
    fixture = r'''
#include "spu_context.h"
#include "spu_policy_overlay.h"
#include "spu_interp.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
''' + registry + r'''
/* These ownership cases are nonpolicy; matching WWS code would be a bug. */
int spu_overlay_match_sig(const uint8_t hdr[16]){(void)hdr;abort();}
static volatile unsigned called;
static void base(spu_context*c){(void)c;called=1;abort();}
static void owner(spu_context*c){(void)c;called=2;abort();}
static void other(spu_context*c){(void)c;called=3;abort();}
static void global(spu_context*c){(void)c;called=4;abort();}
static unsigned checks;
static void expect(spu_context*c,uint32_t pc,spu_fn wanted){
    spu_fn got=spu_lifted_lookup(c,pc);checks++;
    if(got!=wanted){fprintf(stderr,"ownership mismatch check=%u pc=%x\n",checks,pc);exit(1);}
}
int main(void){
    assert(base!=owner && owner!=other && base!=other);
    spu_context*a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));assert(a&&b);
    a->image_id=b->image_id=3;
    spu_begin_image(3);
    uint32_t pcs[]={0x3ffc,0x4000,0x4080,0x40fc,0x4100,0x3fffc};
    for(unsigned i=0;i<sizeof(pcs)/sizeof(*pcs);i++)spu_register_function(pcs[i],base);
    spu_begin_image(7);spu_register_function(0x4000,owner);spu_register_function(0x40fc,owner);
    spu_register_function(0x3fffc,owner);
    spu_begin_image(9);spu_register_function(0x4000,other);
    spu_begin_image(0);spu_register_function(0x200,global);
    expect(a,0x4000,base);expect(NULL,0x200,global);expect(a,0x200,global);
    a->resident_code[0].lsa=0x4000;a->resident_code[0].size=0x100;a->resident_code[0].image_id=7;
    b->resident_code[0].lsa=0x4000;b->resident_code[0].size=0x100;b->resident_code[0].image_id=9;
    expect(a,0x4000,owner);expect(b,0x4000,other);
    expect(a,0x4080,NULL); /* Owned span must not fall through to stale primary code. */
    expect(a,0x40fc,owner);expect(a,0x4100,base);expect(a,0x3ffc,base);
    a->resident_code[1]=b->resident_code[0];expect(a,0x4000,owner); /* First containing slot wins. */
    a->resident_code[0].image_id=0;expect(a,0x4000,other);
    a->resident_code[1].size=0;expect(a,0x4000,base);
    a->resident_code[0].image_id=7;a->resident_code[0].size=0x100;
    expect(a,0x4000,owner);a->resident_code[0].image_id=9;expect(a,0x4000,other);
    a->resident_code[0].image_id=7;a->resident_code[0].lsa=0x3f000;a->resident_code[0].size=0x1000;
    expect(a,0x3fffc,owner);expect(a,0x4000,base);
    /* RELATIVE_CASES */
    assert(!called);free(a);free(b);printf("PASS %u ownership checks\n",checks);return 0;
}
'''
    # Curated runtimes may additionally carry the relocation used by their
    # indirect dispatcher. Test that ABI too when it is actually present.
    if 'uint32_t rel;' in (source.parent/'spu_context.h').read_text():
        fixture=fixture.replace('/* RELATIVE_CASES */', '''
    spu_begin_image(7);spu_register_function(0x80,owner);
    spu_begin_image(3);spu_register_function(0x5080,base);spu_register_function(0x5084,base);
    a->resident_code[0].lsa=0x5000;a->resident_code[0].size=0x100;a->resident_code[0].rel=0x5000;
    expect(a,0x5080,owner);expect(a,0x5084,NULL);
''')
    (out/'fixture.c').write_text(fixture)
    compiler = os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl' if os.name=='nt' else 'cc')
    assert compiler, 'Set MV_CLANG/CC or install a C compiler'
    exe=out/('fixture.exe' if os.name=='nt' else 'fixture')
    if os.name=='nt':
        command=[compiler,'/nologo','/TC','/O2','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS','/I'+str(source.parent),'/I'+str(source.parents[2]/'include'),str(out/'fixture.c'),'/Fo'+str(out/'fixture.obj'),'/Fe:'+str(exe)]
    else:
        command=[compiler,'-std=c11','-O2','-UNDEBUG','-Wall','-Wextra','-Werror','-I'+str(source.parent),'-I'+str(source.parents[2]/'include'),str(out/'fixture.c'),'-o',str(exe)]
    result=subprocess.run(command,text=True,capture_output=True)
    (out/'build.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    result=subprocess.run([str(exe)],text=True,capture_output=True)
    (out/'run.log').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,end='');result.check_returncode()


if __name__=='__main__':
    main()
