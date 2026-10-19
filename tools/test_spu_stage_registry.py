"""Execute production staged-module registry against authored, replaceable bytes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include "spu_workload.h"
#include "spu_interp.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void native_a(spu_context* c) { c->gpr[3]._u32[0] = 77; }
static void native_b(spu_context* c) { c->gpr[3]._u32[0] = 88; }
static unsigned stale_calls, slow_calls;
static void stale(spu_context* c) { (void)c; ++stale_calls; }
spu_lifted_fn spu_lookup(uint32_t pc, int image) {
    if (image==93 && (pc==0x15000 || pc==0x15038 || pc==0x1503c || pc==0x15044)) return native_a;
    if (image==94 && pc==0x17038) return native_b;
    if (image==95 && pc==0x17038) return native_a;
    return image==91 ? stale : NULL;
}
void spu_indirect_branch_mt(spu_context*);
void spu_indirect_branch(spu_context* c) {
    ++slow_calls;
    spu_lifted_fn fn=spu_lifted_lookup(c,c->pc);
    if (fn) fn(c);
}
int main(void) {
    spu_context* a = calloc(1, sizeof *a);
    spu_context* b = calloc(1, sizeof *b);
    assert(a && b);
    spu_context_init(a,0); spu_context_init(b,1);
    uint8_t bytes[32];
    for (unsigned i=0;i<sizeof bytes;++i) bytes[i]=(uint8_t)(i*7+3);
    const uint32_t base=0x15000, second=0x17000, unknown=0x19000;
    const uint32_t segment=0x30, entry=0x38;
    const uint64_t fp=spu_workload_fingerprint(bytes,sizeof bytes);
    spu_workload_register_img_at(fp,native_a,93,base,"authored-native");
    assert(spu_workload_img_base(fp)==base);
    int image=0;
    spu_lifted_entry_fn fn=spu_workload_find_img(fp,&image);
    assert(fn && image==93); fn(a); assert(a->gpr[3]._u32[0]==77);
    spu_workload_register_img_at(fp,native_b,94,second,"authored-replacement");
    assert(spu_workload_img_base(fp)==second);
    fn=spu_workload_find_img(fp,&image);
    assert(fn && image==94); fn(b); assert(b->gpr[3]._u32[0]==88);
    spu_workload_register_img(fp,native_a,93,"base-zero");
    assert(spu_workload_img_base(fp)==0);
    assert(spu_workload_img_base(fp^1)==0);

    spu_workload_register_stage_probe(fp,92,0,entry,segment,sizeof bytes,"relative");
    spu_workload_register_stage_probe(fp,93,base,entry,segment,sizeof bytes,"native");
    spu_workload_register_stage_probe(fp,93,base,entry+4,segment,sizeof bytes,"helper");
    spu_workload_register_stage_probe(fp,94,second,entry,segment,sizeof bytes,"second");
    memcpy(a->ls+base+segment,bytes,sizeof bytes);
    uint32_t found=0,lift=0,span=0;
    assert(spu_workload_stage_probe(a->ls,base+entry,&found,&image,&lift,&span));
    assert(found==base && image==93 && lift==base && span==segment+sizeof bytes);
    assert(spu_workload_stage_probe(a->ls,base+entry+4,&found,&image,&lift,&span));
    assert(image==93 && found==base);
    /* A success in a different context must never authorize these bytes. */
    assert(!spu_workload_stage_probe(b->ls,base+entry,0,0,0,0));
    a->ls[base+segment]^=1;
    assert(!spu_workload_stage_probe(a->ls,base+entry,0,0,0,0));
    memcpy(a->ls+base+segment,bytes,sizeof bytes);
    memcpy(a->ls+second+segment,bytes,sizeof bytes);
    assert(spu_workload_stage_probe(a->ls,second+entry,&found,&image,&lift,&span));
    assert(found==second && image==94 && lift==second);
    memcpy(a->ls+unknown+segment,bytes,sizeof bytes);
    assert(spu_workload_stage_probe(a->ls,unknown+entry,&found,&image,&lift,&span));
    assert(found==unknown && image==92 && lift==0); /* requires runtime fallback */
    assert(!spu_workload_stage_probe(0,base+entry,0,0,0,0));
    assert(!spu_workload_stage_probe(a->ls,SPU_LS_SIZE,0,0,0,0));
    assert(!spu_workload_stage_probe(a->ls,SPU_LS_SIZE-4,0,0,0,0));
    found=0xFFFFFFFFu; image=-1;
    assert(!spu_workload_stage_probe(b->ls,base+entry,&found,&image,0,0));
    assert(found==0xFFFFFFFFu && image==-1);
    /* Execute the actual shared lookup and production musttail dispatcher.
     * A competing primary image deliberately has a stale native entry. */
    a->image_id=91; b->image_id=91;
    spu_native_note_write(a,base+segment,sizeof bytes);
    fn=spu_lifted_lookup(a,base+entry);
    assert(fn==native_a); fn(a); assert(a->gpr[3]._u32[0]==77);
    assert(!spu_lifted_lookup(a,base)); /* Unverified header cannot run natively. */
    assert(!spu_lifted_lookup(a,SPU_LS_SIZE));
    assert(spu_lifted_lookup(a,base+entry+12)==native_a);
    a->pc=base+entry+12; spu_indirect_branch_mt(a);
    assert(!stale_calls && !slow_calls);
    spu_native_note_write(b,base+segment,sizeof bytes);
    assert(!spu_lifted_lookup(b,base+entry));
    /* Same-source replacement and ordinary LS stores invalidate ownership. */
    a->ls[base+segment]^=1;
    spu_native_note_write(a,base+segment,1);
    assert(!spu_lifted_lookup(a,base+entry));
    a->pc=base+entry; spu_indirect_branch_mt(a);
    assert(!stale_calls && slow_calls==1);
    memcpy(a->ls+base+segment,bytes,sizeof bytes);
    spu_native_note_write(a,base+segment,sizeof bytes);
    assert(spu_lifted_lookup(a,base+entry)==native_a);
    spu_ls_write32(a,base+segment,0);
    assert(!spu_lifted_lookup(a,base+entry));
    memcpy(a->ls+base+segment,bytes,sizeof bytes);
    spu_native_note_write(a,base+segment,sizeof bytes);
    assert(spu_lifted_lookup(a,base+entry)==native_a);
    spu_native_note_write(a,second+segment,sizeof bytes);
    a->pc=second+entry; spu_indirect_branch_mt(a);
    assert(a->gpr[3]._u32[0]==88 && !stale_calls);
    spu_native_note_write(a,unknown+segment,sizeof bytes);
    assert(!spu_lifted_lookup(a,unknown+entry));
    assert(!spu_lifted_lookup(a,unknown+entry+12));
    /* Direct initialization and mutation outside the entry instruction must
     * also prevent stale-primary fallback. Restore via a helper entry. */
    spu_context_init(b,1); b->image_id=91;
    memcpy(b->ls+base+segment,bytes,sizeof bytes);
    assert(spu_lifted_lookup(b,base+entry)==native_a);
    b->ls[base+segment]^=1;
    spu_native_note_write(b,base+segment,1);
    assert(!spu_lifted_lookup(b,base+entry));
    memcpy(b->ls+base+segment,bytes,sizeof bytes);
    spu_native_note_write(b,base+segment,sizeof bytes);
    assert(spu_lifted_lookup(b,base+entry+4)==native_a);
    /* Complete payload metadata also supports fragmented real DMA, without
     * needing an entry-specific stage probe or hashing unrelated data DMA. */
    uint8_t payload[96], replacement[96];
    for (unsigned i=0;i<sizeof payload;++i) payload[i]=(uint8_t)(i*11+2);
    memcpy(replacement,payload,sizeof payload); replacement[0]^=1;
    uint64_t payload_fp=spu_workload_fingerprint(payload,sizeof payload);
    uint64_t replacement_fp=spu_workload_fingerprint(replacement,sizeof replacement);
    spu_workload_register_img_at(payload_fp,native_b,94,second,"dma-payload");
    spu_workload_register_img_at(replacement_fp,native_a,95,second,"dma-replacement");
    spu_context_init(b,1); b->image_id=91;
    memcpy(b->ls+second,payload,32);
    spu_workload_stage_note_dma(b,0x120000,second,32);
    memcpy(b->ls+second+32,payload+32,64);
    spu_workload_stage_note_dma(b,0x120020,second+32,64);
    b->pc=second+entry; spu_indirect_branch_mt(b);
    assert(b->gpr[3]._u32[0]==88);
    memcpy(b->ls+second,replacement,sizeof replacement);
    spu_workload_stage_note_dma(b,0x120000,second,sizeof replacement);
    b->pc=second+entry; spu_indirect_branch_mt(b);
    assert(b->gpr[3]._u32[0]==77);
    assert(!stale_calls);
    free(a); free(b);
    puts("PASS native registry, shared lookup, musttail execution, ownership, overwrite rejection and bounds");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--historical', action='store_true',
                        help='require unchanged pre-fix missing-API control to fail compilation')
    parser.add_argument('--historical-ref', default='c2545c5',
                        help='pre-fix source revision used only for the historical control')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='spu-stage-registry-') as directory:
        work = Path(directory)
        selected = ROOT / 'runtime/spu'
        registry = selected / 'spu_native_registry.c'
        if args.historical:
            selected = work / 'control/runtime/spu'
            selected.mkdir(parents=True)
            for name in ('spu_workload.c', 'spu_workload.h'):
                data = subprocess.check_output(['git','show',f'{args.historical_ref}:runtime/spu/{name}'],cwd=ROOT)
                (selected/name).write_bytes(data)
            registry = selected / 'spu_workload.c'
        source = work/'fixture.c'
        source.write_text(FIXTURE)
        extra_sources = []
        if not args.historical:
            channels = (ROOT/'runtime/spu/spu_channels.c').read_text()
            begin = channels.index('spu_lifted_fn spu_lifted_lookup(')
            end = channels.index('/* ---- Swappable code overlays',begin)
            lookup = work/'lookup.c'
            lookup.write_text('#include "spu_workload.h"\n#include "spu_interp.h"\n'
                              'typedef spu_lifted_fn spu_fn;\n'
                              'spu_fn spu_lookup(uint32_t,int);\n'+channels[begin:end])
            extra_sources = [str(lookup),str(ROOT/'runtime/spu/spu_dispatch_mt.c')]
        executable = work/('fixture.exe' if os.name == 'nt' else 'fixture')
        includes = [selected, ROOT/'runtime/spu', ROOT/'include', ROOT/'runtime/platform']
        if os.name == 'nt':
            compiler = os.environ.get('MV_CLANG') or shutil.which('clang-cl')
            if not compiler:
                raise SystemExit('Set MV_CLANG to clang-cl in the Visual Studio environment')
            command = [compiler,'/nologo','/std:c17','/O1','/UNDEBUG',
                       *[f'/I{p}' for p in includes],str(source),str(registry),*extra_sources,
                       f'/Fe{executable}']
        else:
            compiler = os.environ.get('CC','clang')
            command = [compiler,'-std=gnu17','-O1','-UNDEBUG',
                       *[f'-I{p}' for p in includes],str(source),str(registry),*extra_sources,
                       '-pthread','-o',str(executable)]
        built = subprocess.run(command,cwd=work,text=True,capture_output=True)
        diagnostics = built.stdout + built.stderr
        if args.historical:
            assert built.returncode != 0 and 'spu_workload_register_stage_probe' in diagnostics, diagnostics
            print('PASS historical control rejects missing staged native-module API')
            return
        if built.returncode:
            raise SystemExit(diagnostics)
        ran = subprocess.run([str(executable)],cwd=work,text=True,capture_output=True,timeout=15)
        if ran.returncode:
            raise SystemExit(ran.stdout + ran.stderr + f'\nfixture exit {ran.returncode}')
        if ran.stderr:
            print(ran.stderr.strip())
        print(ran.stdout.strip())


if __name__ == '__main__':
    main()
