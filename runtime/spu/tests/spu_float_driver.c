/* Authored value-semantics driver; input contains no guest instructions. */
#include <stdio.h>
#include "runtime/spu/spu_helpers.h"
#ifdef SPU_FLOAT_INTERPRETER
#include "runtime/spu/spu_interp.h"
#include <stdlib.h>
/* These CPU-only instructions must never enter external channel/probe hooks. */
u128 spu_rdch(spu_context* c,uint32_t ch) { (void)c;(void)ch;abort(); }
void spu_wrch(spu_context* c,uint32_t ch,u128 v) { (void)c;(void)ch;(void)v;abort(); }
uint32_t spu_rchcnt(spu_context* c,uint32_t ch) { (void)c;(void)ch;abort(); }
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c,uint32_t a) { (void)c;(void)a;abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c;abort(); }
void spu_ls_watch_slow(uint32_t l,int w,const uint8_t* p,uint32_t pc,uint32_t lr) {
    (void)l;(void)w;(void)p;(void)pc;(void)lr;abort();
}
static spu_context ctx;
static void instruction(unsigned offset,uint32_t word) {
    for (int i=0;i<4;++i) ctx.ls[offset+i]=(uint8_t)(word >> (24-8*i));
}
static u128 interpreted(unsigned op,u128 a,u128 b,u128 c) {
    static const uint32_t rr[]={0x2c4,0x2c5,0x2c6,0,0,0,0x3d4};
    static const uint32_t rrr[]={0,0,0,0xe,0xf,0xd};
    ctx.gpr[2]=a;ctx.gpr[3]=b;ctx.gpr[4]=c;
    unsigned pc=0x100;
    if (op==7) {
        /* Authored zero-normalization chain: frsqest r3,r2; fi r4,r2,r3;
         * fm r5,r2,r4. Inputs in this case are zero/denormal. */
        instruction(pc,(0x1b9u<<21)|(2u<<7)|3u);pc+=4;
        instruction(pc,(0x3d4u<<21)|(3u<<14)|(2u<<7)|4u);pc+=4;
        instruction(pc,(0x2c6u<<21)|(4u<<14)|(2u<<7)|5u);pc+=4;
    } else {
        if (op>6) abort();
        instruction(pc,op>=3 && op<=5 ? (rrr[op]<<28)|(5u<<21)|(3u<<14)|(2u<<7)|4u
                                                   : (rr[op]<<21)|(3u<<14)|(2u<<7)|5u);
        pc+=4;
    }
    instruction(pc,0);spu_interp_run(&ctx,0x100);
    if (ctx.status!=SPU_STATUS_STOPPED_BY_STOP || ctx.pc!=pc+4) abort();
    return ctx.gpr[5];
}
#endif

int main(void) {
    unsigned op;
    u128 a, b, c, r;
#ifdef SPU_FLOAT_INTERPRETER
    spu_context_init(&ctx,0);ctx.image_id=-1;
#endif
    while (scanf("%u", &op) == 1) {
        for (int i=0; i<4; ++i) if (scanf("%x", &a._u32[i]) != 1) return 2;
        for (int i=0; i<4; ++i) if (scanf("%x", &b._u32[i]) != 1) return 2;
        for (int i=0; i<4; ++i) if (scanf("%x", &c._u32[i]) != 1) return 2;
#ifdef SPU_FLOAT_INTERPRETER
        r=interpreted(op,a,b,c);
#else
        switch (op) {
        case 0: r=spu_fa(a,b); break;
        case 1: r=spu_fs(a,b); break;
        case 2: r=spu_fm(a,b); break;
        case 3: r=spu_fma(a,b,c); break;
        case 4: r=spu_fms(a,b,c); break;
        case 5: r=spu_fnms(a,b,c); break;
        case 6: r=spu_fi(a,b); break;
        case 7: r=spu_fm(a,spu_fi(a,spu_frsqest(a))); break;
        default: return 3;
        }
#endif
        printf("%08x %08x %08x %08x\n",r._u32[0],r._u32[1],r._u32[2],r._u32[3]);
    }
    return ferror(stdin) || ferror(stdout);
}
