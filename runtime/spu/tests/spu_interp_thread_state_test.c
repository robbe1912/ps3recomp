/* Payload-free integration test: execute authored IL/STOP programs through
 * the real interpreter, then let another host thread execute a different one.
 * Joining the worker orders every access even against the old shared globals. */
#include "../spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* The program needs no host channels or services. Fail if one is reached. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v;abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch;abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c;abort(); }
void spu_trace_pc(spu_context* c, uint32_t pc) { (void)c;(void)pc;abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t pc) { (void)c;(void)pc;return NULL; }
uint8_t* vm_base;
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch, g_spu_ls_dbg;
void spu_ls_watch_slow(uint32_t a,int w,const uint8_t* p,uint32_t pc,uint32_t lr) {
    (void)a;(void)w;(void)p;(void)pc;(void)lr;abort();
}

static atomic_int failures;
static void check(int condition,const char* message) {
    if (!condition) { fprintf(stderr,"FAIL: %s\n",message); atomic_fetch_add(&failures,1); }
}
static void be32(uint8_t* p,uint32_t value) {
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}
static void execute(uint32_t pc,uint32_t value,unsigned count) {
    spu_context* c=(spu_context*)calloc(1,sizeof(*c));
    if (!c) abort();
    c->ls=c->ls_store;c->image_id=-1;
    for (unsigned i=0;i<count;i++) be32(c->ls+pc+4*i,0x40800003u|(value<<7));
    be32(c->ls+pc+4*count,0x00002000u);
    uint8_t expected[32];memcpy(expected,c->ls+pc,sizeof(expected));
    uint32_t result=spu_interp_run(c,pc);
    check(result==0x2000u,"original STOP code");
    check(c->pc==pc+4*(count+1),"original next PC");
    check(c->status==SPU_STATUS_STOPPED_BY_STOP,"original stopped state");
    for (unsigned lane=0;lane<4;lane++) check(c->gpr[3]._u32[lane]==value,"original IL value");
    check(memcmp(expected,c->ls+pc,sizeof(expected))==0,"program bytes unchanged");
    check(g_spu_interp_last_pc==pc+4*count,"owner last PC");
    check(g_spu_interp_steps==count+1,"owner step count");
    free(c);
}
#ifdef _WIN32
static DWORD WINAPI worker(void* arg) {
#else
static void* worker(void* arg) {
#endif
    unsigned index=(unsigned)(uintptr_t)arg;
    check(g_spu_interp_last_pc==0 && g_spu_interp_steps==0,"new worker reports start empty");
    for (unsigned i=0;i<100;i++) execute(0x200+index*0x40,0x5678+index,2+index);
    return 0;
}
int main(void) {
    execute(0x100,0x1234,2);
    uint32_t* owner_pc=&g_spu_interp_last_pc;
    uint64_t* owner_steps=&g_spu_interp_steps;
#ifdef _WIN32
    HANDLE t[4];
    for (unsigned i=0;i<4;i++) {
        t[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);if (!t[i]) return 2;
    }
    if (WaitForMultipleObjects(4,t,TRUE,10000)!=WAIT_OBJECT_0) return 2;
    for (unsigned i=0;i<4;i++) CloseHandle(t[i]);
#else
    pthread_t t[4];
    for (unsigned i=0;i<4;i++) if (pthread_create(&t[i],NULL,worker,(void*)(uintptr_t)i)) return 2;
    for (unsigned i=0;i<4;i++) if (pthread_join(t[i],NULL)) return 2;
#endif
    check(*owner_pc==0x108u,"another worker overwrote the owner's last PC");
    check(*owner_steps==3,"another worker overwrote the owner's step count");
    int n=atomic_load(&failures);
    printf("interpreter thread reports: %s (%d failures, 401 programs)\n",n?"FAIL":"PASS",n);
    return n?1:0;
}
