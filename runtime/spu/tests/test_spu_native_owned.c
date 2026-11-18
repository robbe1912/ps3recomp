/* Actual interpreter/drain/resolver, authored guest instructions. Link-only
 * aborting boundaries are reused from the existing transition fixture. */
#define main existing_handoff_main
#include "test_spu_interpreter_handoff.c"
#undef main
#include "../spu_native_owned.h"
#include <fenv.h>
#include "spu_native_owned_call.h"
/* Report authored-fixture failures without Windows assertion dialogs. Runtime
 * assertions and error handling remain unchanged. */
#define OWNED_CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while (0)
static unsigned native_calls, native_in_irq;
static int call_policy, queued_irq, nested_owned, nested_tail;
extern void spu_wrch(spu_context*,uint32_t,u128);
extern u128 spu_rdch(spu_context*,uint32_t);
static void forbidden_policy(spu_context* c) { (void)c; fputs("FORBIDDEN compiled policy\n",stderr); exit(2); }
static void owned_job(spu_context* c) {
    ++native_calls;
    if (c->irq_saved || c->irq_frame) ++native_in_irq;
    if (queued_irq) {
        c->gpr[3]=spu_ai(c->gpr[3],3);c->pc=0x4004;
        c->event_status=1;g_spu_trampoline_fn=spu_indirect_branch;
        spu_drain_call(c,0x204);
        OWNED_CHECK(c->pc==0x204 && !c->irq_frame && !c->irq_saved);
        g_spu_trampoline_fn=spu_indirect_branch;return;
    }
    if (call_policy) {
        c->gpr[4]=c->gpr[0];
        spu_context* ctx=c;
        OWNED_POLICY_CALL;
    }
    c->gpr[3]=spu_ai(c->gpr[3],3);
    if (call_policy) c->gpr[0]=c->gpr[4];
    SPU_RET(c);
}
static void spu_func_00004008(spu_context* c) { forbidden_policy(c); }
static void owned_subjob(spu_context* c) {
    ++native_calls;
    if(c->irq_saved || c->irq_frame) ++native_in_irq;
    c->gpr[3]=spu_ai(c->gpr[3],5);
    if(nested_tail) { spu_context* ctx=c; OWNED_SUBJOB_TAIL; }
    SPU_RET_REG(c,6);
}
static spu_context reference;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    OWNED_CHECK(argc==2);const char* mode=argv[1];
    if (!strcmp(mode,"capabilities")) {
#if defined(__clang__)
        puts("musttail=1");
#else
        puts("musttail=0");
#endif
        return 0;
    }
    const int scoped_nested = !strcmp(mode, "scoped-nested");
    const int scoped_overlap = !strcmp(mode, "scoped-overlap");
    nested_tail=!strcmp(mode,"nested-tail");
    nested_owned=nested_tail || !strcmp(mode,"nested-owned") || scoped_nested;
    call_policy=nested_owned || !strcmp(mode,"call-policy");queued_irq=!strcmp(mode,"queued-irq");
    spu_context_init(&ctx,0);ctx.image_id=77;ctx.policy_mode=1;
    ctx.pc=0x200;ctx.status=SPU_STATUS_RUNNING;
    word(0x200,0x31080000); /* brasl r0,0x4000 */
    word(0x204,0x1C008183); /* ai r3,r3,2 */
    word(0x208,0x00002123); /* original stop 0x2123 */
    word(0x300,0x1C01C183); /* ai r3,r3,7 */
    word(0x304,0x35000000); /* bi r0 */
    if (call_policy) {
        ctx.gpr[2]=spu_link(0x300);
        word(0x4000,0x04000004); /* ori r4,r0,0 */
        word(0x4004,0x35200100); /* bisl r0,r2: interpreted policy helper */
        word(0x4008,0x1C00C183); /* ai r3,r3,3 */
        word(0x400C,0x04000200); /* ori r0,r4,0 */
        word(0x4010,0x35000000); /* bi r0 */
    } else {
        word(0x4000,0x1C00C183);word(0x4004,0x35000000);
    }
    if (!strcmp(mode,"irq") || queued_irq) {
        ctx.int_enable=1;ctx.event_mask=1;ctx.event_status=queued_irq?0:1;
        word(0,0x3000C000); /* bra 0x600 */
        word(0x600,0x4080008A); /* il r10,1 */
        word(0x604,0x21A0010A); /* wrch event-ack,r10 */
        word(0x608,0x31080000); /* brasl r0,0x4000 while IRQ state is saved */
        word(0x60C,0x35440000); /* enabled iret */
    }
    if (strcmp(mode,"unknown")) {
        ctx.resident_code[0].lsa=0x4000;ctx.resident_code[0].size=0x14;
        ctx.resident_code[0].image_id=25;
        if (!strcmp(mode,"invalid-range")) ctx.resident_code[0].size=SPU_LS_SIZE+1;
        if (!strcmp(mode,"overlap") || scoped_overlap) {
            ctx.resident_code[1]=ctx.resident_code[0];ctx.resident_code[0].image_id=26;
        }
    }
    if (nested_owned || !strcmp(mode,"live-explicit-stop")) {
        word(0x304,0x310A0006); /* brasl r6,0x5000; preserve outer link r0 */
        word(0x308,0x35000000); /* bi r0 */
        word(0x5000,0x1C014183); /* ai r3,r3,5 */
        word(0x5004,nested_tail?0x327E0080:0x35000300); /* br native caller / bi r6 */
        ctx.resident_code[1].lsa=0x5000;ctx.resident_code[1].size=8;
        ctx.resident_code[1].image_id=scoped_nested ? 26 : 25;
    }
    if (!strcmp(mode,"dma-invalidate")) {
        ppu_vm_size=0x20000;vm_base=calloc(1,ppu_vm_size);OWNED_CHECK(vm_base);
        const uint8_t replacement[16]={0x1C,0x02,0x41,0x83,0x35,0,0,0};
        memcpy(vm_base+0x10000,replacement,sizeof replacement);
        spu_wrch(&ctx,MFC_LSA,spu_link(0x4000));
        spu_wrch(&ctx,MFC_EAH,spu_link(0));
        spu_wrch(&ctx,MFC_EAL,spu_link(0x10000));
        spu_wrch(&ctx,MFC_Size,spu_link(sizeof replacement));
        spu_wrch(&ctx,MFC_TagID,spu_link(3));
        spu_wrch(&ctx,MFC_Cmd,spu_link(0x40)); /* actual MFC GET */
        spu_wrch(&ctx,MFC_WrTagMask,spu_link(1u<<3));
        spu_wrch(&ctx,MFC_WrTagUpdate,spu_link(2));
        OWNED_CHECK(spu_rdch(&ctx,MFC_RdTagStat)._u32[0]==(1u<<3));
        OWNED_CHECK(!memcmp(ctx.ls+0x4000,replacement,sizeof replacement));
        OWNED_CHECK(ctx.resident_code[0].image_id==0);
        /* GET and tag completion are finished; the subsequent authored code
         * accesses only LS. End the external fixture VM lifetime here. */
        free(vm_base);vm_base=NULL;ppu_vm_size=0;
    }
    spu_begin_image(77);spu_register_function(0x200,forbidden_policy);
    spu_register_function(0x204,forbidden_policy);spu_register_function(0x300,forbidden_policy);
    spu_register_function(0x4000,forbidden_policy); /* stale primary must lose */
    spu_register_function(0x600,forbidden_policy);
    spu_register_function(0x308,forbidden_policy);
    spu_begin_image(25);
    spu_register_function(0x5000,owned_subjob);
    if (strcmp(mode,"owner-miss")) spu_register_function(0x4000,owned_job);
    if (scoped_nested || scoped_overlap) {
        spu_begin_image(26);
        spu_register_function(scoped_nested ? 0x5000 : 0x4000,
                              scoped_nested ? owned_subjob : owned_job);
    }
