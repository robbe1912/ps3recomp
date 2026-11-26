/* spu_interp.h — SPU interpreter: the correctness floor for the SPU recompiler.
 *
 * Statically lifted functions are the fast path. Computed/indirect branches
 * (`bi $reg`) and code that only exists at runtime — DMA'd overlays (LBP),
 * self-modified code (spu_0004's `stqd; sync; bi $3` trampoline at LS 0xC0) —
 * have no static bytes to lift. On a lifted-function-table miss we interpret
 * from live local store instead of falling off a cliff.
 *
 * The interpreter shares the EXACT spu_context (gpr, ls, channels) the lifted
 * code uses and calls the SAME spu_<mnemonic> helpers, so control and state
 * cross the boundary transparently.
 */
#ifndef SPU_INTERP_H
#define SPU_INTERP_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A lifted SPU function: void f(spu_context*). Matches the lifter's signature. */
typedef void (*spu_lifted_fn)(spu_context*);

/* Registered lifted-function lookup by local-store entry address for the image
 * currently selected in ctx->image_id. Weak: the default returns NULL (pure
 * interpretation). The lifted image's generated spu_function_table registration
 * overrides this so computed branches reach compiled code. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

/* Interpret from `start_lsa` until the SPU stops (`stop`/`stopd`/halt) or a
 * host-fatal decode. Handles all control flow internally. Returns the stop
 * code (0 if none). ctx->pc holds the address that stopped execution. */
uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);
/* As spu_interp_run, but also hands back at stop_lsa (0 = no such point): the
 * return point of the drain the caller goes back to (spu_smc_microstep). */
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa);
/* Additional exact guest-instruction ceiling, 0=off. The existing global
 * attempted-step ceiling and interrupt policy still apply unchanged. */
uint32_t spu_interp_run_until_budget(spu_context* ctx, uint32_t start_lsa,
                                    uint32_t stop_lsa, uint64_t budget);
/* Host-armed step ceiling for differential harnesses: when nonzero,
 * spu_interp_run/until returns at the ceiling with g_spu_interp_budget_hit
 * set (host treats that as a timeout). Default 0 = off, no change. */
extern uint64_t g_spu_interp_budget;
extern int g_spu_interp_budget_hit;

/* Recoverable bail for a policy-mode lane stranded mid-excision (zero-word
 * stop in virgin LS, runtime-generated stub target): restore the TAKE-time
 * register file and resume at srr0 instead of halting. service!=0 finishes
 * the stall-and-notify service host-side first. Returns 0 when no recoverable
 * target exists (caller keeps the old halt semantics). spu_channels.c. */
int spu_pm_recoverable_bail(spu_context* ctx, uint32_t site, int service);

/* Preserve an outer profiler sample across a nested execution driver.
 * The driver captures a mark before setjmp and unwinds to it after longjmp. */
uint32_t spu_interp_stats_mark(void);
void spu_interp_stats_unwind(uint32_t mark);

/* Reports belong to the host thread that ran the interpreter. In particular,
 * publishing each instruction's PC must not contend with other SPU workers.
 * Read these on that thread after a run; they are not process-wide telemetry. */
extern SPU_THREAD_LOCAL uint32_t g_spu_interp_last_pc;
extern SPU_THREAD_LOCAL uint64_t g_spu_interp_steps;

/* The single dispatch point every computed branch routes through. If `target`
 * is a registered lifted entry, runs it; otherwise interprets from `target`.
 * The lifter emits `bi $reg` / unresolved indirect branches as a call here. */
void spu_dispatch(spu_context* ctx, uint32_t target);

#ifdef __cplusplus
}
#endif
#endif /* SPU_INTERP_H */
