/* Native policy-module services through the actual interpreter, drain and
 * owned gate. Authored guest instructions only: a policy module at LS 0x200,
 * a resident job at LS 0x4000 that calls a policy service at LS 0x300 with
 * bisl, and the same service bytes as the "pristine text" in an authored
 * guest buffer. Lifted bodies are the lifter's own output
 * (spu_pm_services_call.h). Each mode compares every GPR, LS, status and stop
 * code with a run whose job and service are both interpreted. */
#define FIXTURE_REAL_SPU_ALIAS 1
#define main existing_handoff_main
#include "spu_interpreter_handoff.c"
#undef main
#include "spu_native_owned.h"
#include "spu_pm_services_call.h"
extern void spu_wrch(spu_context*,uint32_t,u128);
extern u128 spu_rdch(spu_context*,uint32_t);
#define PM_CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while (0)
static unsigned job_calls, svc_calls;
static int tail_jump, gate_inside;
static void forbidden_policy(spu_context* c) { (void)c; fputs("FORBIDDEN compiled policy\n",stderr); exit(2); }
static void owned_job(spu_context* c) {
    ++job_calls;
    spu_context* ctx=c;
    if (tail_jump) { ctx->gpr[3]=spu_ai(ctx->gpr[3],3); PM_JOB_TAIL; }
    c->gpr[4]=c->gpr[0];
    if (gate_inside) c->irq_saved=1;   /* the owned gate refuses this state */
    PM_JOB_CALL;
    if (gate_inside) c->irq_saved=0;
    c->gpr[3]=spu_ai(c->gpr[3],3);
    c->gpr[0]=c->gpr[4];
    SPU_RET(c);
}
/* A second resident job that enters the service by a tail jump (bi r2). */
static void tail_job(spu_context* c) {
    ++job_calls;
    spu_context* ctx=c;
    ctx->gpr[3]=spu_ai(ctx->gpr[3],3);
    PM_JOB_TAIL;
}
/* Compiled service: the lifter's translation of LS 0x300..0x30C. */
static void service(spu_context* c) {
    ++svc_calls;
    spu_context* ctx=c;
    PM_SVC_300; PM_SVC_304; PM_SVC_308; PM_SVC_30C;
}
/* A compiled body that disagrees with the guest text (ai 8 instead of 7). */
static void wrong_service(spu_context* c) {
    ++svc_calls;
    spu_context* ctx=c;
    PM_SVC_300; PM_SVC_304; ctx->gpr[3]=spu_ai(ctx->gpr[3],8); PM_SVC_30C;
}
static const uint32_t service_words[4]={
    0x21A00B03, /* wrch MFC_WrTagMask,r3 */
    0x01A00605, /* rdch r5,MFC_RdTagMask */
    0x1C01C183, /* ai r3,r3,7 */
    0x35000000, /* bi r0 */
};
static uint64_t fnv(const uint8_t* p,uint32_t n){uint64_t h=1469598103934665603ULL;for(uint32_t i=0;i<n;i++)h=(h^p[i])*1099511628211ULL;return h;}
static spu_context reference;
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    PM_CHECK(argc==2);const char* mode=argv[1];
    /* Authored guest memory: the pristine service text at EA 0x10000. */
    ppu_vm_size=0x20000;vm_base=calloc(1,ppu_vm_size);PM_CHECK(vm_base);
    for (int i=0;i<4;++i) {
        uint8_t* p=vm_base+0x10000+4*i;
        p[0]=service_words[i]>>24;p[1]=service_words[i]>>16;p[2]=service_words[i]>>8;p[3]=service_words[i];
    }
    static const uint32_t entries[1]={0x300};
    static const uint32_t code[1][2]={{0x300,0x310}};
    spu_pm_services services={77,0x300,0x10,0x10000,0x10,fnv(vm_base+0x10000,0x10),entries,1,code,1};
    if (!strcmp(mode,"fp-mismatch")) services.text_fnv^=1;
    spu_pm_services_register(&services);   /* copies the text while guest memory exists */
    /* Other runtime diagnostics assume a fully backed 32-bit guest space; end
     * the authored guest memory before any guest instruction runs. */
    free(vm_base);vm_base=NULL;ppu_vm_size=0;
    if (!strcmp(mode,"env")) { printf("%d\n",g_spu_pm_svc_on); return 0; }

    tail_jump=!strcmp(mode,"tail-jump");gate_inside=!strcmp(mode,"gate");
    spu_context_init(&ctx,0);ctx.image_id=77;ctx.policy_mode=1;
    ctx.pc=0x200;ctx.status=SPU_STATUS_RUNNING;ctx.gpr[2]=spu_link(0x300);ctx.gpr[3]=spu_link(0x10);
    word(0x200,0x31080000); /* brasl r0,0x4000: the job */
    word(0x204,0x1C008183); /* ai r3,r3,2 */
    word(0x208,0x00002123); /* stop 0x2123 */
    if (!strcmp(mode,"scope")) {
        /* After the first job's native service call has returned, a second
         * job enters the service by a jump: that entry must stay interpreted. */
        word(0x204,0x310A0000); /* brasl r0,0x5000 */
        word(0x208,0x1C008183); /* ai r3,r3,2 */
        word(0x20C,0x00002123); /* stop 0x2123 */
        word(0x5000,0x1C00C183); /* ai r3,r3,3 */
        word(0x5004,0x35000100); /* bi r2 */
        ctx.resident_code[1].lsa=0x5000;ctx.resident_code[1].size=8;ctx.resident_code[1].image_id=25;
    }
    for (int i=0;i<4;++i) word(0x300+4*i,service_words[i]);
    if (!strcmp(mode,"text-differs")) word(0x308,0x1C024183); /* ai r3,r3,9 in LS only */
    word(0x4000,0x04000004); /* ori r4,r0,0 */
    word(0x4004,tail_jump?0x35000100:0x35200100); /* bi r2 / bisl r0,r2 */
    if (!strcmp(mode,"wrong-call-opcode")) word(0x4004,0x35000100);
    word(0x4008,0x1C00C183); /* ai r3,r3,3 */
    word(0x400C,0x04000200); /* ori r0,r4,0 */
    word(0x4010,0x35000000); /* bi r0 */
    if (tail_jump) { word(0x4000,0x1C00C183); word(0x4004,0x35000100); }
    ctx.resident_code[0].lsa=0x4000;ctx.resident_code[0].size=0x14;ctx.resident_code[0].image_id=25;
    /* short-span: the call's return (0x4008) lies outside the resident span */
    if (!strcmp(mode,"short-span")) ctx.resident_code[0].size=0x8;
    spu_begin_image(77);
    spu_register_function(0x200,forbidden_policy);spu_register_function(0x204,forbidden_policy);
    spu_register_function(0x208,forbidden_policy);
    spu_register_function(0x300,!strcmp(mode,"verify-mismatch")?wrong_service:service);
    spu_begin_image(25);spu_register_function(0x4000,owned_job);spu_register_function(0x5000,tail_job);

    memcpy(&reference,&ctx,sizeof ctx);reference.ls=reference.ls_store;
    memset(reference.resident_code,0,sizeof reference.resident_code);   /* everything interpreted */
    if (!strcmp(mode,"wrong-call-opcode")) {
        reference.ls[0x4005] = 0x20;
    }
    spu_run_with_halt(forbidden_policy,&reference);
    if (!strcmp(mode,"wrong-call-opcode")) reference.ls[0x4005] = 0;
    PM_CHECK(!job_calls && !svc_calls);
    uint64_t before[SPU_PM_SERVICES_COUNTERS];spu_pm_services_counters(before);
    spu_run_with_halt(forbidden_policy,&ctx);
    uint64_t after[SPU_PM_SERVICES_COUNTERS];spu_pm_services_counters(after);
    uint64_t d[SPU_PM_SERVICES_COUNTERS];
    for (int i=0;i<SPU_PM_SERVICES_COUNTERS;++i) d[i]=after[i]-before[i];
    PM_CHECK(!memcmp(ctx.gpr,reference.gpr,sizeof ctx.gpr));
    PM_CHECK(!memcmp(ctx.ls,reference.ls,SPU_LS_SIZE));
    PM_CHECK(ctx.status==reference.status && ctx.stop_code==reference.stop_code);
    PM_CHECK(ctx.int_enable==reference.int_enable && ctx.irq_saved==reference.irq_saved);
    /* r3 starts at 0x10; the service stores r3 as the tag mask and reads it back into r5 */
    unsigned r3=!strcmp(mode,"scope")?0x26u:!strcmp(mode,"text-differs")?0x1Eu:0x1Cu;
    unsigned r5=!strcmp(mode,"scope")?0x1Du:tail_jump?0x13u:0x10u;
    PM_CHECK(ctx.gpr[3]._u32[0]==r3 && ctx.gpr[5]._u32[0]==r5);
    /* expected compiled-service calls and counters (native, ok, bad, incomplete, overflow, differs, not a job call) */
    unsigned want_svc=0; uint64_t want[SPU_PM_SERVICES_COUNTERS]={0};
    int on=g_spu_pm_svc_on;
    if (!strcmp(mode,"native") || !strcmp(mode,"scope")) { want_svc=1; want[0]=1; }
    else if (!strcmp(mode,"wrong-call-opcode")) { want[6]=1; }
    else if (!strcmp(mode,"verify")) { want_svc=1; want[1]=1; }
    else if (!strcmp(mode,"verify-mismatch")) { want_svc=1; want[2]=1; }
    else if (!strcmp(mode,"text-differs")) want[5]=1;
    else if (!strcmp(mode,"short-span")) want[6]=1;
    /* tail-jump: entered by a branch, not a call: refused before the counters */
    else if (!strcmp(mode,"off") || !strcmp(mode,"gate") || !strcmp(mode,"fp-mismatch") || tail_jump) {}
    else { fprintf(stderr,"unknown mode %s\n",mode); return 2; }
    PM_CHECK(job_calls==(!strcmp(mode,"scope")?2u:1u));
    if (svc_calls!=want_svc || memcmp(d,want,sizeof want)) {
        fprintf(stderr,"FAIL %s: service calls=%u expected=%u counters=%llu,%llu,%llu,%llu,%llu,%llu,%llu on=%d\n",mode,svc_calls,want_svc,
                (unsigned long long)d[0],(unsigned long long)d[1],(unsigned long long)d[2],(unsigned long long)d[3],
                (unsigned long long)d[4],(unsigned long long)d[5],(unsigned long long)d[6],on);
        exit(1);
    }
    printf("%s PASS service=%u full GPR/LS/status/stop parity\n",mode,svc_calls);
    return 0;
}