#ifndef OWNED_HISTORICAL
    if (!strcmp(mode,"admission")) {
        OWNED_CHECK(spu_native_owned_can_run(&ctx,0x4000));
        int saved_watch=g_spu_ls_watch_n;
        g_spu_ls_watch_n=-1;OWNED_CHECK(spu_native_owned_can_run(&ctx,0x4000));
        g_spu_ls_watch_n=0;OWNED_CHECK(spu_native_owned_can_run(&ctx,0x4000));
        g_spu_ls_watch_n=1;OWNED_CHECK(!spu_native_owned_can_run(&ctx,0x4000));
        g_spu_ls_watch_n=saved_watch;
        ctx.int_enable=1;ctx.event_mask=ctx.event_status=1;
        OWNED_CHECK(!spu_native_owned_can_run(&ctx,0x4000));
        ctx.int_enable=0;ctx.irq_frame=(void*)&ctx;
        OWNED_CHECK(!spu_native_owned_can_run(&ctx,0x4000));ctx.irq_frame=NULL;
        ctx.irq_saved=1;OWNED_CHECK(!spu_native_owned_can_run(&ctx,0x4000));ctx.irq_saved=0;
        OWNED_CHECK(!spu_native_owned_can_run(&ctx,0x4001));
        OWNED_CHECK(!spu_native_owned_can_run(&ctx,SPU_LS_SIZE));
        puts("admission PASS");return 0;
    }
