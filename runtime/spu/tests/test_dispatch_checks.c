/* Differentially compare release transfers with the complete diagnostic hook. */
#include "../spu_drain.c"
#include <assert.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
static unsigned indirect_calls, tick_calls;
void spu_indirect_branch(spu_context* ctx) { ++indirect_calls; ctx->pc=0x400; }
void yz_lockstep_tick(spu_context* ctx) { (void)ctx; ++tick_calls; }
#ifdef _WIN32
/* The PE linker retains unrelated drain functions; fail if any are reached. */
uint64_t spu_sf_simd_enter(void) { abort(); }
void spu_sf_simd_leave(uint64_t saved) { (void)saved; abort(); }
static int setenv(const char* key, const char* value, int overwrite)
{ (void)overwrite; return _putenv_s(key, value); }
static int unsetenv(const char* key) { return _putenv_s(key, ""); }
#endif
void spu_restart_dispatch(spu_context* ctx) { (void)ctx; abort(); }
spu_drain_fn spu_lookup(uint32_t addr, int image_id)
{ (void)addr; (void)image_id; abort(); }
static jmp_buf stop;
static unsigned stack_switches;
static void switches_stack(spu_context* ctx) { (void)ctx; }
void spu_check_stack_reset(spu_context* ctx,void (*fn)(spu_context*))
{
    if(ctx->host_depth && !ctx->policy_mode && fn==switches_stack) {
        ++stack_switches;longjmp(stop,2);
    }
}
void spu_halt(spu_context* ctx) { ctx->status=0x1234;longjmp(stop,1); }
static int invoke(spu_context* ctx,int fast,void* fn)
{
    int result=setjmp(stop);
    if(!result) {
        if(fast)spu_task_launch_check(ctx,fn);
        else spu_task_launch_slow(ctx,fn);
    }
    return result;
}
static ps3_spu_execution_event observed[4];
static unsigned observed_count, called;
static uint64_t test_clock(void* user) { (void)user; return 1; }
static void test_observe(void* user, const ps3_spu_execution_event* event)
{ (void)user; assert(observed_count<4); observed[observed_count++]=*event; }
static void normal_dispatch(spu_context* ctx) { ++called; ctx->pc=0x400; }
static void scoped_test(int fast, int irq, int stack_reset)
{
    spu_context* ctx=calloc(1,sizeof *ctx); assert(ctx);
    ctx->ls=ctx->ls_store; ctx->pc=0x200; ctx->host_depth=1; ctx->image_id=1;
    if (irq) {
        /* Real interrupt resolver: BRA absolute 0x40 in the LS0 vector. */
        uint32_t word=(0x060u<<23)|(0x10u<<7);
        ctx->ls[0]=word>>24; ctx->ls[1]=word>>16;
        ctx->ls[2]=word>>8; ctx->ls[3]=word;
        ctx->int_enable=ctx->event_status=ctx->event_mask=1;
    }
    observed_count=called=indirect_calls=tick_calls=0;
    s_fast_dispatch=fast; g_spu_trampoline_fn=stack_reset?switches_stack:normal_dispatch;
    int outcome=setjmp(stop);
    if (!outcome) spu_drain_call_scoped_body(ctx,0x400);
    if (stack_reset) {
        assert(outcome==2 && observed_count==0 && called==0 && tick_calls==1);
    } else {
        assert(!outcome && ctx->pc==0x400 && ctx->steps==1 && tick_calls==1);
        assert(observed_count==1 && observed[0].kind==PS3_SPU_COMPILED_DISPATCH);
        assert(observed[0].function==(uintptr_t)(irq?spu_indirect_branch:normal_dispatch));
        assert(observed[0].target_pc==(uint32_t)(irq?0x40:0x200));
        assert(!!(observed[0].flags & PS3_SPU_REDIRECTED)==irq);
        assert(called==(unsigned)!irq && indirect_calls==(unsigned)irq);
        assert(!ctx->irq_frame && !g_spu_trampoline_fn);
    }
    free(ctx);
}
int main(void)
{
    ps3_spu_execution_observer observer={0, test_clock, test_observe, 0};
    assert(ps3_spu_execution_observer_install(&observer));
    for(int fast=0;fast<=1;++fast) for(int irq=0;irq<=1;++irq)
        scoped_test(fast,irq,0);
    for(int fast=0;fast<=1;++fast) scoped_test(fast,0,1);
    assert(ps3_spu_execution_observer_install(NULL));
    setenv("SPU_FAST_DISPATCH","1",1);
    assert(spu_fast_dispatch_init()==1);
    spu_context* a=calloc(1,sizeof *a);spu_context* b=calloc(1,sizeof *b);assert(a&&b);
    for(unsigned i=0;i<2048;++i) {
        memset(a,0,sizeof *a);
        a->pc=(i%5==0)?0:((i%7==0)?SPU_LS_SIZE:0x4000+i*4);
        a->steps=i%3==0?0:i;
        a->image_id=(i%3==0)?0:900;a->policy_mode=i&1;
        a->host_depth=(i%4);a->gpr[3]._u32[0]=0x12345678;
        memcpy(b,a,sizeof *b);
        void* fn=i%11==0?(void*)switches_stack:NULL;
        int reference=invoke(a,0,fn),candidate=invoke(b,1,fn);
        assert(reference==candidate && !memcmp(a,b,sizeof *a));
    }
    assert(stack_switches);
    memset(a,0,sizeof *a);a->pc=0x1234;a->policy_mode=1;
    g_pm_flow_ctx=a;g_pm_flow_n=0;
    assert(!invoke(a,1,NULL) && g_pm_flow_n==1 && g_pm_flow_buf[0]==0x1234);
    g_pm_flow_ctx=NULL;
    uint32_t recent[32];assert(!spu_recent_pcs(recent,32));
    /* Every existing observation/compatibility option forces the full path,
     * even a value of zero: some legacy switches use presence semantics. */
    const char* probes[]={"MOTORSTORM_WWS_WAIT_TRACE","SPU_NO_LS0_END",
        "SPU_EXITTRACE","SPU_STEPTRACE","SPU_STEPTRACE_FROM","SPU_JOBDRAIN"};
    for(unsigned i=0;i<sizeof probes/sizeof probes[0];++i) {
        setenv(probes[i],"0",1);assert(!spu_fast_dispatch_init());unsetenv(probes[i]);
    }
    assert(spu_fast_dispatch_init()==1);
    printf("PASS: 2048 identical contexts/exits, stack switches, flow capture and probe fallback\n");
    free(a);free(b);return 0;
}
