/* Authored IL/BI leaf called by BRSL, not an extracted program. */
#include "../spu_interp.h"
#include "../spu_native_leaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <signal.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
typedef unsigned HostFP;
static HostFP get_fp(void) { return _mm_getcsr(); }
static void set_fp(HostFP s) { _mm_setcsr(s); }
static void mode_fp(HostFP s,unsigned mode) { set_fp((s&~0x6000u)|(mode<<13)|0x20u); }
static int same_fp(HostFP a,HostFP b) { return a==b; }
#elif defined(__aarch64__)
typedef struct { uint64_t control,status; } HostFP;
static HostFP get_fp(void) {
    HostFP s;
    __asm__ volatile("mrs %0, fpcr" : "=r"(s.control));
    __asm__ volatile("mrs %0, fpsr" : "=r"(s.status));
    return s;
}
static void set_fp(HostFP s) {
    __asm__ volatile("msr fpcr, %0" : : "r"(s.control) : "memory");
    __asm__ volatile("msr fpsr, %0" : : "r"(s.status) : "memory");
}
static void mode_fp(HostFP s,unsigned mode) {
    s.control=(s.control&~(3ull<<22))|((uint64_t)mode<<22);
    s.status|=1u<<4;set_fp(s);
}
static int same_fp(HostFP a,HostFP b) { return a.control==b.control && a.status==b.status; }
#else
#error Host FP verification needs x86 or AArch64
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

u128 spu_rdch(spu_context* c,uint32_t ch) { (void)c;(void)ch;abort(); }
void spu_wrch(spu_context* c,uint32_t ch,u128 v) { (void)c;(void)ch;(void)v;abort(); }
uint32_t spu_rchcnt(spu_context* c,uint32_t ch) { (void)c;(void)ch;abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c;abort(); }
void spu_trace_pc(spu_context* c,uint32_t pc) { (void)c;(void)pc;abort(); }
/* Interrupt delivery is a host-service boundary these leaf tests never reach:
 * owned policy is off (SPU_NATIVE_OWNED_JOBS unset), so abort visibly if it is. */