#endif
#ifndef OWNED_HISTORICAL
    if (!strcmp(mode,"native-call-no-frame") || !strcmp(mode,"native-call-wrong-return")) {
        ctx.resident_code[1].lsa=0x5000;ctx.resident_code[1].size=8;
        ctx.resident_code[1].image_id=25;
        word(0x5000,0x1C014183);word(0x5004,0x35000300);
        ctx.host_depth=!strcmp(mode,"native-call-wrong-return")?1:0;
        ctx.drain_ret_pc=ctx.host_depth?0x5008:0x5004;
        spu_interp_run_native_call(&ctx,0x5000,0x5004);
        OWNED_CHECK(!native_calls && ctx.pc==0x5004 && ctx.gpr[3]._u32[0]==5);
        puts("invalid native call scope retains reference PASS");return 0;
    }
#endif
    if (!strcmp(mode,"live-explicit-stop")) {
        ctx.host_depth=1;ctx.drain_ret_pc=0x5004;
        spu_interp_run_until(&ctx,0x5000,0x5004);
        OWNED_CHECK(!native_calls && ctx.pc==0x5004 && ctx.gpr[3]._u32[0]==5);
        puts("live-explicit-stop PASS");return 0;
    }
    if (!strcmp(mode,"budget")) {
        spu_interp_run_until_budget(&ctx,0x200,0,2);
        OWNED_CHECK(!native_calls && ctx.pc==0x4004 && ctx.gpr[3]._u32[0]==3);
        puts("budget PASS");return 0;
    }
    if (!strcmp(mode,"stop-boundary")) {
        spu_interp_run_until(&ctx,0x200,0x4000);
        OWNED_CHECK(!native_calls && ctx.pc==0x4000 && !ctx.gpr[3]._u32[0]);
        puts("stop-boundary PASS");return 0;
    }
    memcpy(&reference,&ctx,sizeof ctx);reference.ls=reference.ls_store;
    /* Remove only host code ownership; retain the policy registry namespace.
     * Every guest instruction then runs through the real interpreter adapter. */
    memset(reference.resident_code,0,sizeof reference.resident_code);
    OWNED_CHECK(!fesetround(FE_DOWNWARD));
    OWNED_CHECK(!feraiseexcept(FE_INVALID|FE_INEXACT));
    int rounding=fegetround(), flags=fetestexcept(FE_ALL_EXCEPT);
    if (queued_irq) {
        OWNED_CHECK(spu_interp_run_until_budget(&reference,0x200,0,2)==0x4004);
        OWNED_CHECK(reference.gpr[3]._u32[0]==3);
        reference.event_status=1; /* authored external event after the same two guest instructions */
    }
    spu_run_with_halt(forbidden_policy,&reference);
    OWNED_CHECK(fegetround()==rounding && fetestexcept(FE_ALL_EXCEPT)==flags);
#if defined(__clang__)
    if (!strcmp(mode,"musttail")) {
        extern void spu_indirect_branch_mt(spu_context*);
        ctx.pc=0x4000;ctx.gpr[0]=spu_link(0x204);
        spu_indirect_branch_mt(&ctx);SPU_DRAIN(&ctx);
    } else
#endif
    spu_run_with_halt(forbidden_policy,&ctx);
    OWNED_CHECK(fegetround()==rounding && fetestexcept(FE_ALL_EXCEPT)==flags);
    OWNED_CHECK(!memcmp(ctx.gpr,reference.gpr,sizeof ctx.gpr));
    OWNED_CHECK(!memcmp(ctx.ls,reference.ls,SPU_LS_SIZE));
    OWNED_CHECK(ctx.status==reference.status && ctx.stop_code==reference.stop_code);
    OWNED_CHECK(ctx.event_status==reference.event_status && ctx.event_mask==reference.event_mask);
    OWNED_CHECK(ctx.int_enable==reference.int_enable && ctx.irq_saved==reference.irq_saved);
    if (!strcmp(mode,"irq") || queued_irq) {
        OWNED_CHECK(ctx.event_status==0 && ctx.irq_saved==0 && ctx.int_enable==1 && ctx.srr0==(queued_irq?0x4004u:0x200u));
        OWNED_CHECK(!ctx.irq_frame);
    }
    OWNED_CHECK(ctx.gpr[3]._u32[0]==(nested_owned?17u:call_policy?12u:!strcmp(mode,"dma-invalidate")?11u:5u));
    unsigned expected=(!strcmp(mode,"owned")||!strcmp(mode,"irq")||queued_irq||!strcmp(mode,"musttail")||call_policy)?(nested_owned?2u:1u):0u;
    if (scoped_nested) expected = 1;
    if (native_calls!=expected || native_in_irq) {
        fprintf(stderr,"FAIL native calls=%u expected=%u native_in_irq=%u\n",native_calls,expected,native_in_irq);exit(1);
    }
    printf("%s PASS native=%u full GPR/LS/status/stop/FP parity\n",mode,native_calls);
    free(vm_base);vm_base=NULL;ppu_vm_size=0;
    return 0;
}
