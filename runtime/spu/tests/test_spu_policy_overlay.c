#include "spu_context.h"
#include "spu_interp.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "policy_overlay_production.inc"

static unsigned checks, failures;
static volatile unsigned called;
static void primary(spu_context* c) { (void)c; called=1; abort(); }
static void first(spu_context* c) { (void)c; called=2; abort(); }
static void second(spu_context* c) { (void)c; called=3; abort(); }
static void task(spu_context* c) { (void)c; called=4; abort(); }
static void owner(spu_context* c) { (void)c; called=5; abort(); }
static void expect(const char* name, int value) {
    ++checks;
    if (!value) { ++failures; fprintf(stderr, "FAIL %s\n", name); }
}
static void base(spu_context* c, uint32_t value) {
    for (unsigned i=0;i<4;i++) c->ls[0x1320+i]=(uint8_t)(value>>(24-i*8));
}
static void stage(spu_context* c, const uint8_t sig[16]) {
    base(c,0x4000); memcpy(c->ls+0x4000,sig,16);
}
int main(void) {
    spu_fn distinct[]={primary,first,second,task,owner};
    for(unsigned i=0;i<5;i++) for(unsigned j=i+1;j<5;j++)
        expect("distinct registry function pointers",distinct[i]!=distinct[j]);
    const uint8_t a[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const uint8_t b[16]={21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36};
    const uint8_t unknown[16]={0};
    spu_context* c=calloc(1,sizeof(*c)); assert(c); c->ls=c->ls_store;
    spu_overlay_register_sig(a,21);spu_overlay_register_sig(b,22);
    for (unsigned id=2;id<=3;id++) {
        spu_begin_image((int)id);
        spu_register_function(0x4000,primary);spu_register_function(0x4080,primary);
        spu_register_function(0x3ffc,primary);spu_register_function(0x3e000,primary);
    }
    spu_begin_image(37);spu_register_function(0x4000,primary);
    spu_begin_image(21);spu_register_function(0x4000,first);spu_register_function(0x4080,first);
    spu_begin_image(22);spu_register_function(0x4000,second);
    spu_begin_image(91);spu_register_function(0x4000,task);
    spu_begin_image(92);spu_register_function(0x4000,owner);
    c->image_id=3;c->policy_mode=1;c->resident_ovl=22;stage(c,a);
    expect("policy image3 ignores stale primary",resolve_for_test(c,0x4000)==first);
    expect("policy image3 refreshes overlay",c->resident_ovl==21);
    c->image_id=37;
    expect("arbitrary policy image",resolve_for_test(c,0x4000)==first);
    c->image_id=2;
    expect("policy image2 ignores stale primary",resolve_for_test(c,0x4000)==first);
    c->image_id=3;stage(c,b);
    expect("changed header replaces previous hit",resolve_for_test(c,0x4000)==second);
    expect("changed overlay identity",c->resident_ovl==22);
    stage(c,unknown);
    expect("unknown header blocks stale primary",resolve_for_test(c,0x4000)==NULL);
    expect("unknown header retires stale overlay",c->resident_ovl==0);
    stage(c,b);
    expect("known overlay miss blocks stale primary",resolve_for_test(c,0x4080)==NULL);
    stage(c,a);c->resident_task=91;
    expect("resident task keeps priority",resolve_for_test(c,0x4000)==task);
    c->resident_code[0].lsa=0x4000;c->resident_code[0].size=0x100;c->resident_code[0].image_id=92;
    expect("resident code keeps priority",resolve_for_test(c,0x4000)==owner);
    expect("resident owner miss stays a miss",resolve_for_test(c,0x4080)==NULL);
    c->resident_code[0].size=0;c->resident_task=0;
    c->policy_mode=0;c->image_id=2;c->resident_ovl=22;
    expect("nonpolicy image2 primary unchanged",resolve_for_test(c,0x4000)==primary);
    expect("nonpolicy overlay unchanged",c->resident_ovl==22);
    /* No primary function exists here: old numerical-image admission still fires. */
    expect("nonpolicy image2 cannot identify WWS",resolve_for_test(c,0x4084)==NULL);
    expect("nonpolicy missing entry preserves overlay",c->resident_ovl==22);
    c->policy_mode=1;c->image_id=3;c->resident_ovl=22;
    expect("below job address preserves primary",resolve_for_test(c,0x3ffc)==primary);
    expect("below job address preserves overlay",c->resident_ovl==22);
    expect("upper job boundary preserves primary",resolve_for_test(c,0x3e000)==primary);
    base(c,SPU_LS_SIZE-16);memcpy(c->ls+SPU_LS_SIZE-16,a,16);
    expect("last complete header is readable",resolve_for_test(c,0x4000)==first);
    base(c,SPU_LS_SIZE-15);
    expect("partial header rejected",resolve_for_test(c,0x4000)==NULL);
    base(c,SPU_LS_SIZE);
    expect("end address rejected",resolve_for_test(c,0x4000)==NULL);
    base(c,UINT32_MAX-7);
    expect("overflowing base rejected",resolve_for_test(c,0x4000)==NULL);
    expect("invalid base retires stale overlay",c->resident_ovl==0);
    c->ls=NULL;
    expect("missing LS rejects policy overlay",resolve_for_test(c,0x4000)==NULL);
    c->policy_mode=0;
    expect("nonpolicy does not read missing LS",resolve_for_test(c,0x4000)==primary);
    expect("lookup never executes a body",called==0);
    free(c);printf("POLICY_OVERLAY checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
