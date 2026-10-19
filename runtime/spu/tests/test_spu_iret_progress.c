/* A repeated interrupt return address does not establish guest idleness. */
#include "spu_helpers.h"
#include "spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-only instructions never reach these external channel/diagnostic hooks. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}


static void be32(uint8_t* p, uint32_t w) { p[0]=(uint8_t)(w>>24);p[1]=(uint8_t)(w>>16);p[2]=(uint8_t)(w>>8);p[3]=(uint8_t)w; }
/* No channel, interrupt injection, lift, memory-store, or recovery hook is
 * executed by this CPU-only program. Trap if that premise changes. */
uint8_t* vm_base;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile unsigned g_spu_iret_bails;
void (*spu_take_interrupt(spu_context* c, void (*fn)(spu_context*)))(spu_context*) { (void)c;(void)fn;abort(); }
int spu_walkin_lift(uint32_t p,int i,int r) { (void)p;(void)i;(void)r;abort(); }
void spu_indirect_branch(spu_context* c) { (void)c;abort(); }
int spu_pm_recoverable_bail(spu_context* c,uint32_t p,int s) { (void)c;(void)p;(void)s;abort(); }
void spu_pm_fill_note(spu_context* c,uint32_t l,const void* b,const char* s) { (void)c;(void)l;(void)b;(void)s;abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void spu_coh_notify_write(uint32_t a) { (void)a;abort(); }
static int parked;
void spu_halt(spu_context* ctx) { (void)ctx; parked=1; }
int main(void) {
    spu_context c; memset(&c,0,sizeof c);
    c.ls=(uint8_t*)calloc(1,SPU_LS_SIZE); assert(c.ls);
    c.image_id=-1; c.status=SPU_STATUS_RUNNING; c.srr0=0x100;
    /* Authored productive loop: count to eight, returning through the same
     * architectural continuation seven times. Equal return PCs are not idle. */
    be32(c.ls+0x100,0x1c000000u|(1u<<14)|(80u<<7)|80u); /* ai r80,r80,1 */
    be32(c.ls+0x104,0x7c000000u|(8u<<14)|(80u<<7)|81u); /* ceqi r81,r80,8 */
    be32(c.ls+0x108,0x21000000u|(3u<<7)|81u);          /* brnz r81,0x114 */
    be32(c.ls+0x10c,0x35400000u);                      /* iret */
    be32(c.ls+0x114,0);                                /* stop 0 */
    for(unsigned pass=0;pass<16 && !parked;++pass) {
        spu_interp_run(&c,pass?c.pc:0x100);
        if(c.status & (SPU_STATUS_STOPPED_BY_STOP|SPU_STATUS_STOPPED_BY_HALT))break;
    }
    int ok=!parked && c.gpr[80]._u32[0]==8 && c.status==SPU_STATUS_STOPPED_BY_STOP && c.pc==0x118;
    printf("productive IRET: counter=%u pc=%x status=%x parked=%d %s\n",c.gpr[80]._u32[0],c.pc,c.status,parked,ok?"PASS":"FAIL");
    free(c.ls);return !ok;
}
