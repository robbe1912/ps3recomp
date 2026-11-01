#ifndef NATIVE_ATOMIC_OBSERVER_SUPPORT_H
#define NATIVE_ATOMIC_OBSERVER_SUPPORT_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ps3emu/native_atomic_observer.h"
#include "runtime/spu/spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern int fixture_lock_held, fixture_lock_opens, fixture_notifications;
extern int fixture_rewrite_on_unlock;
extern unsigned g_spu_putllc_count, g_spu_putllc_sync_hit;
extern uint32_t g_barrier_sync_watch;
extern void (*g_spu_line_commit_hook)(uint32_t);
extern void (*g_spu_host_line_commit_hook)(uint32_t);
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t);
void spu_taskset_line_committed(uint32_t);
void spu_lockguard_check(spu_context*,uint32_t,const uint8_t*,uint32_t,const char*);
int mfc_ea_range_committed(uint32_t,uint32_t);
void yz_lockstep_tick(spu_context*);
void spu_coh_reserve(spu_context*,uint32_t);
int issue219_atomic(spu_context*,uint32_t);
int ppu_stwcx32(uint64_t,uint32_t,uint32_t);
int ppu_stdcx64(uint64_t,uint64_t,uint64_t);
void issue219_ppu_setup(uint32_t,int,int);
int issue219_ppu_reservation_lost(void);
uint32_t issue219_ppu_breaks(void);
int issue219_ppu_lock_held(void);
void issue219_ppu_no_context(void);
#ifdef __cplusplus
}
#endif

/* Channel constants are values from the public production DMA header. */
#define MFC_ATOMIC_LINE 128
#define MFC_GETLLAR_CMD 0xD0
#define MFC_PUTLLC_CMD 0xB4
#define MFC_PUTLLUC_CMD 0xB0
#define MFC_PUTQLLUC_CMD 0xB8
#endif
