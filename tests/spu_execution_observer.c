/* Authored control-flow streams and dependencies; no original game code. */
#include "spu_interp.h"
#include "spu_native_leaf.h"
#include "spu_native_owned.h"   /* actual owned policy used by the extracted drain body */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct spu_irq_frame {
    jmp_buf env; struct spu_irq_frame* prev; uint32_t depth; int image_id;
} spu_irq_frame;
typedef void (*spu_drain_fn)(spu_context*);
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;
static unsigned count, clocks, native_calls, calls, irq_calls;
static int rejected, zero_clock;
static unsigned return_register;
static spu_lifted_fn resolved_function;
static ps3_spu_execution_event events[32];
uint8_t* vm_base;
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch,g_spu_ls_dbg;
void spu_ls_watch_slow(uint32_t a,int w,const uint8_t*p,uint32_t pc,uint32_t lr) {
    (void)a;(void)w;(void)p;(void)pc;(void)lr;abort();
}
u128 spu_rdch(spu_context*c,uint32_t ch){(void)c;(void)ch;abort();}
void spu_wrch(spu_context*c,uint32_t ch,u128 v){(void)c;(void)ch;(void)v;abort();}
uint32_t spu_rchcnt(spu_context*c,uint32_t ch){(void)c;(void)ch;abort();}
void spu_spurs_taskset_syscall(spu_context*c){(void)c;abort();}
void spu_trace_pc(spu_context*c,uint32_t pc){(void)c;(void)pc;abort();}
spu_lifted_fn spu_lifted_lookup(const spu_context*c,uint32_t pc){(void)c;(void)pc;return resolved_function;}
void spu_depth_guard(spu_context*c){(void)c;}
int spu_tailret_enabled(void){return 0;}
void yz_lockstep_tick(spu_context*c){(void)c;}
void spu_task_launch_check(spu_context*c,void*fn){(void)c;(void)fn;}
void spu_restart_dispatch(spu_context*c){(void)c;abort();}
void spu_indirect_branch(spu_context*c){(void)c;abort();}
void (*spu_lookup(uint32_t pc,int image))(spu_context*){(void)pc;(void)image;return 0;}
static void irq(spu_context*c){++irq_calls;c->pc=0x400;}
void (*spu_take_interrupt(spu_context*c,void(*fn)(spu_context*)))(spu_context*){
    (void)fn;c->int_enable=0;c->pc=0x40;return irq;
}
/* ACTUAL_SCOPED_BODY */
void spu_check_stack_reset(spu_context*c,void(*fn)(spu_context*)){(void)c;(void)fn;}
static void resolved_tail(spu_context*ctx){
    spu_lifted_fn fn=resolved_function;
    /* ACTUAL_RESOLVED_TAIL */
}
static uint64_t clock_tick(void*user){assert(user==events);++clocks;return zero_clock?0:clocks;}
static int accept(void*user,uint32_t kind,uintptr_t ctx,uint32_t a,uint32_t b){
    assert(user==events);(void)kind;(void)ctx;(void)a;(void)b;return !rejected;
}
static void observe(void*user,const ps3_spu_execution_event*e){
    assert(user==events && count<32);events[count++]=*e;
}
static void reset(void){count=clocks=0;memset(events,0,sizeof events);g_spu_trampoline_fn=0;}
static void plain(spu_context*c){++calls;++c->gpr[7]._u32[0];SPU_RET(c);}
static void alternate(spu_context*c){++calls;SPU_RET_REG(c,return_register++);}
static void put(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void native_execute(spu_context*c,const spu_native_leaf_plan*p){++native_calls;c->pc=p->resume_pc;}
static int prepare(const spu_context*c,uint64_t remaining,spu_native_leaf_plan*p){
    (void)remaining;if(c->gpr[127]._u32[0]!=0xA55A)return 0;
    p->execute=native_execute;p->instructions=1;p->last_pc=0x100;p->resume_pc=0x200;return 1;
}
static spu_context* context(void){
    spu_context*c=calloc(1,sizeof(*c));assert(c);c->ls=c->ls_store;c->image_id=-1;
    c->pc=0x100;c->status=SPU_STATUS_RUNNING;return c;
}
static void interpreted(uint32_t instruction,uint32_t expected){
    spu_context*c=context();c->gpr[0]._u32[0]=0x200;put(c->ls+0x100,instruction);reset();
    spu_interp_run_until_budget(c,0x100,0,1);
    assert(c->pc==expected && c->status==SPU_STATUS_RUNNING);
#if PS3_EXECUTION_OBSERVATION
    assert(count==2 && events[0].kind==PS3_SPU_INTERPRETER_INVOKE);
    assert(events[1].kind==PS3_SPU_INTERPRETED_CONTROL);
    assert(events[1].source_pc==0x100 && events[1].target_pc==expected);
    assert(events[1].context==(uintptr_t)c && events[1].image==-1);
    assert(events[1].flags==PS3_SPU_SOURCE_INSTRUCTION);
#else
    assert(count==0 && clocks==0);
#endif
    free(c);
}
static void compiled(int scoped,int interrupt){
    spu_context*c=context();c->pc=0x200;c->host_depth=1;c->gpr[0]._u32[0]=0x400;
    if(interrupt)c->int_enable=c->event_status=c->event_mask=1;
    reset();g_spu_trampoline_fn=plain;
    if(scoped)spu_drain_call_scoped_body(c,0x400);else SPU_DRAIN(c);
    assert(c->pc==0x400 && c->gpr[7]._u32[0]==(uint32_t)!interrupt);
    assert(!g_spu_trampoline_fn && c->irq_frame==0);
#if PS3_EXECUTION_OBSERVATION
    assert(count==(unsigned)(interrupt?1:2));
    assert(events[0].kind==PS3_SPU_COMPILED_DISPATCH);
    assert(events[0].function==(uintptr_t)(interrupt?irq:plain));
    assert(events[0].target_pc==(uint32_t)(interrupt?0x40:0x200));
    assert(!!(events[0].flags&PS3_SPU_REDIRECTED)==interrupt);
    assert(events[0].flags&PS3_SPU_SOURCE_UNAVAILABLE);
    if(!interrupt)assert(events[1].kind==PS3_SPU_COMPILED_RETURN && events[1].target_pc==0x400);
#else
    assert(count==0 && clocks==0);
#endif
    free(c);
}
int main(void){
#if !PS3_EXECUTION_OBSERVATION
    unsigned unevaluated=0;
    PS3_SPU_EXECUTION(++unevaluated,0,0,0,0,0,0);
    assert(unevaluated==0);
#endif
    ps3_spu_execution_observer observer={events,clock_tick,observe,accept};
    assert(ps3_spu_execution_observer_install(&observer)==PS3_EXECUTION_OBSERVATION);
    spu_native_leaf leaf={0};leaf.begin=0x100;leaf.end=0x104;leaf.prepare=prepare;
    assert(!spu_native_leaf_register(&leaf));
    interpreted(0x35000000,0x200); /* BI r0 */
    interpreted(0x32002000,0x200); /* BR +0x100 */
    interpreted(0x20002003,0x200); /* BRZ r3,+0x100: zero, taken */
    interpreted(0x21002003,0x104); /* BRNZ r3,+0x100: zero, fallthrough */
    compiled(0,0);compiled(0,1);compiled(1,0);compiled(1,1);
    spu_context*c=context();c->gpr[6]._u32[0]=0x444;c->host_depth=1;
    reset();return_register=6;alternate(c);assert(c->pc==0x444 && !g_spu_trampoline_fn && return_register==7);
#if PS3_EXECUTION_OBSERVATION
    assert(count==1 && events[0].kind==PS3_SPU_COMPILED_RETURN && events[0].detail==6);
#endif
    reset();c->host_depth=0;return_register=6;alternate(c);
    assert(c->pc==0x444 && g_spu_trampoline_fn==spu_indirect_branch && return_register==7);
    memset(c->gpr,0,sizeof c->gpr);put(c->ls+0x100,0x32002000);c->gpr[127]._u32[0]=0xA55A;
    reset();spu_interp_run_until_budget(c,0x100,0,1);
    assert(native_calls==1 && c->pc==0x200);
#if PS3_EXECUTION_OBSERVATION
    assert(count==3 && events[1].kind==PS3_SPU_NATIVE_BEGIN && events[2].kind==PS3_SPU_NATIVE_END);
    assert(events[1].source_pc==0x100 && events[2].source_pc==0x100 && events[2].target_pc==0x200);
    assert(events[1].detail==1 && events[2].detail==1);
#endif
    rejected=1;reset();c->gpr[127]._u32[0]=0;
    spu_interp_run_until_budget(c,0x100,0,1);assert(c->pc==0x200 && count==0 && clocks==0);
    rejected=0;zero_clock=1;reset();spu_interp_run_until_budget(c,0x100,0,1);
#if PS3_EXECUTION_OBSERVATION
    assert(count==2 && (events[0].flags&PS3_SPU_CLOCK_INVALID));
#endif
    zero_clock=0;resolved_function=plain;
    for(int tail=0;tail<2;++tail){
        reset();c->pc=0x200;c->gpr[0]._u32[0]=0x400;c->host_depth=1;
        if(tail)resolved_tail(c);else spu_dispatch(c,0x200);
        assert(c->pc==0x400 && c->gpr[7]._u32[0]==(uint32_t)(tail+1));
#if PS3_EXECUTION_OBSERVATION
        assert(count==2 && events[0].function==(uintptr_t)plain);
        assert(events[0].flags&PS3_SPU_RESOLVED_TARGET);
#endif
    }
    assert(ps3_spu_execution_observer_install(0));free(c);
    assert(calls==6 && irq_calls==2);
    puts("PASS actual interpreted/native/macro/scoped execution; outputs and native admission retained");
    return 0;
}