void (*spu_take_interrupt(spu_context* c,void (*fn)(spu_context*)))(spu_context*) { (void)c;(void)fn;abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context* c,uint32_t pc) { (void)c;(void)pc;return NULL; }
uint8_t* vm_base;
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch,g_spu_ls_dbg;
void spu_ls_watch_slow(uint32_t a,int w,const uint8_t* p,uint32_t pc,uint32_t lr) {
    (void)a;(void)w;(void)p;(void)pc;(void)lr;abort();
}
extern unsigned leaf_test_environment_reads(void);
void spu_trace_dump(uint32_t at);
static atomic_int enabled,calls,worker_ready;
static int bad,nested,options;
static uint8_t shared_ls[0x40000];
static void require(int ok,const char* why) {
    if (!ok) { fprintf(stderr,"FAIL %s\n",why); exit(1); }
}
static void be32(uint8_t* p,uint32_t w) {
    p[0]=(uint8_t)(w>>24);p[1]=(uint8_t)(w>>16);p[2]=(uint8_t)(w>>8);p[3]=(uint8_t)w;
}
static uint32_t word(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static spu_context* program(unsigned value) {
    spu_context* c=(spu_context*)calloc(1,sizeof(*c)); require(c!=NULL,"allocation");
    c->ls=c->ls_store;c->image_id=0;
    for (unsigned r=0;r<128;r++) for(unsigned lane=0;lane<4;lane++)
        c->gpr[r]._u32[lane]=0x12345678u+17*r+lane;
    be32(c->ls+0x100,0x33002000u); /* BRSL r0,+0x100 */
    be32(c->ls+0x104,0x00002000u); /* STOP 0x2000 */
    be32(c->ls+0x200,0x40800003u|(value<<7)); /* IL r3,value */
    be32(c->ls+0x204,0x35000000u); /* BI r0 */
    if (options&1) c->list_stall_mask=1;
    if (options&2) { c->int_enable=1;c->event_status=c->event_mask=2; }
    if (options&4) { memcpy(shared_ls,c->ls,0x40000);c->ls=shared_ls; }
    if (options&8) be32(c->ls+0x204,0x00002001u);
    if (options&16) c->image_id=-1;
    if (options&64) {
        be32(c->ls+0x100,0x33000000u|(0xffc0u<<7));
        be32(c->ls,0x40800003u|(value<<7));be32(c->ls+4,0x35000000u);
    }
    return c;
}
static void execute(spu_context* c,const spu_native_leaf_plan* plan) {
    atomic_fetch_add(&calls,1);
    if (nested) {
        int before=atomic_load(&calls);
        spu_context* inner=program(42);
        require(spu_interp_run(inner,0x100)==0x2000,"nested real interpreter");
        require(atomic_load(&calls)==before,"nested native execution declined");free(inner);
    }
    for(unsigned lane=0;lane<4;lane++) c->gpr[3]._u32[lane]=42;
    c->pc=plan->resume_pc+(bad ? 4 : 0);
}
static int prepare(const spu_context* c,uint64_t remaining,spu_native_leaf_plan* plan) {
    (void)remaining; /* runtime must also enforce a too-large returned plan */
    if (!atomic_load(&enabled) || word(c->ls+c->pc)!=(0x40800003u|(42u<<7)) ||
        word(c->ls+c->pc+4)!=0x35000000u) return 0;
    plan->instructions=bad==2 ? 0 : 2;plan->last_pc=c->pc+4;
    plan->resume_pc=c->gpr[0]._u32[0]&0x3fffcu;plan->execute=execute;
    return 1;
}
static int also_prepare(const spu_context* c,uint64_t r,spu_native_leaf_plan* p) {
    if(c->pc!=0x200) return 0; /* distinct accepting guard, including under ICF */
    return prepare(c,r,p);
}
static void compare(unsigned value,uint32_t stop,uint64_t budget,int expected_calls) {
    spu_context* ref=program(value),*actual=program(value);
    HostFP csr=get_fp();
    atomic_store(&enabled,0);
    uint32_t result=spu_interp_run_until_budget(ref,0x100,stop,budget);
    uint64_t steps=g_spu_interp_steps;uint32_t last=g_spu_interp_last_pc;
    atomic_store(&enabled,1);int before=atomic_load(&calls);
    uint32_t got=spu_interp_run_until_budget(actual,0x100,stop,budget);
    require(atomic_load(&calls)-before==expected_calls,"real host execution count");
    require(got==result && g_spu_interp_steps==steps && g_spu_interp_last_pc==last,"stop/budget/count/last-PC parity");
    actual->ls=ref->ls;
    require(!memcmp(actual,ref,sizeof(*ref)),"complete context/memory parity");
    require(same_fp(get_fp(),csr),"caller FP state");free(ref);free(actual);
}
#ifdef _WIN32
static DWORD WINAPI worker(void* unused) {
#else
static void* worker(void* unused) {
#endif
    (void)unused;
    atomic_fetch_add(&worker_ready,1);
    while(atomic_load(&worker_ready)!=4) {}
    for(unsigned i=0;i<100;i++) {
        spu_context* c=program(42);
        require(spu_interp_run(c,0x100)==0x2000,"worker stop");
        require(g_spu_interp_steps==4 && c->gpr[3]._u32[0]==42,"worker state/steps");free(c);
    }
    return 0;
}
static void option(const char* name,const char* value) {
#ifdef _WIN32
    require(!_putenv_s(name,value ? value : ""),"test option update");
#else
    require(!(value ? setenv(name,value,1) : unsetenv(name)),"test option update");
#endif
}
static void aborted(int sig) { (void)sig; _Exit(86); }
int main(int argc,char** argv) {
    signal(SIGABRT,aborted);
    const char* mode=argc>1 ? argv[1] : "normal";
    if (!strcmp(mode,"empty")) { compare(42,0,0,0);puts("PASS empty registry unchanged");return 0; }
    if(!strcmp(mode,"prefreeze_trace")) {
        option("SPU_CALLTRACE","0");spu_trace_dump(0x200);
    }
    spu_native_leaf leaf={0x200,0x208,prepare};
    require(spu_native_leaf_register(NULL)==-1,"invalid registration");
    require(spu_native_leaf_register(&leaf)==0,"registration");
    require(spu_native_leaf_register(&leaf)==-1,"duplicate registration");
    spu_native_leaf zero={0,8,prepare};require(!spu_native_leaf_register(&zero),"zero entry registration");
    if(!strcmp(mode,"prefreeze_trace")) {
        compare(42,0,0,0);puts("PASS prefreeze trace retains registration");return 0;
    }
    if (!strcmp(mode,"cold_workers")) { atomic_store(&enabled,1);goto workers; }
    if (!strcmp(mode,"ambiguous")) {
        leaf.prepare=also_prepare;require(!spu_native_leaf_register(&leaf),"second guard");
    }
    if (!strcmp(mode,"bad")) bad=1;
    if (!strcmp(mode,"plan")) bad=2;
    if (bad || !strcmp(mode,"ambiguous")) {
        atomic_store(&enabled,1);spu_context* c=program(42);spu_interp_run(c,0x100);
        require(0,"invalid execution was accepted");
    }
    if (!strcmp(mode,"instrumented")) {
        compare(42,0,0,0);
        const char* value=getenv("NATIVE_TEST_STARTUP_OPTION");
        if(value) {
            char startup[64];require(strlen(value)<sizeof(startup),"test option name");
            strcpy(startup,value);option(startup,NULL);compare(42,0,0,0);
            if(!strcmp(startup,"SPU_CALLTRACE")) spu_trace_dump(0x200);
        }
        puts("PASS instrumented stream retained");return 0;
    }
    if (!strcmp(mode,"lookups")) {
        compare(42,0,0,1);compare(43,0,0,0);
        unsigned reads=leaf_test_environment_reads();
        require(reads>=7,"startup option reads observed");
        option("SPU_BOUNDARY_DIR","1");option("SPU_CALLTRACE","1");
        spu_trace_dump(0x200);
        for(unsigned i=0;i<200;i++) { compare(42,0,0,1);compare(43,0,0,0); }
        require(leaf_test_environment_reads()==reads,"no warmed native-option environment reads");
        option("SPU_BOUNDARY_DIR",NULL);option("SPU_CALLTRACE",NULL);
        puts("PASS no warmed environment scans across accepted/rejected operations");return 0;
    }
    for(unsigned rounding=0;rounding<4;rounding++) {
        HostFP saved=get_fp();mode_fp(saved,rounding);
        compare(42,0,0,1);compare(43,0,0,0);
        compare(42,0x200,0,0);compare(42,0x204,0,0);compare(42,0x104,0,1);
        for(unsigned limit=1;limit<=5;limit++) compare(42,0,limit,limit>=3 ? 1 : 0);
        set_fp(saved);
    }
    nested=1;compare(42,0,0,1);nested=0;
    g_spu_ls_watch_n=1;compare(42,0,0,0);g_spu_ls_watch_n=0;
    for(options=1;options<=8;options*=2) compare(42,0,0,0);
    options=16;compare(42,0,0,1);compare(43,0,0,0);
    options=64;compare(42,0,0,1);compare(43,0,0,0);
    options=0;
    spu_context* c=program(42);c->int_enable=1;c->event_mask=c->event_status=2;
    int before=atomic_load(&calls);spu_interp_run(c,0x100);
    require(atomic_load(&calls)==before && c->event_status==2,"pending event retained");free(c);
    require(spu_native_leaf_register(&leaf)==-1,"frozen registry");
workers:
    ;
    int worker_calls=atomic_load(&calls);
#ifdef _WIN32
    HANDLE threads[4];
    for(unsigned i=0;i<4;i++) { threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);require(threads[i]!=NULL,"thread create"); }
    require(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0,"thread join");
    for(unsigned i=0;i<4;i++) CloseHandle(threads[i]);
#else
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++) require(!pthread_create(&threads[i],NULL,worker,NULL),"thread create");
    for(unsigned i=0;i<4;i++) require(!pthread_join(threads[i],NULL),"thread join");
#endif
    require(atomic_load(&calls)-worker_calls==400,"all workers executed real replacements");
    if(!strcmp(mode,"cold_workers")) require(leaf_test_environment_reads()==7,"racing first callers capture options once");
    puts("PASS actual interpreter native leaves: complete state, guards, stops, budgets, nesting, events, FP and workers");return 0;
}
