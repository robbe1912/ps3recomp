/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_jobmod_identity.h"
#include "spu_native_owned.h"
#include "spu_dma.h"
#include "ps3emu/native_atomic_observer.h"
#include "ps3emu/spu_host_line_commit.h"
void spu_halt(spu_context*); /* Policy trampolines can precede its definition. */
#include "spu_coherency.h" /* lock-line lock + PPU-write coherence bitmap */
#include "spu_helpers.h"   /* spu_splat_u32 / spu_ls_read128 (SMC microstep) */
#include "spu_lockstep.h"
#include "spu_interp.h"    /* spu_lifted_fn / spu_lifted_lookup -- defined below */
#include "spu_workload.h"  /* registry lookup for [pm-jobmod] staged spans */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
/* ---------------------------------------------------------------------------
 * Slow-quantum guest-read ring (round-5 diagnostics; no behavior change).
 *
 * A wedged PM quantum burns 50-174 ms polling guest memory that no jobmod
 * ever produced. Keep the LAST 64 guest reads from [0x40000000..0x50000000)
 * as (ea, first BE word) pairs: GET-family DMA (plain and every list
 * element, via mfc_do_transfer) plus GETLLAR. spurs_policy.c arms the ring
 * around each spu_run_with_halt and dumps it when one quantum exceeds
 * 50 ms or 200 iret bails ([pm-slow]). Concurrent lanes share the ring, so
 * entries can bleed across lanes. Acceptable for a diagnostic.
 * ------------------------------------------------------------------------ */
volatile unsigned g_spu_iret_bails;     /* total interp iret bails (spu_interp.c) */
volatile unsigned g_spu_jobmod_staged;  /* [pm-jobmod] images staged (this file) */
#define SPU_GREAD_N 64
static struct { uint32_t ea; uint32_t val; } s_gread[SPU_GREAD_N];
static volatile unsigned s_gread_i, s_gread_cnt, s_gread_armed;
void spu_gread_arm(void)    { s_gread_i = 0; s_gread_cnt = 0; s_gread_armed = 1; }
void spu_gread_disarm(void) { s_gread_armed = 0; }
void spu_gread_note(uint32_t ea)
{
    if (!s_gread_armed || ea < 0x40000000u || ea >= 0x50000000u) return;
    extern uint8_t* vm_base;
    if (!vm_base) return;
    const uint8_t* m = vm_base + ea;
    uint32_t v = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) |
                 ((uint32_t)m[2] << 8) | m[3];
    unsigned i = s_gread_i & (SPU_GREAD_N - 1);
    s_gread[i].ea = ea; s_gread[i].val = v;
    s_gread_i = (i + 1) & (SPU_GREAD_N - 1);
    if (s_gread_cnt < SPU_GREAD_N) s_gread_cnt++;
}
void spu_gread_dump(const char* tag, uint32_t wid, uint32_t spu)
{
    fprintf(stderr, "%s guest reads wid=%u spu=%u (last %u in "
                    "[0x40000000..0x50000000), oldest first):\n",
            tag, wid, spu, s_gread_cnt);
    unsigned start = (s_gread_i + SPU_GREAD_N - s_gread_cnt) & (SPU_GREAD_N - 1);
    for (unsigned k = 0; k < s_gread_cnt; k++) {
        unsigned i = (start + k) & (SPU_GREAD_N - 1);
        fprintf(stderr, " %08X=%08X", s_gread[i].ea, s_gread[i].val);
        if ((k & 3) == 3) fprintf(stderr, "\n");
    }
    if (s_gread_cnt & 3) fprintf(stderr, "\n");
    fflush(stderr);
}
#include <setjmp.h>
#ifndef _WIN32
#include <sched.h>   /* sched_yield */
#endif
#include <time.h>
#include "../platform/win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Opt-in SPU job dump. The definitions are in this file because each build
 * links it; the two hooks add no link dependency to the headers. */
#include "spu_job_dump.inc"

/* The SPU decrementer ticks at the PS3 timebase, 79.8 MHz -- the same clock
 * sys_time_get_timebase_frequency reports to the PPU. Titles calibrate real
 * delays against it, so the rate has to be right, not merely non-zero. */
#define SPU_DECREMENTER_HZ  79800000ull

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* Split the divide to keep the multiply from overflowing: QPC counters are
     * large enough that (count * 1e9) wraps a u64 within hours of uptime. */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull
           / (uint64_t)s_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
/* Called after an SPU atomic commits a 128-byte line, with the lock-line lock
 * released. cellSpurs sets it so a PPU blocked in cellSpursEventFlagWait wakes
 * when a task sets the flag, instead of on its next 2 ms poll. */
void (*g_spu_line_commit_hook)(uint32_t line) = 0;
/* Independent host waiter, installed only while SPU producers are quiescent.
 * It must remain valid until all producers have stopped. */
void (*g_spu_host_line_commit_hook)(uint32_t line) = 0;
/* SPU_LS_WATCH=0x1BE80[,0x927D80...]: watch up to 4 16-byte LS lines (reads
 * and writes) in every SPU image. See spu_ls_watch_hit2 for the fast path. */
#define SPU_WATCH_MAX 4
int g_spu_ls_watch_n = -1;
static unsigned s_spu_ls_watch[SPU_WATCH_MAX];
unsigned* spu_ls_watch_list(int* out_n)
{
    if (g_spu_ls_watch_n < 0) {
        int n = 0; const char* e = getenv("SPU_LS_WATCH");
        while (e && *e && n < SPU_WATCH_MAX) {
            s_spu_ls_watch[n++] = (unsigned)strtoul(e, (char**)&e, 0) & ~0xFu;
            while (*e == ',' || *e == ' ') e++;
        }
        g_spu_ls_watch_n = n;
    }
    *out_n = g_spu_ls_watch_n;
    return s_spu_ls_watch;
}
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    int n; spu_ls_watch_list(&n);
    if (!n) return;
    uint32_t a = lsa & (SPU_LS_MASK & ~0xFu);
    for (int i = 0; i < g_spu_ls_watch_n; i++) {
        if (s_spu_ls_watch[i] == a) {
            fprintf(stderr, "[spu-watch %s 0x%05X pc=0x%05X lr=0x%05X] "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                is_write ? "WR" : "rd", a, pc, lr,
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            fflush(stderr);
            break;
        }
    }
}

static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

/* A non-local return replaces the old host call chain without changing
 * any architectural registers, local store, or pending guest work. */
void spu_restart_dispatch(spu_context* ctx)
{
    g_spu_trampoline_fn = 0;
    if (s_spu_halt_armed) longjmp(s_spu_halt_env, 2);
    fprintf(stderr, "[spu] non-local return outside execution driver at 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}
/* M31 (SPU_PM_INTERP): full-interpreter dispatch for the wwsjob policy
 * module, driven through the SPU_DRAIN trampoline loop so every segment
 * passes the per-transfer hook -- interrupt delivery (spu_take_interrupt),
 * pch ring, task-launch checks -- exactly like a lifted dispatch. The M30
 * direct spu_interp_run bypassed all of that: no SN interrupt service, and
 * the exit/select kernel vectors (0x9C0/0x9D0) fell to zero-word stops.
 * Handles the two HLE kernel vectors inline (same protocol as
 * spu_indirect_branch) and keeps the context interpreted afterwards. */
static void spu_pm_interp_trampoline(spu_context* ctx);
/* M34b: SPURS-kernel-side event dispatch for the policy module, invoked at
 * EVERY trampoline pass. The real kernel's event loop services a pending
 * enabled event (or a parked MFC list -- kernel business) by vectoring
 * through the LS 0 exception vector into the task's glue (RAW 0xA2C: save
 * regs, kernel hook [NOP'd at staging, M34], SN handler `brsl 0x1810`,
 * restore, iret). The task itself arms and disarms that vector around its
 * idle loop, and the wwsjob yield path zeroes mask AND vector -- so a
 * parked list can wait forever with no hardware path left to deliver it
 * (M33/M33b: zero takes in lifted AND interp). We dispatch as the kernel
 * would, using the last valid LS0 bra vector the task planted (the entry
 * writes `bra 0xA2C' at every attach). NOT while already inside an
 * interrupt (irq_saved -- the handler runs with the vector disarmed by
 * construction). Returns 1 if the context was vectored into the handler. */
static int spu_pm_kernel_dispatch(spu_context* ctx)
{
    static uint32_t s_pm_vec_word = 0;
    {   uint32_t w = ((uint32_t)ctx->ls[0] << 24) | ((uint32_t)ctx->ls[1] << 16) |
                    ((uint32_t)ctx->ls[2] << 8) | (uint32_t)ctx->ls[3];
        uint32_t op9 = w >> 23;
        if (op9 == 0x060 || op9 == 0x064) s_pm_vec_word = w; }
    /* Pending MFC work cannot override disabled interrupts or masked events.
     * Entering the handler inside a disabled critical section can run policy
     * commands before their descriptor reset finishes. */
    if (ctx->irq_saved || !ctx->int_enable) return 0;
    int want = (ctx->event_status & ctx->event_mask) != 0;
    static unsigned s_chain = 0;
    if (!want) { s_chain = 0; return 0; }
    if (++s_chain > 4096) {
        static int _w = 0;
        if (_w++ < 4)
            fprintf(stderr, "[pm-exit] img=%d dispatch chain >4096 "
                    "(parked=0x%X st=0x%X) -- halting\n", ctx->image_id,
                    ctx->list_stall_mask, ctx->event_status);
        return 0;
    }
    uint32_t before_pc = ctx->pc;
    void (*nf)(spu_context*) = spu_take_interrupt(ctx, spu_pm_interp_trampoline);
    if (ctx->pc == before_pc && s_pm_vec_word) {
        /* LS0 was no branch at this pass (yield disarm), but the entry
         * planted a valid vector earlier: vector manually. */
        uint32_t op9 = s_pm_vec_word >> 23;
        uint32_t tgt = (((s_pm_vec_word >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
        (void)op9;
        ctx->srr0 = ctx->pc;
        ctx->int_enable = 0;
        { extern void spu_irq_regs_save(spu_context*);
          spu_irq_regs_save(ctx); }
        ctx->pc = tgt;
        nf = spu_pm_interp_trampoline;
        { static int _n = 0; if (_n++ < 16)
            fprintf(stderr, "[pm-exit] img=%d pc=0x%05X parked=0x%X st=0x%X "
                    "mask=0x%X LS0 disarmed -> manual vector 0x%05X\n",
                    ctx->image_id, before_pc, ctx->list_stall_mask,
                    ctx->event_status, ctx->event_mask, ctx->pc);
          fflush(stderr); }
    }
    if (ctx->pc != before_pc) {             /* taken: handler entered */
        g_spu_trampoline_fn = nf ? nf : spu_pm_interp_trampoline;
        return 1;
    }
    return 0;
}

/* cellSpursModulePoll returns the selected workload ID, not CELL_OK.
 * Each host policy context retains its own virtual SPU, so keeping that
 * context running selects the current ID from the kernel entry block. */
static uint32_t spu_pm_current_workload(const spu_context* ctx)
{
    const uint8_t* p = ctx->ls + 0x1DC;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void spu_pm_interp_trampoline(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (ctx->policy_mode && spu_pm_kernel_dispatch(ctx))
        return;   /* vectored into the SN handler; re-enter there */
    if (ctx->policy_mode) {
        if (pc == SPURS_PM_EXIT_TO_KERNEL_LS) {
            /* M33/M33b dispatch logic moved to spu_pm_kernel_dispatch (M34b):
             * it runs at EVERY trampoline pass -- the task never re-exits
             * after stalling a list; it spins in its idle loop (image
             * 0x2190-0x219C: ila/bi/bi/bi self-loop) with the vector
             * disarmed, so exit-time dispatch alone never fired. */
            { static int _n = 0;
              if (_n++ < 16) {
                  const uint8_t* v0 = ctx->ls;
                  fprintf(stderr, "[pm-exit] img=%d int_en=%d st=0x%X mask=0x%X "
                          "LS0=%02X%02X%02X%02X parked=0x%X\n",
                          ctx->image_id, (int)ctx->int_enable,
                          ctx->event_status, ctx->event_mask,
                          v0[0], v0[1], v0[2], v0[3],
                          ctx->list_stall_mask);
                  fflush(stderr); } }

            g_spurs_pm_exited = 1;
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (pc == SPURS_PM_SELECT_WORKLOAD_LS) {

            ++g_spurs_pm_polls;
            ctx->gpr[3] = spu_make_preferred_u32(spu_pm_current_workload(ctx));
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            g_spu_trampoline_fn = spu_pm_interp_trampoline;
            return;
        }
    }
    const int owned_mode=spu_native_owned_policy(ctx);
    const uint32_t stop=owned_mode ? ctx->drain_ret_pc : 0;
    uint32_t r = owned_mode ? spu_interp_run_native_call(ctx,pc,stop) : spu_interp_run(ctx,pc);
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      spu_irq_regs_maybe_restore(ctx); }   /* iret-bail contract (see
          spu_indirect_branch prologue): restore the handler's saved regs */
    if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) {
        spu_halt(ctx);
        return;
    }
    ctx->pc = r & SPU_LS_MASK;
    if (owned_mode && stop && ctx->pc==(stop & SPU_LS_MASK)) {
        if (g_spu_trampoline_fn==spu_indirect_branch) g_spu_trampoline_fn=0;
        return;
    }
    if (!owned_mode || !g_spu_trampoline_fn)
        g_spu_trampoline_fn = spu_pm_interp_trampoline;
}

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
int (*g_spu_user_event_hook)(spu_context*, uint32_t) = NULL;
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

/* Every MFC PUT an SPU completes, if anyone is listening. Installed by the GCM
 * layer (libs/video/cellGcmSys.c), which needs it to find the RSX pushbuffer:
 * a title whose SPUs build the command stream never tells libgcm where that
 * stream is, so the only evidence is the DMA that writes it. NULL until
 * installed, so a title with no SPU-built FIFO pays nothing. */
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = 0;

void spu_halt(spu_context* ctx)
{
    /* A halt abandons the host callable bracket, including when a lifted
     * IRQ/service halts outside the staged interpreter's resume helper. */
    ctx->interp_span_lo = ctx->interp_span_hi = ctx->interp_return_pc = 0;
    /* Register/context dump at halt-asserts: the WWS job's parameter guard
     * (heqi 0x1650 in job_wws_7BD900) checks (r12 bit0) & (r9 16-aligned) &
     * (r15 != 0) -- job descriptor/ABI values our execute path must supply.
     * Dump the low registers + the job's parameter area so a firing assert
     * names exactly which input is wrong. */
    { static int _n = 0;
      if (_n < 6 && ctx->status == SPU_STATUS_STOPPED_BY_HALT) { _n++;
        fprintf(stderr, "[spu-halt] img=%d pc=0x%05X regs:", ctx->image_id,
                ctx->pc & SPU_LS_MASK);
        for (int r = 3; r <= 16; r++)
            fprintf(stderr, " r%d=%08X", r, ctx->gpr[r]._u32[0]);
        fprintf(stderr, " r80=%08X r85=%08X\n",
                ctx->gpr[80]._u32[0], ctx->gpr[85]._u32[0]);
        fflush(stderr);
      } }
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

static mfc_engine* mfc_for(spu_context* ctx);   /* below: slot table */

/* Recoverable bail for a policy-mode lane that ran into an unrecoverable
 * spot mid-exursion (synthesised stop on zero words, a runtime-generated
 * stub-page target). Instead of spu_halt (which killed BOTH PM lanes in the
 * boot race, native_usercheck.err:4502-4506, and stalled the WWS queue until
 * the PPU spun on 0x40044480), abandon the excursion the same way the
 * [pm-iret] interpreter bail does: restore the register file saved at
 * interrupt TAKE, re-dispatch at srr0 with interrupts enabled, and let the
 * kernel/trampoline machinery service the remaining business.
 *
 * service != 0 also finishes the kernel service the SN handler was taken
 * for: ack every parked stall-and-notify list (WrListStallAck semantics)
 * and clear the SN event once nothing stays parked (WrEventAck semantics).
 * Without this the bail/take pair loops on the same pending event.
 *
 * Returns 0 (caller keeps the old stop/halt semantics) when the bail cannot
 * make progress: no usable resume target, or the same site bailed 256 times
 * in a row (a wedge must stay loud, not spin silently). */
int spu_pm_recoverable_bail(spu_context* ctx, uint32_t site, int service)
{
    /* SPU_PM_HALT_FORENSIC=1: one state dump per context on the bail path. */
    { static int s_f = -1;
      if (s_f < 0) s_f = getenv("SPU_PM_HALT_FORENSIC") ? 1 : 0;
      if (s_f) {
          static const spu_context* s_dumped[8];
          int i, seen = 0;
          for (i = 0; i < 8; i++) if (s_dumped[i] == ctx) seen = 1;
          if (!seen) {
              for (i = 0; i < 8 && s_dumped[i]; i++) {}
              if (i < 8) s_dumped[i] = ctx;
              #define FW(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|\
                             ((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
              uint32_t p1330 = FW(0x1330);
              uint32_t lo = site > 0x20u ? site - 0x20u : 0u;
              fprintf(stderr, "[pm-forensic] img=%d site=0x%05X pc=0x%05X "
                      "lr=0x%05X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X\n"
                      "[pm-forensic] 12A0=%08X 12A4=%08X 12A8=%08X 12AC=%08X "
                      "1330=%08X [1330]+10=%08X 14E0=%08X 1E0.w1=%08X\n"
                      "[pm-forensic] LS[%05X..%05X):",
                      ctx->image_id, site, ctx->pc & SPU_LS_MASK,
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                      ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0],
                      ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0],
                      ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0],
                      FW(0x12A0), FW(0x12A4), FW(0x12A8), FW(0x12AC),
                      p1330,
                      (p1330 + 0x13u < SPU_LS_SIZE) ? FW(p1330 + 0x10u) : 0u,
                      FW(0x14E0), FW(0x1E4),
                      lo, lo + 0x40u);
              for (uint32_t o = 0; o < 0x40u; o += 4)
                  fprintf(stderr, " %08X", FW(lo + o));
              fprintf(stderr, "\n");
              #undef FW
              fflush(stderr);
          }
      } }

    static uint32_t s_last = 0xFFFFFFFFu, s_same = 0;
    if (site == s_last) { if (++s_same > 256u) return 0; }
    else { s_last = site; s_same = 1; }

    if (service) {
        mfc_engine* mfc = mfc_for(ctx);
        while (ctx->list_stall_mask) {
            uint32_t t = 0;
            while (t < 32 && !(ctx->list_stall_mask & (1u << t))) t++;
            if (t >= 32) break;
            { static int _n = 0;
              if (_n++ < 16)
                  fprintf(stderr, "[pm-bail] img=%d host stall service: ack tag %u "
                          "(parked=0x%X evt=0x%X)\n", ctx->image_id, t,
                          ctx->list_stall_mask, ctx->event_status); }
            mfc_list_stall_ack(mfc, ctx, t);
        }
        if (!ctx->list_stall_mask) {
            spu_lockline_lock();
            ctx->event_status &= ~0x2u;   /* SPU_EVENT_SN (spu_dma.h) */
            spu_lockline_unlock();
        }
    }

    uint32_t resume;
    if (ctx->irq_saved && (ctx->srr0 & SPU_LS_MASK)) {
        resume = ctx->srr0 & SPU_LS_MASK;
    } else if (ctx->gpr[0]._u32[0] & SPU_LS_MASK) {
        /* Not inside a take: return to the caller's bisl link, the same
         * resume the 0x9D0 select-workload interception uses. */
        resume = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    } else {
        return 0;   /* nowhere recoverable to -- keep the old semantics */
    }

    { static int _n = 0;
      if (_n++ < 16)
          fprintf(stderr, "[pm-bail] img=%d site=0x%05X recoverable: "
                  "srr0=0x%05X irq_saved=%d -> lane resumes\n",
                  ctx->image_id, site, resume, ctx->irq_saved); }

    ctx->interp_span_lo = ctx->interp_span_hi = ctx->interp_return_pc = 0;
    ctx->status = SPU_STATUS_RUNNING;
    ctx->stop_code = 0;
    ctx->pc = resume;
    if (ctx->irq_saved) {
        /* The bail plays the handler's irete: land on srr0 with interrupts
         * enabled, where spu_irq_regs_maybe_restore replays the TAKE-time
         * register file (it may also unwind an irq frame; that frame's
         * drain resumes the lane -- the [pm-iret] contract). */
        ctx->int_enable = 1;
        { extern int spu_irq_regs_maybe_restore(spu_context*);
          spu_irq_regs_maybe_restore(ctx); }
    }
    /* Bracket the continuation as a staged span: a mid-function srr0/link
     * has no lifted entry, so the re-dispatch interprets it until it
     * rejoins lifted code (the resolver's staged entry does the rest). */
    ctx->interp_span_lo = resume;
    ctx->interp_span_hi = SPU_LS_SIZE;
    ctx->interp_return_pc = 0;
    g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}

/* Diagnostic: dump the taskset-policy scheduler's working tables (LS 0x2700..)
 * at func_00000E60 entry, to reverse why it computes "no runnable task". Env
 * YDKJ_E60. Called from the lifted taskset policy. */
void spu_dbg_e60(spu_context* ctx)
{
    static int s_e = -1; if (s_e < 0) s_e = getenv("YDKJ_E60") ? 1 : 0;
    if (!s_e) return;
    static int _d = 0; if (_d++ >= 6) return;
    const uint8_t* L = ctx->ls;
    #define RD(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
    fprintf(stderr, "[E60] r20=%08X r24=%08X | run2700=%08X ready2710=%08X pend2720=%08X en2730=%08X sig2740=%08X wait2750=%08X x2770=%08X\n",
            ctx->gpr[20]._u32[0], ctx->gpr[24]._u32[0],
            RD(0x2700), RD(0x2710), RD(0x2720), RD(0x2730), RD(0x2740), RD(0x2750), RD(0x2770));
    #undef RD
    fflush(stderr);
}

/* SPU `stop` / stop-and-signal. A real SPU stop HALTS the core; the previous
 * lifted emission only did `status=...; return;`, which unwound ONE frame and
 * let the caller's service loop keep running -- so a SPURS policy doing
 * stop-and-signal in a loop spun millions of times instead of yielding. Halt the
 * host SPU thread (longjmp to spu_run_with_halt) so the job stops AT the stop,
 * with ctx->status + the outbound mailbox value available for the dispatcher to
 * service. Env YDKJ_STOP_NOHALT restores the old (looping) behavior for A/B. */
void spu_stop(spu_context* ctx)
{
    /* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
     * policy this is stop-and-signal: it returns up into the kernel/policy
     * service loop, which continues (i.e. the SPU "resumes" past the stop). That
     * resume-by-return behavior is correct; the previous spin was caused by the
     * lack of (a) a PPU listener for the outbound mailbox and (b) mailbox
     * backpressure -- both handled in the channel write path, not here. So by
     * default DO NOT halt. Env YDKJ_STOP_HALT forces a hard halt (longjmp) for
     * A/B experiments (it makes the kernel terminate, which the game restarts). */
    static int s_halt = -1;
    if (s_halt < 0) s_halt = getenv("YDKJ_STOP_HALT") ? 1 : 0;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (s_halt) spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
/* The three option readers of spu_native_owned.h. Each runs one time on a
 * host thread: it reads the option and fills the thread-local word that the
 * inline readers use. */
int spu_native_owned_enabled(void) {
    const char* value=getenv("SPU_NATIVE_OWNED_JOBS");
    int mode;
    if (!value || !strcmp(value,"0")) mode=0;
    else if (!strcmp(value,"1")) mode=1;
    else { fprintf(stderr,"invalid SPU_NATIVE_OWNED_JOBS: expected 0 or 1\n"); abort(); }
    g_spu_native_owned_mode=mode+1;
    return mode;
}
int spu_native_owned_observed(void) {
    int observed;
#if PS3_EXECUTION_OBSERVATION || PS3_ATOMIC_OBSERVATION
    observed=1; /* retain the original interpreter observation granularity */
#else
    static const char* const options[]={"SPU_TRACE_CALLS","SPU_BOUNDARY_DIR",
        "SPU_CALLTRACE","SPU_PCHIST","SPU_LS_LOWREAD","SPU_LS_WATCH",
        "SPU_SMC_WATCH","SPU_STEPCAP","SPU_TRACE","YDKJ_SPU_TRACE","YDKJ_CRI_GATE1TRACE"};
    observed=0;
    for (unsigned i=0;i<sizeof(options)/sizeof(options[0]);++i)
        observed |= getenv(options[i])!=NULL;
#endif
    g_spu_native_owned_observed=observed+1;
    return observed;
}
int spu_native_owned_image(void) {
    const char* value = getenv("SPU_NATIVE_OWNED_IMAGE");
    int image = 0;
    if (value) {
        unsigned long id = strtoul(value, NULL, 10);
        if (!id || id > INT_MAX ||
            value[strspn(value, "0123456789")] != '\0') {
            fprintf(stderr, "invalid SPU_NATIVE_OWNED_IMAGE: expected a positive decimal image ID\n");
            abort();
        }
        image = (int)id;
    }
    g_spu_native_owned_image=image+1;
    return image;
}
void spu_native_owned_interpret(spu_context* ctx) {
    spu_pm_interp_trampoline(ctx);
}

int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    /* Job-args probe: image-1 (the WWS job binary) entries arrive at the crt
     * with r3/r4 zeroed in SOME runs while the jm2 runner sets them -- count
     * and stamp every image-1 run here to find the argless caller. */
    if (ctx->image_id == 1) {
        static int _n = 0;
        if (_n < 12) { _n++;
            fprintf(stderr, "[rwh] #%d img=1 ctx=%p r3=%08X r4=%08X entry=%p\n",
                    _n, (void*)ctx, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    (void*)entry);
            fflush(stderr); }
    }
    /* Also discard any saved register file keyed to this context pointer:
     * contexts are stack locals, so the address recurs and a stale save from
     * an earlier run would be restored over this job's arguments. */
    { extern void spu_irq_regs_forget(spu_context*); spu_irq_regs_forget(ctx); }
    ctx->steps = 0;   /* fresh run: step 0 is the entry */
    /* The host stack is empty here by definition, so no lifted caller frame
     * is live. A persistent context (a SPURS policy module re-entered every
     * scheduling quantum) otherwise carries the depth its last quantum
     * halted at: two frames per quantum for an idle job manager, and the
     * recursion guard halted the SPU at 2000 after a few minutes of idling. */
    ctx->host_depth = 0;
    ctx->irq_frame = 0;
    ctx->drain_ret_pc = 0;
    ctx->interp_span_lo = ctx->interp_span_hi = ctx->interp_return_pc = 0;
    /* Nest-safe: a job run synchronously from inside another SPU's execution
     * re-enters here on the same host thread. The halt target is per-thread,
     * so save the outer one and put it back on the way out -- otherwise the
     * outer run's next halt longjmps into this (by then dead) frame, and a
     * stack-local job context skips its coherency unregister (GH3: crashes in
     * spu_coh_notify_write on a freed stack). */
    jmp_buf outer_env; const int outer_armed = s_spu_halt_armed;
    if (outer_armed) memcpy(outer_env, s_spu_halt_env, sizeof(jmp_buf));
    s_spu_halt_armed = 1;
    g_spu_trampoline_fn = 0;                        /* no stale transfer pending */
    /* Lockstep gate (env SPU_LOCKSTEP, default off): join the round-robin
     * ring and BLOCK until this ctx holds the run token, so only one lifted SPU
     * executes at a time. No-op when unarmed. The thread-local halt env above
     * makes a token pause/resume mid-run safe. */
    yz_lockstep_register(ctx);
    const uint32_t stats_mark = spu_interp_stats_mark();
    /* Both jumps below leave float scopes without running their leave. */
    const uint64_t sf_mark = spu_sf_simd_mark();
    switch (setjmp(s_spu_halt_env)) {
    case 1:
        spu_sf_simd_leave(sf_mark);
        spu_interp_stats_unwind(stats_mark);
        halted = 1;
        g_spu_trampoline_fn = 0;
        break;
    case 2: {
        spu_sf_simd_leave(sf_mark);
        spu_interp_stats_unwind(stats_mark);
        /* A one-way guest stack reset invalidates every lifted host caller.
         * Re-enter its target with the same guest state on the driver stack. */
        ctx->host_depth = 0;
        ctx->drain_ret_pc = 0;
        g_spu_trampoline_fn = 0;
        /* The abandoned callers' scope covered this code; hold one again. A
         * further jump out of it lands above and restores sf_mark. */
        const uint64_t sf_scope = spu_sf_simd_enter();
        spu_indirect_branch(ctx);
        SPU_DRAIN(ctx);
        spu_sf_simd_leave(sf_scope);
        break;
    }
    default:
        /* SPU_DRAIN trampoline model: the top-level entry runs until its first
         * cross-function tail transfer, which sets g_spu_trampoline_fn and
         * returns; the drain loop re-enters each queued target until the SPU
         * halts (stop -> longjmp) or the trampoline empties. Nested brsl/bisl
         * calls drain inside their own call brackets (see the lifter). */
        /* M28 (SPU_PM_INTERP=1): run the wwsjob policy module (image 200)
         * through the INTERPRETER instead of lifted dispatch, inside the
         * same halt harness. The M11 pre-registration flow (fully
         * interpreted) reached the loader chain and staged jobmod image
         * 215; the lifted flow never does. Interpreting isolates whether
         * the divergence lives in lifted-helper math (shufb/rotqby class,
         * cf. the M16 rotate bug) or in shared state. */
        static int s_pminterp = -1;
        if (s_pminterp < 0) s_pminterp = getenv("SPU_PM_INTERP") ? 1 : 0;
        if (s_pminterp && ctx->policy_mode) {
            /* M31: drain-driven interpretation so interrupt delivery stays
             * live (the M30 direct call bypassed spu_take_interrupt -- no SN
             * service -- and the exit/select kernel vectors fell to zero-word
             * stops). Image sits at RAW LS 0xA00; labels image-relative. */
            /* Round 29 (SPU_PM_PC_RESUME, default on): resetting pc to the
             * PM entry every quantum abandons a walk-in jobmod mid-run --
             * the next pass re-ran the state machine from its top and the
             * per-slot prep wiped the module's still-live input tables
             * (retire/republish livelock, gate 0x40044414 spin). A real
             * SPU never restarts a preempted context at its entry. Resume
             * from the halted pc while a walk-in is logically in flight
             * (wi_state 1 = pc inside a staged module span, 2 = inside
             * since the last kernel handoff); wi_state is cleared only at
             * the exit/select vector handoff (spu_interp.c), so every
             * genuine kernel-handoff cycle still restarts at the entry.
             * SPU_PM_PC_RESUME=0 restores the unconditional reset. */
            static int s_pcr = -1;
            if (s_pcr < 0) { const char* e = getenv("SPU_PM_PC_RESUME");
                             s_pcr = (e && e[0] == '0') ? 0 : 1; }
            static int s_flow = -1;
            if (s_flow < 0) { const char* e = getenv("SPU_PM_FLOW_RESUME");
                              s_flow = (e && e[0] == '0') ? 0 : 1; }
            static int s_yld = -1;
            if (s_yld < 0) { const char* e = getenv("SPU_PM_YIELD_RESUME");
                             s_yld = (e && e[0] == '0') ? 0 : 1; }
            /* Round 30 (a): PASS log shows what EVERY interp pass starts
            * with -- bg_287's walkin_resume=0 was partly a probe artifact:
            * the round-29 RESUME log was capped at 16 TOTAL, and the first
            * 16 passes are normal wi==0 restarts, so the cap was spent
            * before the first resume could ever print. Both logs now run
            * 16 + every Nth. */
            { static int s_pn = 0;
              if (s_pn++ < 32 || (s_pn % 4096) == 0)
                  fprintf(stderr, "[walkin] PASS wi=%d img=%d pc=0x%05X #%d\n",
                          ctx->wi_state, ctx->wi_img,
                          (uint32_t)ctx->pc & SPU_LS_MASK, s_pn),
                  fflush(stderr); }
            /* Round 33 (SPU_PM_FLOW_RESUME, default on): generalize the
             * walk-in resume to the WHOLE PM flow. bg_289 froze at
             * {12A0=0 (post-0x3014 fsmbi reset), 12C0 parked, 12D0=-1}
             * while the guest kept posting bit-30 0xEB0 entries: a pass
             * was cut inside the retire->rebuild->launch window
             * (0x2EF0..0x3074), the next pass restarted at the entry, and
             * the half-updated state is dead-parked by construction (the
             * claim gate needs signed 12C0 > -1; the SN re-arm needs
             * 12A0==1; both un-reachable from state 0). On HW the window
             * runs without interruption. Resume from ctx->pc whenever the
             * previous pass did NOT end at a designed cooperative yield:
             * the interpreter marks pm_designed=1 only at the kernel
             * handoff (exit/select vectors), the idle-bail park, and a
             * genuine stop; a fresh/reset context starts designed=1 so the
             * first pass resets. Composes with the walk-in resume: wi!=0
             * still resumes regardless. SPU_PM_FLOW_RESUME=0 restores the
             * entry-only reset (walk-in resume still governed by
             * SPU_PM_PC_RESUME). */
            if (s_pcr && ctx->wi_state) {
                /* Round 34 (SPU_PM_YIELD_RESUME, default on): a module STOP
                 * inside a walk-in span flagged wi_link_pending with the
                 * bisl link captured at span entry. Resuming AT THE STOP
                 * would re-execute the stopd forever; the HW continuation
                 * is the LINK (0x3068-class) -- the PM window that launched
                 * the module completes (brsl 0x2480, bi 0x14E0), the
                 * completion counters get written, and the kernel handoff
                 * then proceeds normally. */
                if (s_yld && ctx->wi_link_pending) {
                    static unsigned s_yr = 0;
                    ctx->pc = ctx->wi_resume_link & SPU_LS_MASK;
                    ctx->wi_link_pending = 0;
                    if (s_yr++ < 16 || (s_yr % 1024) == 0)
                        fprintf(stderr, "[pm-yield] RESUME at bisl-link "
                                "pc=0x%05X wi=%d #%u\n",
                                (uint32_t)ctx->pc & SPU_LS_MASK,
                                ctx->wi_state, s_yr),
                        fflush(stderr);
                }
                static unsigned s_rq = 0;
                if (s_rq++ < 16 || (s_rq % 1024) == 0)
                    fprintf(stderr, "[walkin] RESUME pc=0x%05X wi=%d #%u ctx=%p\n",
                            (uint32_t)ctx->pc & SPU_LS_MASK, ctx->wi_state,
                            s_rq, (void*)ctx), fflush(stderr);
            } else if (s_flow && !ctx->pm_designed) {
                static unsigned s_fr = 0;
                if (s_fr++ < 16 || (s_fr % 1024) == 0)
                    fprintf(stderr, "[pm-flow] RESUME pc=0x%05X #%u "
                            "(pass cut mid-flow)\n",
                            (uint32_t)ctx->pc & SPU_LS_MASK, s_fr),
                    fflush(stderr);
            } else {
                ctx->pc = ctx->img_ls_base;
            }
            ctx->pm_designed = 0;      /* consume: this pass's exit re-marks it */
            ctx->pm_win_touched = 0;   /* probe window resets per pass (R34) */
            g_spu_trampoline_fn = spu_pm_interp_trampoline;
        } else if (spu_native_owned_policy(ctx))
            spu_indirect_branch(ctx);
        else entry(ctx);
        { static int _e = 0;
          if (getenv("SPU_ARGWATCH") && _e < 8) { _e++;
              fprintf(stderr, "[argwatch] after entry(): img=%d pc=0x%05X "
                      "r2=0x%08X r3=0x%08X r4=0x%08X\n", ctx->image_id,
                      (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[2]._u32[0],
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr); } }
        SPU_DRAIN(ctx);
    }
    ctx->drain_ret_pc = 0; /* no compiled caller survives this driver return */
    yz_lockstep_unregister(ctx);   /* leave the ring; hand the token onward */
    s_spu_halt_armed = outer_armed;
    if (outer_armed) memcpy(s_spu_halt_env, outer_env, sizeof(jmp_buf));
    return halted;
}

/* Private ownership and generation-validated warm engine lookup. */
#include "spu_mfc_registry.h"

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* The spinlock guarding all atomic line ops now lives in spu_coherency.c as
 * spu_lockline_lock/unlock. It used to be a file-static here, which serialized
 * SPU against SPU but left the PPU free to store into a line in the middle of
 * a PUTLLC's compare-and-commit -- the update the SPU was about to make would
 * be written over, and the SPU would never learn the line had changed. Same
 * lock, same critical sections, now shared with the PPU store paths. */

/* Total PUTLLC attempts (all SPUs). The PM flow trace (spurs_policy.c) reads
 * the delta across one policy run to find the run that performed a claim. */
volatile unsigned g_spu_putllc_count = 0;
/* PUTLLCs that hit the WATCHED sync line (g_barrier_sync_watch) -- isolates the
 * work-run on the stalled job queue from the dozen idle jobmanager instances. */
volatile unsigned g_spu_putllc_sync_hit = 0;

/* Lowest LS address treated as "not job code". Jobs load at 0 and
 * their stacks top out well below this; a branch at or above it with no
 * lifted code is a runaway, not a service call. */
#define SPU_JM2_KERNEL_BASE 0x15000u

/* Link-register value handed to a SPURS job so its final `bi $r0` lands
 * somewhere we recognise. The real job manager passes a return address into
 * itself; we have no manager, so we pass this and treat arriving here as job
 * completion. Chosen above the context/descriptor we park near the top of LS. */
#define SPU_JOB_RETURN_LS   0x3FF00u

/* Round-8: policy-context WRITE watch. Every store from a policy context
 * into the joblist window [0x40044000,0x40045000) logs the old guest word
 * and the word about to land, capped at 32. Catches the stale-flush class
 * directly: the PM writing its CACHED header line back over a freshly
 * posted joblist, leaving host dumps reading empty forever while the PM
 * believes it committed. */
/* Round 23: last staged jobmod window (set at registration in
 * spu_overlay_note_get) + traffic watch counter. */
uint32_t g_ms_last_lsa, g_ms_last_size;
unsigned g_ms_last_img, g_ms_last_seq;

/* Round 17: who fills the PM's launch-window LS [0xEB0,0x12B0)?
 * (0xEB0 gather-descriptor table, 0x11B0 logicalToBufferNum map, 0x12A0
 * state cell.) Every policy-context write landing in the window logs the
 * 16 landed bytes: interpreter/HLE stores (via spu_ls_write128), DMA GET
 * payloads, GETLLAR lines. The healthy fillers stop firing at the wedge --
 * that filler's data source is the broken input. Capped 12 then 1/256. */
void spu_pm_fill_note(spu_context* ctx, uint32_t lsa, const void* bytes16, const char* src)
{
    /* SPU_LANE_TRACE gates the [pm-fill] lines; see spu_context.h. */
    if (!spu_lane_trace()) return;
    static int _n = 0;
    if (_n++ < 12 || (_n % 256) == 0) {
        const uint8_t* b = (const uint8_t*)bytes16;
        fprintf(stderr, "[pm-fill] %s pc=0x%05X lsa=0x%05X:", src,
                (uint32_t)ctx->pc & SPU_LS_MASK, lsa & SPU_LS_MASK);
        for (int i = 0; i < 16; i += 4)
            fprintf(stderr, " %02X%02X%02X%02X", b[i], b[i+1], b[i+2], b[i+3]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/* Round 27: SPU side of the [wp] watchpoints (PPU side: ppu_loader.cpp
 * barrier_watch_hit + stwcx/stdcx). Words: 0x400444EC (gate+0x6C),
 * 0x4004C592 (vjob taken@+0x12), 0x4004C594 (minActive@+0x14). new_bytes
 * are the FINAL memory bytes exactly as the following memcpy will land
 * them (raw BE). First healthy-phase baseline of who advances the gate.
 * SPU_WP=0 disables both sides; default ON. */
void spu_wp_note(const spu_context* ctx, uint32_t ea, uint32_t size,
                 const void* new_bytes, const char* by)
{
    static const uint32_t w[3] = { 0x400444ECu, 0x4004C592u, 0x4004C594u };
    static int on = -1;
    static long cnt[3];
    if (on < 0) { const char* e = getenv("SPU_WP");
                  on = (e && e[0] == '0' && !e[1]) ? 0 : 1; }
    if (!on) return;
    const uint8_t* nb = (const uint8_t*)new_bytes;
    for (int i = 0; i < 3; i++) {
        if (ea > w[i] || ea + size <= w[i]) continue;
        const long n = ++cnt[i];
        if (n <= 24 || (n % 256) == 0) {
            const uint32_t d = w[i] - ea;
            const uint8_t* ob = vm_base + w[i];
            fprintf(stderr, "[wp] ea=0x%08X old=%02X%02X%02X%02X "
                    "new=%02X%02X%02X%02X by=%s-img%d-pc0x%05X\n", w[i],
                    ob[0],ob[1],ob[2],ob[3],
                    nb[d],nb[d+1],nb[d+2],nb[d+3],
                    by, ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK);
            fflush(stderr);
        }
    }
}



void spu_pm_store_note(spu_context* ctx, uint32_t ea, uint32_t lsa, const char* kind)
{
    if (!ctx || !ctx->policy_mode) return;
    if (ea < 0x40044000u || ea >= 0x40045000u) return;
    static int _n = 0;
    if (_n++ >= 32) return;
    const uint8_t* m = vm_base + ea;
    const uint32_t old = ((uint32_t)m[0]<<24)|((uint32_t)m[1]<<16)|((uint32_t)m[2]<<8)|m[3];
    const uint8_t* l = ctx->ls + (lsa & SPU_LS_MASK);
    const uint32_t nw  = ((uint32_t)l[0]<<24)|((uint32_t)l[1]<<16)|((uint32_t)l[2]<<8)|l[3];
    fprintf(stderr, "[pm-store] %s img=%d pc=0x%05X lr=0x%05X ea=0x%08X"
                    " old=0x%08X new=0x%08X\n",
            kind, ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK),
            ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea, old, nw);
    fflush(stderr);
}

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
/* PS3_ATOMIC_OBSERVER_SPU_BEGIN */
#if PS3_ATOMIC_OBSERVATION
static const ps3_atomic_observer* s_spu_atomic_observer;
#endif
int ps3_atomic_observer_install_spu(const ps3_atomic_observer* o)
{
    if (o && (!o->clock || !o->observe)) return 0;
#if PS3_ATOMIC_OBSERVATION
    s_spu_atomic_observer = o;
    return 1;
#else
    return o == NULL;
#endif
}
/* PS3_ATOMIC_OBSERVER_SPU_END */

static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    /* Classify FIRST. Every MFC_Cmd write lands here, and only the switch at
     * the bottom used to filter -- so the uncommitted-EA guard below reported
     * ORDINARY DMA as `[spu-atomic]`. A plain put (0x20) to a garbage EA read
     * as an atomic spin, which is a genuinely misleading place to start
     * debugging from. Non-atomic commands belong to the DMA engine. */
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    /* LV1 dual mapping (see spu_dma.h): SPURS lock-line ops on work-area EAs
     * arrive as base|0x05000000; normalize to the registered allocation. */
    { extern uint32_t sysmem_spu_alias_fixup(uint32_t);
      ea = sysmem_spu_alias_fixup(ea); }
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;
#if PS3_ATOMIC_OBSERVATION
    const ps3_atomic_observer* observer = cmd == MFC_GETLLAR_CMD ? NULL :
        ps3_atomic_observer_select(s_spu_atomic_observer, PS3_ATOMIC_SPU_STORE,
            (uintptr_t)ctx, ea, MFC_ATOMIC_LINE);
    ps3_atomic_observation observation;
#endif
    /* Round 21(b): final-EA truth for policy atomics. bg_276 killed the
     * 0x7FFFFFFF rule (0x00440440 holds EBOOT .text, not the queue); the
     * container-arena rule 0x40000000|(ea&0x3FFFFFFF) puts 0x80440440 at
     * 0x40440440, inside the live SPURS arena. Log BOTH candidates' guest
     * words so one run decides which mapping holds the posted queue. */
    if (ctx->policy_mode && (ctx->mfc_eal & 0x80000000u)
        && ctx->mfc_eal < 0x90000000u) {
        static int _a = 0;
        if (_a++ < 16) {
            uint32_t lo = ctx->mfc_eal & 0x7FFFFFFFu;
            uint32_t hi = 0x40000000u | (ctx->mfc_eal & 0x3FFFFFFFu);
            const unsigned char* gl = vm_base + lo;
            const unsigned char* gh = vm_base + hi;
            fprintf(stderr, "[mfc-alias] raw=0x%08X cmd=0x%X | @0x%08X:", ctx->mfc_eal, cmd, lo);
            for (int k = 0; k < 8; k += 4)
                fprintf(stderr, " %02X%02X%02X%02X", gl[k], gl[k+1], gl[k+2], gl[k+3]);
            fprintf(stderr, " | @0x%08X:", hi);
            for (int k = 0; k < 8; k += 4)
                fprintf(stderr, " %02X%02X%02X%02X", gh[k], gh[k+1], gh[k+2], gh[k+3]);
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_lastea = 0;
        if ((++s_n % 2000000) == 0 || ea != s_lastea) {
          if ((s_n % 2000000) == 0)
            fprintf(stderr, "[atomcnt] %llu atomic ops; last cmd=0x%X ea=0x%08X\n",
                    (unsigned long long)s_n, cmd, ea);
          s_lastea = ea; } } }
    /* SPU_ATOMTRACE=1 keeps the old 40-line cap; =<N> raises it. A busy image
     * (FMOD) exhausts 40 before a quieter one issues its first atomic, which
     * reads as "that image never does atomics" when it simply never got a line. */
    { static int s_at = -1;
      if (s_at < 0) { const char* e = getenv("SPU_ATOMTRACE");
                      int v = e ? atoi(e) : 0; s_at = e ? (v > 1 ? v : 40) : 0; }
      if (s_at) { static int _a=0; if (_a++ < s_at)
        fprintf(stderr, "[atom] cmd=0x%02X ea=0x%08X (img=%d)\n", cmd, ea, ctx->image_id); } }
    /* cri task (img22) atomic on the taskset: dump the loaded bitset line so we can
     * see if the task reads MY taskset (0x4005E000) with my READY bit, or elsewhere. */
    { static int s_ct=-1; if(s_ct<0) s_ct=getenv("SPU_ATOMTRACE")?1:0;
      if(s_ct && ctx->image_id==22 && cmd==0xD0 && mfc_ea_range_committed(ea,16)) {
        static int _c=0; if(_c++<24){
          uint8_t* m=vm_base+ea;
          #define BW(o) (((uint32_t)m[o]<<24)|((uint32_t)m[o+1]<<16)|((uint32_t)m[o+2]<<8)|m[o+3])
          fprintf(stderr,"[cri-atom] GETLLAR ea=0x%08X line[0..0x30]: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            ea, BW(0),BW(4),BW(8),BW(0xC), BW(0x10),BW(0x14),BW(0x18),BW(0x1C), BW(0x20),BW(0x24),BW(0x28),BW(0x2C));
          #undef BW
        } } }
    /* YDKJ_CRI_R4: dump the CellSpursTaskset bitsets when the policy atomically
     * touches my taskset (0x0F000000), to watch the task-activation state machine
     * (why task0 isn't selected+first-run). running@0 ready@0x10 pending@0x20
     * enabled@0x30 signalled@0x40 waiting@0x50 (each 16B; word0 = MSB, task0=bit127). */
    { static int s_td = -1; if (s_td < 0) s_td = (getenv("YDKJ_CRI_CHAIN") && getenv("SPU_ATOMTRACE")) ? 1 : 0;
      if (s_td && ea >= 0x0F000000u && ea < 0x0F001900u) {
        extern uint8_t* vm_base;
        static int _t=0; if (vm_base && _t++ < 24) {
            uint8_t* t = vm_base + 0x0F000000u;
            #define TW(o) (((uint32_t)t[o]<<24)|((uint32_t)t[o+1]<<16)|((uint32_t)t[o+2]<<8)|t[o+3])
            fprintf(stderr, "[tset] %s run=%08X rdy=%08X pnd=%08X ena=%08X sig=%08X wait=%08X | wid=%08X last=%02X\n",
                    cmd==0xD0?"GET":cmd==0xB4?"PUT":"?", TW(0x00), TW(0x10), TW(0x20), TW(0x30), TW(0x40), TW(0x50), TW(0x74), t[0x73]);
            #undef TW
        }
      } }

    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_range_committed(ea, MFC_ATOMIC_LINE)) {
        static int s_w = 0;
        if (s_w++ < 16)
            fprintf(stderr, "[spu-atomic] cmd=0x%X ea=0x%08X pc=0x%05X img=%d uncommitted -- skipped\n",
                    cmd, ea, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->image_id);
        if (cmd == MFC_GETLLAR_CMD) {
            memset(ls, 0, MFC_ATOMIC_LINE);
            ctx->resv_ea = ea; ctx->resv_valid = 0; ctx->atomic_stat = 4;
        } else {
            ctx->atomic_stat = 1;   /* PUTLLC failure (line "moved") */
        }
        #if PS3_ATOMIC_OBSERVATION
        if (observer) {
            ps3_atomic_observation_begin(&observation, observer,
                PS3_ATOMIC_SPU_STORE, cmd, ea, MFC_ATOMIC_LINE, (uintptr_t)ctx,
                (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            ps3_atomic_observation_end(&observation, observer,
                PS3_ATOMIC_INVALID_RANGE | PS3_ATOMIC_DATA_UNAVAILABLE);
        }
        #endif
        return 1;
    }

    /* SPU_ATOM_EA=<hex>: log every lock-line atomic touching that EA`s 128B line.
     * SPU_ATOM_FULL=1 additionally dumps 32 bytes of the line before and after.
     *
     * Built into ONE buffer and written with a single fprintf: several SPU host
     * threads and the PPU all write stderr, so a dump split across many fprintf
     * calls comes out shredded and unreadable exactly when it matters. */
    { static int s_ae = -1; static uint32_t s_aea;
      if (s_ae < 0) { const char* e = getenv("SPU_ATOM_EA");
        s_aea = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_ae = s_aea ? 1 : 0;
        if (s_ae == 1) { extern uint32_t g_barrier_sync_watch;
            g_barrier_sync_watch = s_aea; } /* arm PUTLLC OK/FAIL verdict log */ }
      if (s_ae == 1 && ((uint32_t)ea & ~127u) == s_aea) {
          static int _n = 0, s_cap = -1;
          /* SPU_ATOM_EA_MAX=<n>: lines to print (default 48, 0 = unlimited). */
          if (s_cap < 0) { const char* m = getenv("SPU_ATOM_EA_MAX"); s_cap = m ? atoi(m) : 48; }
          if (!s_cap || _n++ < s_cap) {
              extern uint8_t* vm_base;
              const uint8_t* r = vm_base + ((uint32_t)ea & ~127u);
              static int s_af = -1;
              if (s_af < 0) { const char* f = getenv("SPU_ATOM_FULL"); s_af = f ? (atoi(f) > 1 ? 2 : 1) : 0; }
              char buf[1024]; int p = 0;
              /* lr (gpr[0]) as well as pc: these atomics sit in generic helpers,
               * so pc names the HELPER and only lr names the real caller. */
            p += snprintf(buf + p, sizeof buf - p,
                            "[atom-ea] cmd=0x%X img=%d pc=0x%05X lr=0x%05X ea=0x%08X r3=%u r14=%u",
                            cmd, ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                            ctx->gpr[0]._u32[0] & SPU_LS_MASK, (uint32_t)ea,
                            ctx->gpr[3]._u32[0], ctx->gpr[14]._u32[0] & 0xF);
              int nb = s_af == 2 ? 128 : s_af ? 64 : 8;   /* 64 covers +0x30 pendingRecv; =2 the whole line */
              p += snprintf(buf + p, sizeof buf - p, " RAM=");
              for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                  p += snprintf(buf + p, sizeof buf - p, "%02X%s", r[i],
                                (i % 4 == 3) ? " " : "");
              if (cmd == MFC_PUTLLC_CMD) {
                  p += snprintf(buf + p, sizeof buf - p, " STORE=");
                  for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                      p += snprintf(buf + p, sizeof buf - p, "%02X%s", ls[i],
                                    (i % 4 == 3) ? " " : "");
              }
              fprintf(stderr, "%s\n", buf); fflush(stderr);
          }
      } }
    /* Bink sync-line atomic trace (armed by the PPU-side producer probe). */
    { extern uint32_t g_barrier_sync_watch;
      uint32_t b = g_barrier_sync_watch;
      if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
          static int _n = 0;
          if (_n++ < 4096)
              fprintf(stderr, "[sync-atomic] %s img=%d pc=0x%05X ea=0x%08X\n",
                      cmd == MFC_GETLLAR_CMD ? "GETLLAR" :
                      cmd == MFC_PUTLLC_CMD ? "PUTLLC" : "PUTLLUC",
                      ctx->image_id, (uint32_t)ctx->pc, ea);
      } }
    switch (cmd) {
    case MFC_GETLLAR_CMD:
        /* Lockstep tick: a guest GETLLAR..PUTLLC poll loop is intra-function
         * (all gotos), so it never crosses the SPU_DRAIN tick site -- without a
         * tick here a polling SPU would hold the run token forever while the
         * peer that must WRITE the line waits for it (canersaka ticks the
         * GETLLAR fast+slow paths for exactly this reason). */
        yz_lockstep_tick(ctx);
        spu_lockline_lock();
        /* Tell the PPU store paths this line is live, so a store into it goes
         * through the lock and raises SPU_EVENT_LR here instead of landing
         * unannounced. Nothing else about this transaction changes. */
        spu_coh_reserve(ctx, ea);

        /* ONE read of guest memory, then the snapshot from that copy.
         *
         * This used to memcpy from `mem` twice -- once to the local store, once
         * to resv_line -- and a PPU store landing BETWEEN the two reads gave the
         * SPU a stale line and a fresh snapshot. That is a permanent deadlock,
         * and it is the one this port spent a session chasing: the SPU compares
         * its (stale) copy, finds produced == consumed, and sleeps on
         * MFC_LLR_LOST_EVENT; spu_resv_lost_poll then compares memory against a
         * snapshot that ALREADY has the new value, finds no difference, and
         * never raises the event. Measured at a freeze: mirror 0x9E, snapshot
         * 0x9F, memory 0x9F, and the SPU polling rchcnt 1.6 billion times.
         *
         * Hardware cannot produce that state: GETLLAR is a single atomic
         * 128-byte read, so the reserved data and the reservation come from the
         * same instant. Copying the snapshot from `ls` restores that property,
         * and a store that lands after the read now leaves BOTH stale -- which
         * is what makes the lost-reservation event fire. */
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        if (ctx->policy_mode && (lsa & SPU_LS_MASK) >= 0xEB0u
            && (lsa & SPU_LS_MASK) < 0x12B0u)
            spu_pm_fill_note(ctx, lsa, ls, "getllar");
        /* Snapshot from `ls`, NOT a second read of `mem`. Hardware GETLLAR is a
         * single atomic 128-byte read, so the reserved data and the reservation
         * come from the same instant. Reading guest memory twice lets a PPU store
         * land between them, leaving the SPU a STALE line and a FRESH snapshot --
         * and then both halves of the wait fail: the SPU compares its stale copy,
         * sees produced == consumed and sleeps on MFC_LLR_LOST_EVENT, while the
         * reservation poll compares memory against a snapshot that already holds
         * the new value, finds no difference, and never raises the event. Nobody
         * wakes anybody. Snapshotting from `ls` makes a later store leave BOTH
         * stale, which is exactly what makes the lost-reservation event fire. */
        memcpy(ctx->resv_line, ls, MFC_ATOMIC_LINE);   /* snapshot for compare */
        /* MFC_RdAtomicStat after a GETLLAR reads 4 (GETLLAR complete), not 0:
         * Havok's SPU allocator (GH3 collide task, LS 0xA66C) re-issues the
         * GETLLAR until that bit is set, and spun forever on 0. PUTLLUC is 2. */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 4;
        ctx->dbg_getllar++;
        /* Slow-quantum ring: the PM's atomic polls land here. */
        spu_gread_note(ea);
        /* A GETLLAR re-issued millions of times on one line is an SPU spin-wait
         * that nobody is releasing (a ticket lock, a counter barrier). Say so
         * once per line with the line's head, so the stuck word is visible
         * without a per-function probe. */
        { static SPU_THREAD_LOCAL uint32_t t_ea; static SPU_THREAD_LOCAL unsigned long t_n;
          if (ea != t_ea) { t_ea = ea; t_n = 0; }
          else if (++t_n == 4000000) {
              const uint8_t* m = vm_base + ea;
              fprintf(stderr, "[spu-spin] img=%d pc=0x%05X lr=0x%05X GETLLAR ea=0x%08X x4M; line:"
                              " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea,
                      m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
          } }
        spu_lockline_unlock();
        /* SPU_LLARWATCH=<hex EA>: every GETLLAR of that line, with the LSA it
         * used and the first word as it lands in BOTH places.
         *
         * The deadlock this settles: at a freeze the reservation snapshot holds
         * the fresh produced value (0xC9) while the SPU's local-store mirror
         * still holds the stale one (0xC8) -- and the compare reads the mirror.
         * Both memcpys above copy from the same `mem`, so that can only happen
         * if `lsa` was not where the SPU asked. Reading ctx->mfc_lsa from a
         * later poll cannot prove it (by then the SPU has issued other DMA and
         * overwritten the field, which is exactly the compare-two-moments
         * mistake that has cost this port ten retractions). So log it HERE, at
         * the GETLLAR, with the word that actually landed. */
        { static int s_lw2 = -2; static uint32_t s_lwea;
          if (s_lw2 == -2) { const char* e6 = getenv("SPU_LLARWATCH");
                             s_lw2 = e6 ? 1 : 0;
                             s_lwea = e6 ? (uint32_t)strtoul(e6, 0, 16) & ~127u : 0u; }
          if (s_lw2 && (ea & ~127u) == s_lwea) {
              static unsigned long ln;
              /* A histogram of the LSAs this line's GETLLARs use, not a
               * sample of them. resv_line has exactly one writer -- this
               * GETLLAR -- and it copies to &ls[mfc_lsa] from the same source,
               * so a snapshot that is fresh while LS 0x10800 is stale can only
               * mean some GETLLAR ran with a DIFFERENT mfc_lsa. Sampling every
               * 256th hit would miss exactly those. Count them all and print
               * every LSA seen. */
              { static uint32_t seen[8]; static unsigned long cnt[8]; static int ns;
                int f = -1;
                for (int z = 0; z < ns; z++) if (seen[z] == lsa) { f = z; break; }
                if (f < 0 && ns < 8) { f = ns; seen[ns] = lsa; cnt[ns] = 0; ns++; }
                if (f >= 0) cnt[f]++;
                if ((ln % 4096) == 0) {
                    fprintf(stderr, "[llar-lsa] %lu GETLLARs on 0x%08X;", ln, ea);
                    for (int z = 0; z < ns; z++)
                        fprintf(stderr, " lsa=0x%05X x%lu", seen[z], cnt[z]);
                    fprintf(stderr, "\n"); fflush(stderr);
                } }
              if (++ln <= 8 || (ln % 256) == 0) {
                  const uint8_t* q = (const uint8_t*)ls;
                  const uint32_t landed = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)
                                        | ((uint32_t)q[2] << 8) | q[3];
                  const uint8_t* m8 = (const uint8_t*)mem;
                  const uint32_t frommem = ((uint32_t)m8[0] << 24) | ((uint32_t)m8[1] << 16)
                                         | ((uint32_t)m8[2] << 8) | m8[3];
                  const uint8_t* mir = ctx->ls + 0x10800u;
                  const uint32_t mirw = ((uint32_t)mir[0] << 24) | ((uint32_t)mir[1] << 16)
                                      | ((uint32_t)mir[2] << 8) | mir[3];
                  fprintf(stderr, "[llar] n=%lu spu%u pc=0x%05X lsa=0x%05X"
                                  " ea=0x%08X mem=0x%08X landed=0x%08X"
                                  " mirror[0x10800]=0x%08X r90=0x%08X\n",
                          ln, (unsigned)(ctx->spu_id & 7u),
                          (uint32_t)ctx->pc & SPU_LS_MASK, lsa, ea,
                          frommem, landed, mirw, ctx->gpr[90]._u32[0]);
                  fflush(stderr);
              }
          } }
        return 1;

    case MFC_PUTLLC_CMD:
        g_spu_putllc_count++;   /* run-scoped delta read by the PM flow trace */
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && (ea & ~127u) == (b & ~127u)) g_spu_putllc_sync_hit++; }
        #if PS3_ATOMIC_OBSERVATION
        if (observer) ps3_atomic_observation_begin(&observation, observer,
            PS3_ATOMIC_SPU_STORE, cmd, ea, MFC_ATOMIC_LINE, (uintptr_t)ctx,
            (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        #endif
        spu_lockline_lock();
        #if PS3_ATOMIC_OBSERVATION
        if (observer) memcpy(observation.before, mem, MFC_ATOMIC_LINE);
        #endif
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
              spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLC"); }
            spu_pm_store_note(ctx, ea, lsa, "PUTLLC");
            spu_wp_note(ctx, ea, MFC_ATOMIC_LINE, ls, "putllc");
            { static int s_jc = 0;   /* round 22(d): completion-window
                 * commits from ANY context -- the jobmod's joblist counter
                 * writes are the gate writers; old->new at wedge tells
                 * whether the completion PUTLLC still happens. */
              if (((ea >= 0x400444C0u && ea < 0x40044540u)
                   || (ea >= 0x4004C580u && ea < 0x4004C600u))
                  && (s_jc++ < 24 || (s_jc % 512) == 0)) {
                  const uint32_t* w = (const uint32_t*)ls;
                  const uint32_t* o = (const uint32_t*)mem;
                  fprintf(stderr, "[joblist-c] PUTLLC img=%d ea=0x%08X "
                          "%08X->%08X %08X->%08X\n",
                          ctx->image_id, ea,
                          __builtin_bswap32(o[0]), __builtin_bswap32(w[0]),
                          __builtin_bswap32(o[1]), __builtin_bswap32(w[1]));
                  fflush(stderr);
              } }
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            /* A committing PUTLLC is a line write like any other, so every
             * PEER reservation on it is lost and its SPU takes SPU_EVENT_LR.
             * Silent, this is the same lost update the PPU half was added to
             * close, with an SPU on the writing side: a peer is never told to
             * re-read, so it goes on polling a line it believes it still owns,
             * and a peer parked on RdEventStat sleeps through the commit it
             * was waiting for.
             *
             * Our own reservation is dropped FIRST so the notify does not
             * raise a self-LR: hardware CONSUMES the reservation on a
             * successful PUTLLC, it does not report it lost. Already under the
             * lock-line lock, which is what spu_coh_notify_write expects. */
            ctx->resv_valid = 0;
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 100000) == 1)
                      /* ...and the first four words of the line being written.
                       * A succeeding PUTLLC that repeats millions of times is a
                       * livelock, and the only useful question is what the SPU
                       * keeps seeing there -- the count alone cannot answer it. */
                      { uint32_t w[8] = {0,0,0,0,0,0,0,0};
                        for (int _i = 0; _i < 8; _i++) {
                            uint32_t t = 0;
                            memcpy(&t, (const unsigned char*)ctx->ls + ((lsa + _i*4) & SPU_LS_MASK), 4);
                            w[_i] = __builtin_bswap32(t);
                        }
                        fprintf(stderr, "[putllc-ok] %llu: img=%d ctx=%p ea=0x%08X"
                                        " line=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                                n, ctx->image_id, (void*)ctx, ea,
                                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); } } }
            spu_coh_notify_write(ea);
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
            /* SPU_PUTLLC_WHY=1: which of the three conditions failed.
             * A PUTLLC retry loop that never succeeds is a livelock, and
             * "failed" alone cannot distinguish a lost reservation from a line
             * another agent is rewriting -- opposite causes, opposite fixes. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) {
                  static unsigned long long n_noresv, n_ea, n_line;
                  if (!ctx->resv_valid)            n_noresv++;
                  else if (ctx->resv_ea != ea)     n_ea++;
                  else                             n_line++;
                  static unsigned long long total;
                  if ((++total % 500000) == 0) {
                      fprintf(stderr, "[putllc-why] %llu fails: no-reservation=%llu"
                                      " ea-mismatch=%llu line-changed=%llu"
                                      " (last ea=0x%08X img=%d resv_ea=0x%08X"
                                      " getllar=%llu)\n",
                              total, n_noresv, n_ea, n_line, ea, ctx->image_id,
                              ctx->resv_ea, (unsigned long long)ctx->dbg_getllar);
                      fflush(stderr);
                  }
              } }
        }
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
              static int _n = 0;
              if (_n++ < 96 && ctx->atomic_stat == 0) {
                  uint32_t sn = ((uint32_t)ctx->ls[0x1C8]<<24)|((uint32_t)ctx->ls[0x1C9]<<16)|
                                ((uint32_t)ctx->ls[0x1CA]<<8)|ctx->ls[0x1CB];
                  extern uint8_t* vm_base;
                  const uint8_t* r = vm_base + b + 0x70;   /* row 3 (sync+0x40+16*3) */
                  fprintf(stderr, "[sync-atomic] PUTLLC-OK spuNum=%u ea=+0x%X lanes@row3={%u %u %u %u %u %u %u}\n",
                          sn, ea - b,
                          (r[2]<<8)|r[3],(r[4]<<8)|r[5],(r[6]<<8)|r[7],(r[8]<<8)|r[9],
                          (r[10]<<8)|r[11],(r[12]<<8)|r[13],(r[14]<<8)|r[15]);
              }
          } }
        /* SPU_ATOM_EA: the [atom-ea] line above prints every ATTEMPT, before
         * the compare. Say which of them committed, or a retried failure reads
         * as a lost update. */
        { static int s_v = -1; static uint32_t s_vl;
          if (s_v < 0) { const char* e = getenv("SPU_ATOM_EA");
                         s_vl = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_v = s_vl ? 1 : 0; }
          if (s_v && (ea & ~127u) == s_vl)
              fprintf(stderr, "[atom-ea] PUTLLC %s lr=0x%05X ea=0x%08X\n",
                      ctx->atomic_stat ? "FAIL" : "OK",
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea); }
        ctx->resv_valid = 0;                           /* reservation consumed */
        #if PS3_ATOMIC_OBSERVATION
        if (observer) memcpy(observation.after, mem, MFC_ATOMIC_LINE);
        #endif
        spu_lockline_unlock();
        #if PS3_ATOMIC_OBSERVATION
        if (observer) ps3_atomic_observation_end(&observation, observer,
            PS3_ATOMIC_ATTEMPTED | (ctx->atomic_stat == 0 ? PS3_ATOMIC_COMMITTED : 0));
        #endif
        if (ctx->atomic_stat == 0 && g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        if (ctx->atomic_stat == 0 && g_spu_host_line_commit_hook) g_spu_host_line_commit_hook(ea & ~127u);
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        #if PS3_ATOMIC_OBSERVATION
        if (observer) ps3_atomic_observation_begin(&observation, observer,
            PS3_ATOMIC_SPU_STORE, cmd, ea, MFC_ATOMIC_LINE, (uintptr_t)ctx,
            (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        #endif
        spu_lockline_lock();
        #if PS3_ATOMIC_OBSERVATION
        if (observer) memcpy(observation.before, mem, MFC_ATOMIC_LINE);
        #endif
        { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
          spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLUC"); }
        spu_pm_store_note(ctx, ea, lsa, "PUTLLUC");
        spu_wp_note(ctx, ea, MFC_ATOMIC_LINE, ls, "putlluc");
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        /* Unconditional, so it invalidates EVERY reservation on the line --
         * this SPU's included, which is why the notify runs before the
         * bookkeeping below rather than after it. A peer left holding a
         * reservation here would commit a PUTLLC against a snapshot this store
         * has already overwritten. */
        spu_coh_notify_write(ea);
        ctx->resv_valid = 0; ctx->atomic_stat = 2;   /* PUTLLUC complete */
        #if PS3_ATOMIC_OBSERVATION
        if (observer) memcpy(observation.after, mem, MFC_ATOMIC_LINE);
        #endif
        spu_lockline_unlock();
        #if PS3_ATOMIC_OBSERVATION
        if (observer) ps3_atomic_observation_end(&observation, observer,
            PS3_ATOMIC_ATTEMPTED | PS3_ATOMIC_COMMITTED);
        #endif
        if (g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        if (g_spu_host_line_commit_hook) g_spu_host_line_commit_hook(ea & ~127u);
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
static void spu_wrch_channel(spu_context* ctx, uint32_t channel, u128 value)
{
    /* SPU_CHHIST=1: which channels a job actually touches, and how often. A
     * worker that never issues an MFC command is either not being handed its
     * work descriptor or is waiting on a channel we never satisfy; the channel
     * mix distinguishes those. */
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long w[128]; static unsigned long long n;
          w[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chw] %llu writes:%c", n, 10);
              for (int i = 0; i < 128; i++) if (w[i])
                  fprintf(stderr, "   wrch ch%-3d %llu%c", i, w[i], 10); } } }
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        /* MFC tag-status-update EVENT producer (channel-stall milestone). Our
         * DMA completes synchronously, so the instant the program arms tag
         * notification (MFC_WrTagUpdate with a non-zero mode = any/all), the
         * tags in MFC_WrTagMask are already complete -- pend the tag event
         * (bit 0, MFC_TAG_STATUS_UPDATE_EVENT) when the SPU has it enabled, and
         * wake any host thread blocked in rdch SPU_RdEventStat. This is the
         * long-missing producer for event_status (previously only ever cleared
         * via WrEventAck -> RdEventStat waits could never complete). */
        if (channel == MFC_WrTagUpdate && v != 0 && (ctx->event_mask & 0x1u)) {
            spu_lockline_lock();
            ctx->event_status |= 0x1u;
            spu_lockline_unlock();
            spu_ch_wake(ctx);
        }
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        /* A raw SPU's outbound mailbox is one entry deep and wrch stalls while
         * it is full. Buffering instead let two replies queue up, and a PPU
         * that tests bit 0 of the count -- Tornado Outbreak's boot handshake,
         * func_000D5A10 -- read count 2 as empty and spun forever (an
         * intermittent boot hang). Raw SPUs only: their reader is the PPU
         * polling the MMIO window, whereas SPURS-thread mailboxes are drained
         * by paths that do not wake a stalled writer. A raw SPU's local store
         * is the guest window, not its own buffer. */
        if (ctx->ls != ctx->ls_store)
            while (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && ctx->status != SPU_STATUS_STOPPED)
                Sleep(0);
        spu_channel_write(&ctx->ch_out_mbox, v);
        /* SPU_DBG_MBOX=1: depth after the write. The image-1 SPUs publish their
         * LS work buffer here (LS 0x11D4) and then poll it, and the guest sets
         * class-2 mask 0x3 -- which excludes the plain-mailbox bit 0x10 -- so
         * this write raises no interrupt and the PPU must POLL to see it. If the
         * depth climbs and stays, nobody is polling and the message is stranded.
         * That is the difference between "the PPU is slow" and "the PPU never
         * looks", which no other probe here distinguishes. */
        { static int _d = -1; if (_d < 0) _d = getenv("SPU_DBG_MBOX") ? 1 : 0;
          if (_d) { static unsigned long _n = 0; if (++_n <= 24)
            fprintf(stderr, "[spu-outmbox] spu=%X wrote 0x%08X depth=%u\n",
                    ctx->spu_id, v, (unsigned)ctx->ch_out_mbox.count); } }
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] OUT  grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        /* Plain mailbox data is consumed by the following interrupt request. */
        break;
    case SPU_WrOutIntrMbox:
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] INTR grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_user_event_hook && g_spu_user_event_hook(ctx, v)) break;
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:
        /* Under the lock-line lock, because every producer of the LR bit sets
         * it under that lock: the PPU coherent store, and a peer SPU's PUTLLC,
         * PUTLLUC or plain PUT. A bare read-modify-write here can read
         * event_status, have a concurrent |= SPU_EVENT_LR land in between, and
         * write the stale value back. The edge is then gone, and an SPU that
         * has just acknowledged the events it read goes back to sleep believing
         * it still holds a reservation it has already lost, which is the parked
         * SPURS kernel this whole mechanism exists to wake. */
        spu_lockline_lock();
        ctx->event_status &= ~v;
        spu_lockline_unlock();
        break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel-stall contract (faithful-adopt, from canersaka's fork).
 *
 * A blocking spu_rdch on an empty read channel parks the SPU host thread on a
 * per-SPU CV until a producer (mailbox/signal write, event raise) calls
 * spu_ch_wake -- never fabricating a value. Under SPU_LOCKSTEP the wait
 * releases the run token first (block_begin) so a peer SPU that must post the
 * awaited data is never blocked on this ctx. A 10 ms re-poll is the missed-wake
 * safety net. Opt-in (env SPU_CH_BLOCK=1), default OFF while the producers
 * (mailbox/signal writes, MFC tag-status event raise) are being wired -- until
 * then blocking a read whose producer is missing would hang, so default stays
 * legacy non-blocking with zero regression, exactly like the lockstep gate.
 * ===========================================================================*/
/* Set by runtime/spu/spu_raw.c once a raw SPU exists. A raw SPU runs on its own
 * host thread against a PPU that pokes its mailboxes by MMIO, so an empty
 * RdInMbox must PARK, not fabricate a zero -- there is no scheduler above it to
 * retry. Off unless a raw SPU is actually created, so no other port changes. */
int g_spu_force_ch_block = 0;

static int yz_ch_block(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SPU_CH_BLOCK") ? 1 : 0;
    return v || g_spu_force_ch_block;
}

/* Would rdch complete right now? Plain reads + the 10 ms re-poll cover cross-
 * thread visibility (x86 TSO + the wait syscall's barrier); the s43 atomics are
 * a later refinement. */
/* MFC_LLR_LOST_EVENT (0x400) -- the only producer for it.
 *
 * The SPU "wait until another processor touches this variable" idiom is: GETLLAR
 * the 128-byte line, set the event mask to Lr, then block in
 * `rdch SPU_RdEventStat` until the reservation is lost. Nothing ever raised that
 * bit, so an SPU using it waited forever -- ps1_netemu's GPU core (SPU 4) parks
 * at pc=0x0A5E8 with evmask=0x400 exactly here while the PPU spins waiting on the
 * SPU. Deadlock, and the reason the PS1 core never starts.
 *
 * Losing a reservation means the reserved line changed, and GETLLAR already
 * snapshots it, so compare against that snapshot rather than hooking every store.
 * ponytail: polled, not store-hooked -- one 128-byte memcmp per 10 ms poll on an
 * already-blocked SPU, and nothing at all on the PPU store path. The ceiling is
 * one poll of wake latency, and an A-B-A write that restores the same bytes
 * between polls is missed; hook the writers if either ever matters.
 */
static void spu_resv_lost_poll(spu_context* ctx)
{
    if (!(ctx->event_mask & 0x400u) || !ctx->resv_valid) return;
    if (!vm_base || ctx->resv_ea == 0) return;
    if (memcmp(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0) {
        ctx->resv_valid = 0;
        ctx->event_status |= 0x400u;
    }
}

static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    if (channel == SPU_RdEventStat) spu_resv_lost_poll(ctx);
    switch (channel) {
    case SPU_RdInMbox:      return ctx->rcv_evt_n != 0 || ctx->ch_in_mbox.count != 0;
    case SPU_RdSigNotify1:  return ctx->ch_sig_notify[0].count != 0;
    case SPU_RdSigNotify2:  return ctx->ch_sig_notify[1].count != 0;
    case SPU_RdEventStat:   return (ctx->event_status & ctx->event_mask) != 0;
    default:                return 1;   /* non-blocking channels: always ready */
    }
}

/* Signal the per-SPU wait CV so a blocked spu_rdch re-checks its predicate.
 * MUST be called by every producer AFTER it makes a read predicate true. */
void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
    WakeAllConditionVariable((CONDITION_VARIABLE*)&ctx->ch_wait_cv);
}

/* Block the calling SPU host thread until `channel` is readable. */
static void spu_ch_wait(spu_context* ctx, uint32_t channel, const char* op)
{
    if (!yz_ch_block() || spu_ch_ready(ctx, channel)) return;

    { static unsigned long bn = 0; unsigned long n = ++bn;
      if (n <= 50 || (n % 512) == 0)
        fprintf(stderr, "[ch-block] spu=%X pc=0x%05X op=%s ch=%u evstat=0x%X evmask=0x%X\n",
                ctx->spu_id, ctx->pc & SPU_LS_MASK, op, channel,
                ctx->event_status, ctx->event_mask); }

    ctx->status = SPU_STATUS_WAITING_CHANNEL;
    yz_lockstep_block_begin(ctx);          /* release the run token before the OS wait */
    {
        unsigned long long start = GetTickCount64(), next_hb = 2000;
        while (!spu_ch_ready(ctx, channel)) {
            AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            if (!spu_ch_ready(ctx, channel))
                SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                          (SRWLOCK*)&ctx->ch_wait_lock, 10, 0);
            ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            unsigned long long waited = GetTickCount64() - start;
            if (waited >= next_hb) {
                fprintf(stderr, "[ch-wait] spu=%X pc=0x%05X ch=%u waited=%llums evstat=0x%X evmask=0x%X resv[valid=%d ea=0x%08X]\n",
                        ctx->spu_id, ctx->pc & SPU_LS_MASK, channel, waited,
                        ctx->event_status, ctx->event_mask,
                        ctx->resv_valid, ctx->resv_ea);
                fflush(stderr);
                next_hb = ((waited / 2000) + 1) * 2000;
            }
        }
    }
    yz_lockstep_block_end(ctx);            /* rejoin the rotation, reacquire the token */
    ctx->status = SPU_STATUS_RUNNING;
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
static u128 spu_rdch_channel(spu_context* ctx, uint32_t channel)
{
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long r[128]; static unsigned long long n;
          r[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chr] %llu reads:%c", n, 10);
              for (int i = 0; i < 128; i++) if (r[i])
                  fprintf(stderr, "   rdch ch%-3d %llu%c", i, r[i], 10); } } }
    /* Block (never fabricate) on an empty producer-fed read channel (opt-in
     * SPU_CH_BLOCK). RdEventStat now has a producer (the MFC tag-status event
     * raise above), but only block it when the SPU has actually enabled events
     * (event_mask != 0) -- a masked-off read must return 0 immediately, not
     * park forever. RdInMbox/RdSigNotify park on their PPU producers. */
    if (channel == SPU_RdInMbox || channel == SPU_RdSigNotify1 || channel == SPU_RdSigNotify2)
        spu_ch_wait(ctx, channel, "rdch");
    else if (channel == SPU_RdEventStat && ctx->event_mask != 0)
        spu_ch_wait(ctx, channel, "rdch");

    uint32_t v = 0;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_c[10] = {0}; static uint64_t s_tot = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdSigNotify1)?1:(channel==SPU_RdSigNotify2)?2:
                (channel==SPU_RdDec)?3:(channel==SPU_RdEventStat)?4:(channel==SPU_RdEventMask)?5:
                (channel==MFC_RdTagStat)?6:(channel==MFC_RdAtomicStat)?7:(channel==SPU_RdMachStat)?8:9;
        s_c[b]++;
        if ((++s_tot % 2000000) == 0)
          fprintf(stderr, "[rdch] InMbox=%llu Sig1=%llu Sig2=%llu Dec=%llu EvStat=%llu EvMask=%llu TagStat=%llu AtomStat=%llu MachStat=%llu other=%llu\n",
            (unsigned long long)s_c[0],(unsigned long long)s_c[1],(unsigned long long)s_c[2],(unsigned long long)s_c[3],
            (unsigned long long)s_c[4],(unsigned long long)s_c[5],(unsigned long long)s_c[6],(unsigned long long)s_c[7],
            (unsigned long long)s_c[8],(unsigned long long)s_c[9]); } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->rcv_evt_n > 0) { v = ctx->rcv_evt[ctx->rcv_evt_i++]; ctx->rcv_evt_n--; }
        else v = spu_channel_read(&ctx->ch_in_mbox);
        break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    /* The decrementer must actually DECREMENT. Returning the latched WrDec
     * value verbatim makes every `rdch $ch8` deadline poll spin forever: the
     * elapsed-time term is always zero, so the deadline never passes. LBP's
     * wwsjob SPURS policy module hangs on exactly that at LS 0x2D68:
     *
     *     2d6c:  lqa   $2, 0x1530     ; deadline
     *     2d74:  rdch  $5, $ch8       ; now  <- always read 0
     *     2d78:  sf    $6, $2, $5     ; now - deadline
     *     2d80:  cgti  $5, $6, 0
     *     2d88:  binz  $2, $0         ; leave once it goes positive
     *
     * It never leaves, never reaches its first DMA, and blocks the workload.
     * Wraparound on subtract is deliberate -- it is what hardware does, and the
     * `cgti > 0` test above is written to tolerate it. */
    case SPU_RdDec: {
        /* Never armed by a WrDec: stamp the base on first read rather than
         * measuring from epoch 0, which would subtract the host's entire
         * uptime and hand back a wild value to code that has every right to
         * read the decrementer before writing it. */
        if (!ctx->dec_base_ns) ctx->dec_base_ns = spu_host_ns();
        uint64_t elapsed = spu_host_ns() - ctx->dec_base_ns;
        /* Split seconds from the remainder before scaling: elapsed_ns * 79.8M
         * overflows 64 bits 231 s after the base stamp, and a context that
         * never writes its decrementer (the SPURS kernel's) keeps the stamp
         * from boot, so every timed loop on that SPU saw the counter jump
         * forward by ~16 s of ticks at 231 s, 462 s, 693 s... */
        const uint64_t secs = elapsed / 1000000000ull, rem = elapsed % 1000000000ull;
        uint32_t ticks   = (uint32_t)(secs * SPU_DECREMENTER_HZ +
                                      (rem * SPU_DECREMENTER_HZ) / 1000000000ull);
        v = ctx->decrementer - ticks;
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   spu_resv_lost_poll(ctx); v = ctx->event_status; break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
static uint32_t spu_rchcnt_channel(spu_context* ctx, uint32_t channel)
{
    /* SPU_WHOPOLLS=<n>: rchcnt/park accounting PER SPU, printed every n calls
     * with the pc each SPU is sitting at.
     *
     * SPU_CHHIST sums every SPU into one histogram, which is exactly the wrong
     * shape for a deadlock: it showed rchcnt ch0/ch4 climbing past 1.7 billion
     * while MFC_WrTagUpdate (ch23) stayed frozen, i.e. somebody polls forever
     * and somebody else stopped doing DMA -- without saying whether that is one
     * SPU or two. The freeze under investigation is the PPU-side spin in
     * func_000D2298 waiting for spu4 to advance a counter at 0x002DF008, so
     * which SPU is parked, and on what, is the whole question. */
    { static int s_w = -1;
      if (s_w < 0) { const char* e = getenv("SPU_WHOPOLLS");
        s_w = e ? (atoi(e) > 0 ? atoi(e) : 4000000) : 0; }
      if (s_w) { static unsigned long long c[8][40]; static unsigned long long n;
          static uint32_t lastpc[8], lastst[8], lastmask[8];
          static uint32_t lastsig[8][2], lastmb[8], lastrea[8];
          static int lastrv[8], lastrd[8];
          static uint32_t lastline[8][8], lastls[8][8];
          static uint32_t lastlsa[8], lastr90[8];
          const unsigned sp = (unsigned)(ctx->spu_id & 7u);
          if (channel < 40u) c[sp][channel]++;
          lastpc[sp] = (uint32_t)ctx->pc & SPU_LS_MASK;
          lastst[sp] = ctx->event_status; lastmask[sp] = ctx->event_mask;
          lastsig[sp][0] = ctx->ch_sig_notify[0].count;
          lastsig[sp][1] = ctx->ch_sig_notify[1].count;
          lastmb[sp] = ctx->ch_in_mbox.count;
          lastrv[sp] = ctx->resv_valid; lastrea[sp] = ctx->resv_ea;
          lastlsa[sp] = ctx->mfc_lsa & SPU_LS_MASK;
          lastr90[sp] = ctx->gpr[90]._u32[0];
          lastrd[sp] = (ctx->resv_valid && vm_base && ctx->resv_ea)
                     ? (memcmp(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0)
                     : -1;
          { static int s_ld2 = -2; static uint32_t s_lo2;
            if (s_ld2 == -2) { const char* e5 = getenv("SPU_LSDUMP");
                               s_ld2 = e5 ? 1 : 0;
                               s_lo2 = e5 ? (uint32_t)strtoul(e5,0,16) : 0u; }
            if (s_ld2 && ctx->ls)
                for (int z = 0; z < 8; z++) {
                    const uint8_t* l8 = ctx->ls + ((s_lo2 + z * 4) & SPU_LS_MASK);
                    lastls[sp][z] = ((uint32_t)l8[0] << 24) | ((uint32_t)l8[1] << 16)
                                  | ((uint32_t)l8[2] << 8) | l8[3];
                } }
          for (int z = 0; z < 8; z++) {
              const uint8_t* b8 = ctx->resv_line + z * 4;
              lastline[sp][z] = ((uint32_t)b8[0] << 24) | ((uint32_t)b8[1] << 16)
                              | ((uint32_t)b8[2] << 8) | b8[3];
          }
          if ((++n % (unsigned long long)s_w) == 0) {
              fprintf(stderr, "[whopolls] %llu rchcnt calls\n", n);
              for (unsigned q = 0; q < 8; q++) {
                  int any = 0;
                  for (unsigned k = 0; k < 40; k++) if (c[q][k]) any = 1;
                  if (!any) continue;
                  /* The channel state each poll is actually testing. Counts
                   * alone cannot say whether a poll returns 0 or 1, and that is
                   * the difference between "the SPU is not being told" and "the
                   * SPU is told and ignores it". */
                  fprintf(stderr, "   spu%u pc=0x%05X ev[st=%08X mask=%08X]"
                                  " sig[%u %u] inmbox=%u", q, lastpc[q],
                          lastst[q], lastmask[q], lastsig[q][0], lastsig[q][1],
                          lastmb[q]);
                  /* The reservation is the other half of an evmask=0x400 wait:
                   * spu_resv_lost_poll returns immediately unless resv_valid,
                   * so a cleared reservation is indistinguishable from "nobody
                   * wrote the line" in event_status alone -- and they are
                   * different bugs. diff says whether the line has actually
                   * changed under the snapshot right now. */
                  /* mfc_lsa and r90 alongside the reservation. GETLLAR
                   * copies the line to &ls[mfc_lsa] and to resv_line from the
                   * same source, so a snapshot that disagrees with the local
                   * store can only mean mfc_lsa was not what the SPU asked for
                   * -- and the SPU computes both that LSA and the address its
                   * compare reads from r90. Printing the two together is what
                   * decides it. */
                  fprintf(stderr, " resv[v=%d ea=0x%08X diff=%d]"
                                  " mfc_lsa=0x%05X r90=0x%08X",
                          lastrv[q], lastrea[q], lastrd[q],
                          lastlsa[q], lastr90[q]);
                  /* And the line itself. "The reservation is intact and the
                   * line has not changed" still does not say what the SPU is
                   * waiting FOR; the words do. Snapshot side, so it is exactly
                   * what the SPU last read. */
                  /* SPU_LSDUMP=<hex LS offset>: 8 words of this SPU's local
                   * store. The reserved line says what the SPU can SEE; its LS
                   * says what it BELIEVES. For the spu4 deadlock those are the
                   * two numbers to compare -- its consumed counter is staged at
                   * LS 0x10888 (the counter PUT's lsa 0x10880, +8). */
                  { static int s_ld = -2; static uint32_t s_lo;
                    if (s_ld == -2) { const char* e4 = getenv("SPU_LSDUMP");
                                      s_ld = e4 ? 1 : 0;
                                      s_lo = e4 ? (uint32_t)strtoul(e4,0,16) : 0u; }
                    if (s_ld) {
                        fprintf(stderr, " ls[0x%05X:", s_lo);
                        for (int z = 0; z < 8; z++)
                            fprintf(stderr, " %08X", lastls[q][z]);
                        fprintf(stderr, "]");
                    } }
                  if (lastrv[q]) {
                      fprintf(stderr, " line[");
                      for (int z = 0; z < 8; z++)
                          fprintf(stderr, "%s%08X", z ? " " : "", lastline[q][z]);
                      fprintf(stderr, "]");
                  }
                  for (unsigned k = 0; k < 40; k++) if (c[q][k])
                      fprintf(stderr, " ch%u=%llu", k, c[q][k]);
                  fprintf(stderr, "\n");
              }
              fflush(stderr);
          } } }
    /* SPU_CHHIST also covers rchcnt. It used to instrument only wrch/rdch,
     * which is the one place a parked persistent worker is guaranteed NOT to
     * appear: park_on_empty_inmbox halts from inside THIS function, so a
     * worker that polls rchcnt and parks produced a completely empty channel
     * histogram and read as 'never touches a channel'. */
    { static int s_c = -1;
      if (s_c < 0) { const char* e = getenv("SPU_CHHIST");
        s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long c[128]; static unsigned long long n;
          c[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) {
              fprintf(stderr, "[chc] %llu rchcnt:%c", n, 10);
              for (int i = 0; i < 128; i++) if (c[i])
                  fprintf(stderr, "   rchcnt ch%-3d %llu%c", i, c[i], 10);
              fflush(stderr); } } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_cnt[8] = {0}; static uint64_t s_total = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdEventStat)?1:(channel==SPU_RdSigNotify1)?2:
                (channel==SPU_RdSigNotify2)?3:(channel==MFC_RdTagStat)?4:(channel==SPU_WrOutMbox)?5:
                (channel==SPU_WrOutIntrMbox)?6:7;
        s_cnt[b]++;
        if ((++s_total % 2000000) == 0)
          fprintf(stderr, "[pollcnt] InMbox=%llu EvStat=%llu Sig1=%llu Sig2=%llu TagStat=%llu OutMbox=%llu OutIntr=%llu other=%llu (ch last=%u)\n",
                  (unsigned long long)s_cnt[0],(unsigned long long)s_cnt[1],(unsigned long long)s_cnt[2],
                  (unsigned long long)s_cnt[3],(unsigned long long)s_cnt[4],(unsigned long long)s_cnt[5],
                  (unsigned long long)s_cnt[6],(unsigned long long)s_cnt[7], channel); } }
    switch (channel) {
    case SPU_RdInMbox:
        /* Synchronous persistent-worker park: after its handshake the SPU polls
         * the inbound mailbox for PPU commands; if empty and parking is armed,
         * halt (longjmp out of spu_run_with_halt) instead of spinning forever. */
        /* A pending sys_spu_thread_receive_event reply counts as readable
         * mailbox words: the worker checks rchcnt to decide whether its reply
         * has arrived, and parking on an "empty" inbox that actually holds the
         * reply strands it. */
        if (ctx->park_on_empty_inmbox && ctx->rcv_evt_n == 0 &&
            ctx->ch_in_mbox.count == 0) {
            extern void spu_halt(spu_context*);
            spu_halt(ctx);
        }
        return (uint32_t)ctx->rcv_evt_n + ctx->ch_in_mbox.count;           /* readable */
    /* Free slots, CLAMPED. The queue behind each channel is SPU_CHANNEL_CAP
     * deep -- deeper than the hardware depth reported here -- so `count` can
     * exceed the depth and a bare subtraction underflows, telling the SPU it
     * has four billion free slots. That silently converts a bounded mailbox
     * into an unbounded one for any title that polls before writing, which is
     * every SPU-heavy one. */
    case SPU_WrOutMbox:
        return ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_MBOX_DEPTH - ctx->ch_out_mbox.count);
    case SPU_WrOutIntrMbox:
        return ctx->ch_out_intr_mbox.count >= SPU_INTR_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count);
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    /* An enabled event pending, or 0 -- NOT the `default: 1` this used to fall
     * through to. The SPU idiom is `rchcnt SPU_RdEventStat; brnz -> rdch`: it
     * asks whether an event is pending and only commits to the BLOCKING read if
     * the answer is yes. Answering 1 unconditionally lured it into a read that
     * then parked forever, because rdch correctly blocks while
     * (event_status & event_mask) == 0 -- rchcnt and rdch disagreed.
     *
     * ps1_netemu's audio SPU does exactly this at LS 0x8934 and parked at
     * 0x0A5E8 waiting for MFC_LLR_LOST_EVENT on a line nothing was ever going
     * to touch; on hardware it would simply have fallen through to 0x893C and
     * kept working. Same condition as spu_ch_ready's case, including the
     * lost-reservation poll, so the two now agree by construction. */
    case SPU_RdEventStat:
        spu_resv_lost_poll(ctx);
        return (ctx->event_status & ctx->event_mask) != 0;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    /* Stall-and-notify status is a single-value channel: count is 1 while a
     * newly-stalled tag is pending to be read, else 0. The wwsjob interrupt
     * handler gates on this (rchcnt $ch25; brz IhcExit) -- returning the old
     * default of 1 made it enter on spurious interrupts with an empty stat,
     * running IhcLoop on a zero mask (clz(0) -> bogus tagId/ack). */
    case MFC_RdListStallStat: return ctx->list_stall_stat ? 1 : 0;
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

/* One entry per lifted SPU function across EVERY registered image, and a game
 * that lifts its whole SPU workload set has a lot of them: Yakuza: Dead Souls
 * registers ~170k (cri_audio alone is ~35k, gs_task ~9k, the job binaries ~11k).
 * At 65536 the registry silently truncated every image registered past the cap
 * -- and the SPURS job-chain policy, the job binaries and the Edge geometry
 * task (gs_task) register LAST, so their functions were dropped wholesale and
 * every indirect branch into them fell through to a branch-to-0. Size it for
 * the real workload; the overflow is now loud (see spu_register_function). */
#define SPU_FN_REGISTRY_MAX 262144
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Accessor for the sampling profiler in ppu_loader.cpp. Its address->function
 * map is built from the PPU table alone, which leaves the SPU-lifted bodies as
 * unowned gaps -- and an [entry, next_entry) extent then charges every SPU
 * sample to whichever PPU function happens to precede it. That produced a
 * confident and completely wrong "99% of guest time in one PPU function".
 * Handing the SPU host pointers over lets the map cover both, so the gaps are
 * real entries and the extents mean something. */
uint32_t spu_registry_size(void) { return s_registry_count; }

int spu_registry_entry(uint32_t i, void** host, uint32_t* ls_addr)
{
    if (i >= s_registry_count) return 0;
    if (host)    *host    = (void*)s_registry[i].fn;
    if (ls_addr) *ls_addr = s_registry[i].addr;
    return 1;
}

/* Hash index over the registry. spu_lookup runs on EVERY guest indirect
 * branch -- with the Bink decoder live that is millions of dispatches per
 * second, and the old linear scan (~950 binkspu entries walked per branch)
 * dominated movie playback. Buckets chain in REGISTRATION ORDER (tail
 * append) so the first-registered-match-wins semantics of the linear scan
 * are preserved exactly; the image-id wildcard rules stay in the bucket
 * walk, which is 1-3 entries (same LS addr across overlapping images).
 * Registration is startup-single-threaded; lookups treat the index as
 * read-only. Chain links store index+1 so zero-init means "empty". */
#define SPU_FN_HASH_SIZE 131072   /* power of two, ~= MAX/2 -> load factor ~2 */
static uint32_t s_hash_head[SPU_FN_HASH_SIZE];
static uint32_t s_hash_tail[SPU_FN_HASH_SIZE];
static uint32_t s_hash_next[SPU_FN_REGISTRY_MAX];

static inline uint32_t spu_fn_hash(uint32_t addr)
{
    return ((addr >> 2) * 2654435761u) & (SPU_FN_HASH_SIZE - 1);
}

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count >= SPU_FN_REGISTRY_MAX) {
        /* Silent truncation here dropped whole late-registered SPU images and
         * cost a multi-round hunt (the branch-to-0 looked like a lifter/overlay
         * bug). Never again: say so, once, loudly, with the number to raise the
         * cap to. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "[spu] FATAL: SPU function registry full at %u entries "
                    "(SPU_FN_REGISTRY_MAX=%u) -- image %d function 0x%05X and all "
                    "later registrations DROPPED. Raise SPU_FN_REGISTRY_MAX.\n",
                    s_registry_count, (unsigned)SPU_FN_REGISTRY_MAX,
                    s_reg_image, addr);
            fflush(stderr);
        }
        return;
    }
    uint32_t i = s_registry_count;
    s_registry[i].addr = addr;
    s_registry[i].fn = fn;
    s_registry[i].image_id = s_reg_image;
    s_registry_count = i + 1;
    uint32_t h = spu_fn_hash(addr);
    s_hash_next[i] = 0;
    if (s_hash_head[h] == 0)
        s_hash_head[h] = i + 1;
    else
        s_hash_next[s_hash_tail[h] - 1] = i + 1;
    s_hash_tail[h] = i + 1;
}

/* Report a CROSS-IMAGE match: the registry served a function some other image
 * registered at this address, because one side's image_id was the 0 wildcard.
 *
 * This is a real hazard, reported with evidence by @canersaka (#59): SPURS job
 * chains load job binaries into descriptor-assigned LS slots, so the same binary
 * can sit at different addresses on different rounds. If the lift was fixed-base
 * and the exact match misses, the wildcard can serve ANOTHER image's function at
 * that address -- the job then runs the wrong program end to end, returns
 * cleanly, and the only symptom is something three layers downstream that never
 * happens. Their words: one log line would have saved a day.
 *
 * Logging, not refusing. Refusing the wildcard outright was measured and does
 * NOT survive contact: a dormant task image can register spans overlapping the
 * resident kernel's low LS range, so a legitimate service-to-kernel yield gets
 * falsely rejected. Telling resident from dormant needs residency tracking the
 * registry does not have. So this reports and keeps going.
 *
 * Deduped per (addr, from, to): spu_lookup runs millions of times a second, and
 * a substitution that repeats every dispatch would bury the log it is meant to
 * make readable. */
static void spu_report_cross_image(uint32_t addr, int want, int got)
{
    enum { SEEN_MAX = 64 };
    static struct { uint32_t addr; int want, got; } seen[SEEN_MAX];
    static unsigned n_seen = 0;
    for (unsigned i = 0; i < n_seen; i++)
        if (seen[i].addr == addr && seen[i].want == want && seen[i].got == got)
            return;
    if (n_seen < SEEN_MAX) {
        seen[n_seen].addr = addr; seen[n_seen].want = want; seen[n_seen].got = got;
        n_seen++;
    }
    fprintf(stderr,
            "[spu] CROSS-IMAGE dispatch: LS 0x%05X requested by image %d, served "
            "by image %d's function (id-0 wildcard). If this job was loaded at a "
            "runtime-chosen base, it may be running the WRONG program -- see "
            "issue #59.\n", addr, want, got);
}

spu_fn spu_lookup(uint32_t addr, int image_id)   /* exported: clang-built fast-path dispatch (spu_dispatch_mt.c) needs it */
{
    /* Match the context's active image; image_id 0 (context or entry) matches
     * any, for back-compat with single-image contexts. */
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr &&
            (image_id == 0 || e->image_id == 0 || e->image_id == image_id)) {
            /* One compare on the return path; the report itself is rare. */
            if (e->image_id != image_id)
                spu_report_cross_image(addr, image_id, e->image_id);
            return e->fn;
        }
    }
    return NULL;
}

/* Round 24: walk-in native handoff helper (spu_interp.c). spu_fn is
 * file-local here, so expose the resolver verdict as an int. */
int spu_walkin_lift(uint32_t pc, int img, int rel)
{
    return spu_lookup(pc - (uint32_t)rel, img) != NULL;
}

/* Which image registered a function at this LS address, or -1 if none did.
 *
 * Deliberately NOT spu_lookup(addr, 0): that call means "dispatch here in a
 * context with no image", so it reports a cross-image substitution for every
 * hit in a real image. This is a question about the registry, not a dispatch,
 * and it runs once per SPU thread start. First match wins, in registration
 * order, so it agrees with which function spu_lookup would actually serve. */
int spu_image_of_function(uint32_t addr)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr) return e->image_id;
    }
    return -1;
}

/* Does a lifted function exist at this LS address (any image)? Lets the lv2
 * layer decide between real SPU execution and the PPU-fallback paths. */
int spu_have_function(uint32_t addr)
{
    return spu_image_of_function(addr) >= 0;
}

/* The pure interpreter's fast-path "is this LSA already lifted?" probe
 * (spu_interp.c, rejoin path). It IS the registry lookup, so overlay eviction and
 * self-modifying-code invalidation are honored automatically. Lived in
 * spu_fn_registry.c before the registry was consolidated into this TU. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    /* Streamed code owns its LS span even when the primary image also has a
     * function at that address. Match indirect dispatch's first-owner rule;
     * an owner miss must not rejoin the primary image's overwritten code. */
    if (ctx) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (ctx->resident_code[slot].image_id &&
                lsa >= ctx->resident_code[slot].lsa &&
                lsa - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size)
                return (spu_lifted_fn)spu_lookup(lsa - ctx->resident_code[slot].rel, ctx->resident_code[slot].image_id);
        }
    }
    spu_lifted_fn fn = (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);
    /* Policy-module images load at a nonzero LS base (SPURS: LS 0xA00) while
     * their lift labels are image-relative. Guest-visible ABSOLUTE targets --
     * the irq vector word at LS 0, srr0 continuations, kernel-context
     * function pointers -- are LS-space, so a miss retries at
     * lsa - img_ls_base. MS1 wwsjob PM: vector bra 0xA2C = label 0x2C,
     * srr0 0x2B98 = mid func_00002190; without the retry the interpreter
     * could never rejoin the lift and the PM run wedged. */
    if (!fn && ctx && ctx->policy_mode && ctx->img_ls_base &&
        lsa >= ctx->img_ls_base)
        fn = (spu_lifted_fn)spu_lookup(lsa - ctx->img_ls_base, ctx->image_id);
    return fn;
}

/* ---- Swappable code overlays (see spu_context.resident_ovl) --------------
 * A title registers each runtime-streamed overlay's SOURCE content EA with
 * the image id its lifted functions were registered under. The MFC GET path
 * marks that overlay resident in the streaming context; dispatch retries a
 * primary-image miss against the resident overlay's registry. */
typedef struct { uint32_t src_ea; int image_id; uint8_t sig[16]; int has_sig; uint32_t span; } spu_ovl_src;
/* 6 FMOD codec/DSP overlays + up to 89 WWS job-code modules (all stream into
 * the same job code buffer at LS 0x4000, dispatched by content signature). */
#define SPU_OVL_SRC_MAX 128
static spu_ovl_src s_ovl_src[SPU_OVL_SRC_MAX];
static int s_ovl_src_count = 0;

void spu_overlay_register_source(uint32_t content_ea, int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = content_ea;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        s_ovl_src[s_ovl_src_count].has_sig = 0;
        s_ovl_src_count++;
    }
}

/* Register a bounded code image that may coexist with other streamed images.
 * Its functions must be translated at the local-store addresses used by the title. */
void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id)
{
    if (!span || span > SPU_LS_SIZE || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_overlay_register_source(content_ea, image_id);
    s_ovl_src[s_ovl_src_count - 1].span = span;
}

/* Register one-way runtime entries that replace the guest call stack.
 * Matching the resolved function, not just its LS address, avoids affecting
 * unrelated overlays using the same address. Registration precedes execution. */
static struct { uint32_t entry; int image_id; } s_stack_reset[16];
static unsigned s_stack_reset_count;
void spu_register_stack_reset_entry(uint32_t entry, int image_id)
{
    if (s_stack_reset_count < 16) {
        s_stack_reset[s_stack_reset_count].entry = entry & SPU_LS_MASK;
        s_stack_reset[s_stack_reset_count++].image_id = image_id;
    }
}

/* Also called before direct trampoline transfers: generated calls can enter
 * a stack-switching routine without going through spu_indirect_branch. */
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*))
{
    if (!ctx->host_depth || !s_spu_halt_armed || ctx->policy_mode) return;
    for (unsigned i = 0; i < s_stack_reset_count; ++i)
        if (ctx->pc == s_stack_reset[i].entry &&
            fn == spu_lookup(ctx->pc, s_stack_reset[i].image_id)) {
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
}

/* SPURS taskset TASK entries (see spu_context.resident_task). A taskset can hold
 * several tasks whose lifts share the SAME LS base -- the co-resident task-code
 * region -- so no LS address identifies which task owns it. The title registers
 * each task's ELF ENTRY point with the image id its functions were registered
 * under; spu_indirect_branch adopts that image the moment the policy branches
 * into the entry from outside the region. This is what lets each such task be
 * registered under its own real image id instead of the id-0 wildcard, so a
 * dormant co-resident task can no longer shadow the one the policy launched. */
#define SPU_TASK_ENTRY_MAX 16
static struct { uint32_t entry; int image_id; } s_task_entry[SPU_TASK_ENTRY_MAX];
static int s_task_entry_count = 0;
void spu_taskset_register_task_entry(uint32_t entry, int image_id)
{
    if (s_task_entry_count < SPU_TASK_ENTRY_MAX) {
        s_task_entry[s_task_entry_count].entry = entry & SPU_LS_MASK;
        s_task_entry[s_task_entry_count].image_id = image_id;
        s_task_entry_count++;
    }
}
static int spu_taskset_task_image(uint32_t entry)
{
    for (int i = 0; i < s_task_entry_count; i++)
        if (s_task_entry[i].entry == entry) return s_task_entry[i].image_id;
    return 0;
}
/* Resume PCs and even fresh entry PCs overlap between task ELFs. The
 * taskset policy's TaskInfo identifies which ELF it has actually loaded. */
static struct { uint32_t elf_ea; int image_id, policy_image_id; }
    s_task_elf[SPU_TASK_ENTRY_MAX];
static int s_task_elf_count;
void spu_taskset_register_task_elf(uint32_t elf_ea, int image_id, int policy_image_id)
{
    if (s_task_elf_count < SPU_TASK_ENTRY_MAX) {
        s_task_elf[s_task_elf_count].elf_ea = elf_ea & ~7u;
        s_task_elf[s_task_elf_count].image_id = image_id;
        s_task_elf[s_task_elf_count++].policy_image_id = policy_image_id;
    }
}
static int spu_taskset_resident_image(const spu_context* ctx)
{
    const uint8_t* p = ctx->ls + 0x2794; /* SpursTasksetContext.taskInfo.elf */
    uint32_t elf = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3]) & ~7u;
    for (int i = 0; i < s_task_elf_count; ++i)
        if (s_task_elf[i].elf_ea == elf &&
            s_task_elf[i].policy_image_id == ctx->resident_ovl)
            return s_task_elf[i].image_id;
    return 0;
}
/* Lowest LS address of the shared task-code region: the taskset tasks all lift
 * at LS 0x3000 (below it is the SPURS kernel/policy/context, 0x290..0x2FFF). */
#define SPU_TASKSET_TASK_LO 0x3000u

/* Content-signature variant: FMOD COPIES codec overlays to the heap before
 * streaming them into the swap slot, so the source EA is unknowable ahead of
 * time -- match the first 16 bytes of the streamed content instead. */
void spu_overlay_register_sig(const uint8_t sig[16], int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = 0;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        memcpy(s_ovl_src[s_ovl_src_count].sig, sig, 16);
        s_ovl_src[s_ovl_src_count].has_sig = 1;
        s_ovl_src_count++;
    }
}

/* Called from the MFC GET path after the copy: ls points at the JUST-COPIED
 * bytes. EA match first (exact, cheap), then content signature for sizeable
 * chunks (overlay bodies are >= 0x500 bytes). */
/* SPU_DUMP_OVL=<dir>[,<lsa>]: write out every distinct code body streamed into
 * LS <lsa> (default 0x5000, the job code buffer), so job bodies the title
 * keeps in its own data can be lifted. A body larger than one DMA arrives as
 * consecutive GETs (contiguous EA and LS); those are appended to the same
 * file. One file per source EA: ovl_<ea>.bin. */
static void spu_ovl_dump(spu_context* ctx, uint32_t ea, uint32_t lsa, const uint8_t* ls, uint32_t size)
{
    static int s_on = -1; static char s_dir[400]; static uint32_t s_lsa = 0x5000;
    if (s_on < 0) { const char* e = getenv("SPU_DUMP_OVL"); s_on = e && *e;
        if (s_on) { snprintf(s_dir, sizeof s_dir, "%s", e);
                    char* c = strchr(s_dir, ','); if (c) { *c = 0; s_lsa = (uint32_t)strtoul(c + 1, 0, 0); } } }
    if (!s_on) return;
    static uint32_t s_seen[256]; static unsigned s_n;
    static SPU_TLS uint32_t t_src, t_next_ea, t_next_lsa;
    FILE* f = NULL; char path[512];
    if (lsa == s_lsa && size >= 256) {
        for (unsigned i = 0; i < s_n; i++) if (s_seen[i] == ea) { t_src = 0; return; }
        if (s_n < 256) s_seen[s_n++] = ea;
        t_src = ea;
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, ea);
        f = fopen(path, "wb");
        fprintf(stderr, "[spu-ovl-dump] img=%d body ea=0x%08X size=%u -> %s\n", ctx->image_id, ea, size, path);
    } else if (t_src && ea == t_next_ea && lsa == t_next_lsa) {
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, t_src);
        f = fopen(path, "ab");
    } else { t_src = 0; return; }
    if (f) { fwrite(ls, 1, size, f); fclose(f); }
    t_next_ea = ea + size; t_next_lsa = lsa + size;
}

/* SPU_LOCKGUARD=<hex ea>: the word at that EA is a ticket lock (serving,next
 * halfwords). Log any SPU write that moves either halfword by anything other
 * than 0 or +1 -- a stale whole-line store rolling the lock back. `src` is the
 * new bytes for [ea, ea+size). */
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* src, uint32_t size, const char* what)
{
    static int64_t s_g = -2;
    if (s_g == -2) { const char* e = getenv("SPU_LOCKGUARD"); s_g = e ? (int64_t)strtoul(e, 0, 16) : -1; }
    if (s_g < 0 || !vm_base) return;
    uint32_t g = (uint32_t)s_g;
    if (g < ea || g + 4 > ea + size) return;
    const uint8_t* o = vm_base + g; const uint8_t* n = src + (g - ea);
    unsigned os = (o[0] << 8) | o[1], on = (o[2] << 8) | o[3];
    unsigned ns = (n[0] << 8) | n[1], nn = (n[2] << 8) | n[3];
    int bad = !(ns == os || ns == ((os + 1) & 0xFFFF)) || !(nn == on || nn == ((on + 1) & 0xFFFF));
    if (bad) { static int c; if (c++ < 32)
        fprintf(stderr, "[lockguard] %s img=%d pc=0x%05X ea=0x%08X size=%u: %04X/%04X -> %04X/%04X\n",
                what, ctx ? ctx->image_id : -1, ctx ? (unsigned)(ctx->pc & SPU_LS_MASK) : 0u,
                ea, size, os, on, ns, nn); }
}

/* Round-10: source EAs whose streamed module DID register a resident slot.
 * Mid-stream list-DMA prefixes from the same source then stay silent in the
 * UNRECOGNIZED log (each contiguous prefix is hashed before the full module
 * arrives -- bg_264 showed 0x4000/0x8000/0xC000/0x10000 prefixes of image 210
 * all from src 0x100E6F80). */
static uint32_t s_stage_good_src[8];
static unsigned s_stage_good_n;

/* ---- Staged jobmod identity ----
 * Policy-mode staging bursts are fingerprinted with FNV-1a-64 over every byte
 * of a contiguous burst (the legacy key, offered after each transfer once the
 * burst holds 0x800 bytes). Registered identities are image minus the 0x20
 * trailer, so a loader that pads its last transfer (MS1's MP3 job objects:
 * 0x179B0-byte image staged as 0x17A00) never matches. SPU_JOBMOD_HEADER_FNV=1
 * also offers, exactly once per burst, the key over the span its C0DEC0DE
 * header declares. Configure before workers start; unset or 0 = legacy only.
 * In that mode a burst without the magic at +0x20 is data: no identity has
 * that form, so the runtime hashes no byte of it and offers no key. A burst
 * is sorted when it holds 0x30 bytes. The keys of each other burst do not
 * change. */
/* Hash one transfer of a burst that is not data: every byte into the legacy
 * key, the bytes of the declared span into the header key. Returns 1 when
 * the header key is complete at the end of this call. */
static int stage_hash_bytes(spu_context* ctx, const uint8_t* ls, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
        ctx->stage_fnv = (ctx->stage_fnv ^ ls[i]) * 1099511628211ULL;
    int header_ready = 0;
    if (ctx->stage_hdr_len < ctx->stage_hdr_span) {
        uint32_t take = ctx->stage_hdr_span - ctx->stage_hdr_len;
        if (take > size) take = size;
        for (uint32_t i = 0; i < take; ++i)
            ctx->stage_hdr_fnv = (ctx->stage_hdr_fnv ^ ls[i]) * 1099511628211ULL;
        ctx->stage_hdr_len += take;
        header_ready = ctx->stage_hdr_len == ctx->stage_hdr_span;
    }
    return header_ready;
}
/* Account one policy-mode transfer into LS [lsa, lsa+size). Writes the
 * candidate registry keys, legacy first, and returns how many (0..2). */
static unsigned spu_stage_accumulate(spu_context* ctx, const uint8_t* ls, uint32_t lsa,
                                     uint32_t ea, uint32_t size, int header_identity,
                                     uint64_t keys[2])
{
    if (ctx->stage_hashed == 0 || lsa != ctx->stage_lo + ctx->stage_hashed) {
        ctx->stage_lo = lsa;
        ctx->stage_ea0 = ea;
        ctx->stage_hashed = 0;
        ctx->stage_fnv = 1469598103934665603ULL;
        ctx->stage_hdr_span = !header_identity ? STAGE_SPAN_LEGACY
            : size >= 0x30u ? stage_burst_kind(ls, lsa, 1) : STAGE_SPAN_PENDING;
        ctx->stage_hdr_len = 0;
        ctx->stage_hdr_fnv = 1469598103934665603ULL;
    } else if (ctx->stage_hdr_span == STAGE_SPAN_PENDING && ctx->stage_hashed + size >= 0x30u) {
        ctx->stage_hdr_span = stage_burst_kind(ctx->ls + ctx->stage_lo, ctx->stage_lo, 1);
        if (ctx->stage_hdr_span < STAGE_SPAN_PENDING) /* not data: the bytes that
            landed before the sort belong to the burst as well */
            stage_hash_bytes(ctx, ctx->ls + ctx->stage_lo, ctx->stage_hashed);
    }
    if (ctx->stage_hdr_span == STAGE_SPAN_PENDING) { ctx->stage_hashed += size; return 0; }
    if (ctx->stage_hdr_span > STAGE_SPAN_PENDING) return 0; /* data: no hash, no key */
    const int header_ready = stage_hash_bytes(ctx, ls, size);
    ctx->stage_hashed += size;
    if (ctx->stage_hashed < 0x800u) return 0;
    unsigned n = 0;
    keys[n++] = ctx->stage_fnv;
    if (header_ready && ctx->stage_hdr_fnv != ctx->stage_fnv) keys[n++] = ctx->stage_hdr_fnv;
    return n;
}
/* ---- end staged jobmod identity ---- */

void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    uint32_t lsa = (uint32_t)(ls - ctx->ls);
    spu_ovl_dump(ctx, ea, lsa, ls, size);
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!ctx->resident_code[slot].image_id) continue;
        uint32_t base = ctx->resident_code[slot].lsa;
        uint32_t end = base + ctx->resident_code[slot].size;
        if (lsa < end && lsa + size > base &&
            (lsa < base || ea != ctx->resident_code[slot].source_ea + (lsa - base)))
            ctx->resident_code[slot].image_id = 0;
    }
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit = o->has_sig ? (size >= 512 && memcmp(ls, o->sig, 16) == 0)
                             : (o->src_ea == ea);
        if (hit) {
            if (o->span) {
                if (lsa + o->span > SPU_LS_SIZE) return;
                for (unsigned slot = 0; slot < 4; ++slot) {
                    if (ctx->resident_code[slot].image_id && ctx->resident_code[slot].lsa != lsa)
                        continue;
                    ctx->resident_code[slot].lsa = lsa;
                    ctx->resident_code[slot].size = o->span;
                    ctx->resident_code[slot].source_ea = ea;
                    ctx->resident_code[slot].image_id = o->image_id;
                    ctx->resident_code[slot].rel = 0;
                    return;
                }
                fprintf(stderr, "[spu-ovl] no free resident code span for image %d\n", o->image_id);
                return;
            }
            if (ctx->resident_ovl != o->image_id) {
                ctx->resident_ovl = o->image_id;
                { static int _n = 0; if (_n++ < 32)
                    fprintf(stderr, "[spu-ovl] img=%d streamed overlay src=0x%08X "
                            "-> LS 0x%05X size=%u -> resident ovl image %d%s\n",
                            ctx->image_id, ea, (uint32_t)(ls - ctx->ls), size,
                            ctx->resident_ovl, o->has_sig ? " (sig match)" : ""); }
            }
            return;
        }
    }
    /* [pm-jobmod] The img-200 WWS policy module stages its job binaries at
     * LS 0x4000.. via a burst of plain tag-1 GETs from the SPU-image area,
     * and the per-module source EAs are not overlay-registered. Accumulate
     * FNV-1a-64 over the staged bytes (contiguity-checked) and, on a registry
     * hit (payload fingerprints registered by jobmods_register.c), claim a
     * resident_code slot. source_ea anchors on the burst-start EA, recorded
     * when the burst resets: the GET that lands module byte 0 reads the
     * module's own EA, while the PM's DMA-list staging makes chunk EAs
     * non-linear, so reconstructing from the claiming GET's EA is wrong.
     * The resolver must translate a staged pc into the
     * lift's own address space: rel = staged_lsa - lift_base. C0DEC0DE
     * images declare their link base in the header (+0x28; MS1: 0x4000) and
     * the PM stages them exactly there, so a link-base lift needs no
     * translation (rel 0) while a legacy module-relative lift (base 0) keeps
     * the historical pc - lsa mapping. Without a slot the resolver misses,
     * execution falls to the interpreter span, the job never runs natively,
     * and the PM parks in kExecuteCommands forever (state never 2->3, store
     * phase never commits, the PPU spins on the batch seq). */
    if (ctx->policy_mode && lsa >= 0x4000u && lsa + size <= 0x3E000u) {
        uint64_t keys[2];
        unsigned nkeys = spu_stage_accumulate(ctx, ls, lsa, ea, size,
                                              spu_jobmod_header_fnv_enabled(), keys);
        if (nkeys) {
            int img = 0;
            uint64_t key = keys[0];
            for (unsigned k = 0; k < nkeys && img <= 0; ++k) {
                key = keys[k];
                spu_workload_find_img(key, &img);
            }
            if (img > 0 && key != ctx->stage_fnv) {
                if (spu_jobmod_header_log_enabled())
                    fprintf(stderr, "[pm-jobmod] header identity image=%d at LS 0x%05X"
                                    " span=0x%X staged=0x%X src=0x%08X\n",
                            img, ctx->stage_lo, ctx->stage_hdr_span, ctx->stage_hashed,
                            ctx->stage_ea0);
            }
            if (img > 0) {
                { uint32_t _s0 = ctx->stage_ea0; int _kn = 0;
                  for (unsigned k = 0; k < s_stage_good_n && k < 8; k++)
                      if (s_stage_good_src[k] == _s0) _kn = 1;
                  if (!_kn && _s0) s_stage_good_src[(s_stage_good_n++) & 7u] = _s0; }
                uint32_t lift_base = spu_workload_img_base(key);
                unsigned pick = 4;
                for (unsigned slot = 0; slot < 4; ++slot) {
                    if (!ctx->resident_code[slot].image_id ||
                        ctx->resident_code[slot].lsa == ctx->stage_lo) { pick = slot; break; }
                }
                /* Round-9: all four resident slots held DIFFERENT load
                 * addresses -- the old code silently skipped registration,
                 * the resolver missed, the job fell to an interpreter span
                 * and the PM parked in kExecuteCommands forever (the exact
                 * retire/re-publish livelock bg_263 caught). Attract cycles
                 * load a rotating set of jobmods, so a 5th distinct module
                 * MUST evict. Round-robin: wrong-slot cost is a resolver
                 * miss (interpreted fallback), wrong-eviction cost here is
                 * nothing worse than the status quo. */
                if (pick == 4) {
                    static unsigned s_rr = 0;
                    pick = (s_rr++) & 3u;
                    { static int _e = 0; if (_e++ < 16)
                        fprintf(stderr, "[pm-jobmod] stage slots FULL -- evicting"
                                        " LS 0x%05X (img=%d) for LS 0x%05X\n",
                                ctx->resident_code[pick].lsa,
                                ctx->resident_code[pick].image_id, ctx->stage_lo); }
                }
                if (pick < 4) {
                    ctx->resident_code[pick].lsa = ctx->stage_lo;
                    ctx->resident_code[pick].size = ctx->stage_hashed;
                    ctx->resident_code[pick].source_ea = ctx->stage_ea0;
                    ctx->resident_code[pick].image_id = img;
                    /* Round 23: last staged module window -- the modspan-w
                     * watch in mfc_do_transfer logs any later policy traffic
                     * into [lsa, lsa+size); a restage between registration
                     * and the PM's sequential walk-in serves the walkin the
                     * wrong bytes ([walkin] match=NEITHER). */
                    g_ms_last_lsa = ctx->stage_lo;
                    g_ms_last_size = ctx->stage_hashed;
                    g_ms_last_img = (unsigned)img;
                    g_ms_last_seq++;
                    ctx->resident_code[pick].rel = ctx->stage_lo - lift_base;
                    g_spu_jobmod_staged++;
                    { static int _n = 0; _n++;
                      if (_n <= 16 || (_n % 128) == 0) {
                        fprintf(stderr, "[pm-jobmod] staged image=%d at LS 0x%05X size=0x%X src=0x%08X "
                                "(rel=0x%X, %s)\n",
                                img, ctx->stage_lo, ctx->stage_hashed,
                                ctx->resident_code[pick].source_ea,
                                ctx->resident_code[pick].rel,
                                lift_base ? "link-base lift" : "module-relative lookups");
                        /* M35c probe: the state-2 cmdlist scan is the only
                         * writer of the logicalToBufferNum mapping table (LS
                         * 0x11B0) that the state-3 gather consumes; ours
                         * stayed all-FF sentinel. Dump the cmdlist head
                         * (0xC00), the gather descriptor list (0xEB0), the
                         * mapping table (0x11B0) and the state pair at the
                         * moment the loader staged the job module. */
                        #define DMP16(base) do { \
                            fprintf(stderr, "[pm-jobmod] " #base ":"); \
                            for (unsigned _o = 0; _o < 0x80; _o += 4) \
                                fprintf(stderr, " %08X", \
                                    (unsigned)((ctx->ls[(base)+_o] << 24) | \
                                               (ctx->ls[(base)+_o+1] << 16) | \
                                               (ctx->ls[(base)+_o+2] << 8) | \
                                               ctx->ls[(base)+_o+3])); \
                            fprintf(stderr, "\n"); } while (0)
                        DMP16(0xC00); DMP16(0xEB0); DMP16(0x11B0);
                        #undef DMP16
                        fprintf(stderr, "[pm-jobmod] state 12A0=%08X 12D0=%08X 12F0=%08X 1420=%08X\n",
                                (unsigned)((ctx->ls[0x12A0]<<24)|(ctx->ls[0x12A1]<<16)|(ctx->ls[0x12A2]<<8)|ctx->ls[0x12A3]),
                                (unsigned)((ctx->ls[0x12D0]<<24)|(ctx->ls[0x12D1]<<16)|(ctx->ls[0x12D2]<<8)|ctx->ls[0x12D3]),
                                (unsigned)((ctx->ls[0x12F0]<<24)|(ctx->ls[0x12F1]<<16)|(ctx->ls[0x12F2]<<8)|ctx->ls[0x12F3]),
                                (unsigned)((ctx->ls[0x1420]<<24)|(ctx->ls[0x1421]<<16)|(ctx->ls[0x1422]<<8)|ctx->ls[0x1423]));
                        fflush(stderr); } }
                        /* PM_JOBPARAMS=1: dump the launched jobmod's
                         * parameter record at jobEA+0x200 -- the 0x1F0 bytes
                         * the module GETs (observed: pc 0x4200 fetching
                         * jobEA+0x200). These are the module's AddU32Param /
                         * PVSThreadParams blocks (camera, view, frustum and
                         * resource data for the spupvs module at
                         * 0x100E6F80), not a fixed ABI: field layout varies
                         * per module. Observer-only probe; the job EA is the
                         * second word of the loadJobState quadword at LS
                         * 0x12A0. Capped at 8 dumps. */
                        { static int s_on = -1;
                          if (s_on < 0) s_on = getenv("PM_JOBPARAMS") ? 1 : 0;
                          if (s_on) { static int s_jp = 0;
                            if (s_jp++ < 8) {
                              uint32_t jea = (uint32_t)((ctx->ls[0x12A4] << 24) |
                                                        (ctx->ls[0x12A5] << 16) |
                                                        (ctx->ls[0x12A6] << 8) |
                                                        ctx->ls[0x12A7]);
                              if (vm_base && jea > 0x10000u && jea < 0xF0000000u) {
                                extern uint8_t* vm_base;
                                fprintf(stderr, "[pm-jobmod] jobparams @0x%08X +0x200:", jea);
                                for (unsigned _o = 0; _o < 0x40; _o += 4)
                                    fprintf(stderr, " %08X", (unsigned)
                                        ((vm_base[jea + 0x200 + _o] << 24) |
                                         (vm_base[jea + 0x200 + _o + 1] << 16) |
                                         (vm_base[jea + 0x200 + _o + 2] << 8) |
                                         vm_base[jea + 0x200 + _o + 3]));
                                fprintf(stderr, "\n"); fflush(stderr);
                              }
                            } }
                        }
                        /* M36b: the bind-array candidate EA lives at
                         * cmdlist+0x14 (observed 0x9C580100). Dump 32 bytes
                         * of GUEST memory there: if it holds a {size,ea}
                         * element array (1492*8 = 0x2EA8 bytes == the gather
                         * list byte-size), the missing table builder is a
                         * single GET from this EA. */
                        { extern uint8_t* vm_base;
                          uint32_t arr = (uint32_t)((ctx->ls[0xC14] << 24) |
                                                   (ctx->ls[0xC15] << 16) |
                                                   (ctx->ls[0xC16] << 8) |
                                                   ctx->ls[0xC17]);
                          if (arr > 0x10000u && arr < 0xF0000000u && vm_base) {
                              const uint8_t* g = vm_base + arr;
                          { static int s_m36b = 0;
                          if (s_m36b++ < 4) {
                              fprintf(stderr, "[pm-jobmod] guest@0x%08X:", arr);
                              for (unsigned _o = 0; _o < 0x20; _o += 4)
                                  fprintf(stderr, " %02X%02X%02X%02X",
                                      g[_o],g[_o+1],g[_o+2],g[_o+3]);
                              fprintf(stderr, "\n"); fflush(stderr);
                          } }
                          } }
                        /* M36c: the walker stops at the first op!=0 word
                         * (op = bits[8:5]) and 0xC00 was fetched with only
                         * 0x50 bytes of a ~0x1E8-byte per-job descriptor
                         * (PpuConsumer). Dump the descriptor's +0x50..+0x250
                         * guest region: the op0 bufferSet records for the
                         * gather bind should be in there. */
                        { extern uint8_t* vm_base;
                          uint32_t lnk2 = (uint32_t)((ctx->ls[0xC30] << 24) |
                                                    (ctx->ls[0xC31] << 16) |
                                                    (ctx->ls[0xC32] << 8) |
                                                    ctx->ls[0xC33]);
                          uint32_t base = (lnk2 >= 0x200u) ? (lnk2 - 0x200u) : 0u;
                          if (base > 0x10000u && base < 0xF0000000u && vm_base) {
                              const uint8_t* g = vm_base + base;
                              { static int s_m36c = 0;
                              if (s_m36c++ < 4) {
                                  fprintf(stderr, "[pm-jobmod] guest@0x%08X +0x50:",
                                          base);
                                  for (unsigned _o = 0x50; _o < 0x250; _o += 4)
                                      fprintf(stderr, " %02X%02X%02X%02X",
                                          g[_o],g[_o+1],g[_o+2],g[_o+3]);
                                  fprintf(stderr, "\n"); fflush(stderr);
                              } }
                          } }
                }
                ctx->stage_hashed = 0;   /* complete: don't re-match follow-ups */
            } else if (ctx->stage_hashed >= 0x800u && ctx->stage_hdr_span < STAGE_SPAN_PENDING) {
                /* Round-9: the streamed module matched NO registered hash.
                 * Old code was silent here -- a new attract-cycle jobmod
                 * (PVS video-post class) then never got a resident_code
                 * slot, its jobs fell to the interpreter span, nothing
                 * completed, and the PM entered the retire/re-publish
                 * livelock. Name the hash so it can be registered. */
                { static int _u = 0; int _q = 0;
                  static uint32_t s_unk_src[8]; static unsigned s_nunk;
                  for (unsigned k = 0; k < s_stage_good_n && k < 8; k++)
                      if (s_stage_good_src[k] == ctx->stage_ea0) _q = 1;
                  /* bg_264/265: the first attract cycle streams 0x4000..
                   * prefixes of a module that registers moments later. Log
                   * at most ONE unrecognized line per distinct source EA so
                   * the 8-line cap stays available for genuinely-new
                   * modules. */
                  for (unsigned k = 0; k < s_nunk && k < 8; k++)
                      if (s_unk_src[k] == ctx->stage_ea0) _q = 1;
                  if (!_q) {
                      s_unk_src[(s_nunk++) & 7u] = ctx->stage_ea0;
                      if (_u++ < 8)
                    fprintf(stderr, "[pm-jobmod] UNRECOGNIZED module at LS 0x%05X"
                                    " size=0x%X src=0x%08X hash=%016llX -- no resident"
                                    " slot, jobs will run interpreted\n",
                            ctx->stage_lo, ctx->stage_hashed, ctx->stage_ea0,
                            (unsigned long long)ctx->stage_fnv); }
                    }
            }
        }
    }
}

/* Match a 16-byte module header against the registered content signatures.
 * Returns the overlay image id, or 0 if none. Used by the WWS job-code dispatch
 * path: the job manager streams a module into the code buffer via a DMA LIST
 * (per-element GETs the single-transfer note_get hook doesn't see as one >=512
 * chunk), so resident_ovl is set at branch-in time by matching the buffer head. */
int spu_overlay_match_sig(const uint8_t hdr[16])
{
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        if (o->has_sig && memcmp(hdr, o->sig, 16) == 0)
            return o->image_id;
    }
    return 0;
}

/* HLE of the taskset Policy Module's task-syscall entry (LS 0xA70). A SPURS task
 * (e.g. the cri_mpv task, image 22) reads syscallAddr from its SpursTasksetContext
 * (LS 0x27C4) and branches to it to perform a task syscall (EXIT/YIELD/WAIT/POLL).
 * The real kernel has the PM code resident at 0xA70; we don't, so we plant 0xA70 as
 * syscallAddr (in the cri dispatch) and INTERCEPT a branch to it here to HLE the
 * syscall. num = r3&0xF (0x10 bit = the "2" variant), args in r4. Adopted from the
 * JonathanDC64/ps3recomp fork (aaea4158) which uses this to run SPURS tasks clean. */
/* Write the event bits a PPU-side Set owed this wait object into guest memory,
 * at object+0x00 (events) and every object+0x30 slot (pendingRecv).
 *
 * Called from the WAIT_SIGNAL handler immediately before control returns to the
 * guest. Doing it at Set time loses a race: the guest reads object+0x30 the
 * instant it resumes, and a task that took the latched-wake path was never
 * parked for the Set to target. Here the guest demonstrably has not read yet.
 *
 * ponytail: fills all 16 slots rather than the one the guest picked -- the slot
 * index is derived from state we do not model, and a wrong slot delivers
 * nothing. Narrow it if a title ever cares which slot fired. */
static void spu_ef_deliver_owed(spu_context* ctx, uint32_t wobj)
{
    (void)ctx;
    static int s_od = -1;
    if (s_od < 0) s_od = getenv("SPURS_EF_OBJ_DELIVER") ? 1 : 0;
    if (!s_od || !wobj || !vm_base) return;
    extern uint16_t spu_ef_bits_take(uint32_t);
    uint16_t bits = spu_ef_bits_take(wobj);
    if (!bits) return;
    uint8_t* m = vm_base + wobj;
    m[0] = (uint8_t)(bits >> 8); m[1] = (uint8_t)bits;          /* +0x00 events */
    for (uint32_t s = 0; s < 16; s++) {                          /* +0x30 slots */
        m[0x30 + 2*s]     = (uint8_t)(bits >> 8);
        m[0x30 + 2*s + 1] = (uint8_t)bits;
    }
    { static int _n = 0; if (_n++ < 8)
        fprintf(stderr, "[spu] delivered owed bits=0x%04X to object 0x%08X \n",
                (unsigned)bits, wobj); }
}

#include "spu_task_lifecycle_trace.h"

void spu_spurs_taskset_syscall(spu_context* ctx)   /* non-static: also called by the pure interpreter (spu_interp.c) */
{
    uint32_t raw = ctx->gpr[3]._u32[0];
    uint32_t num = raw & 0x0F;
    if (num == 0 || num == 1)
        spu_task_lifecycle_trace("syscall", ctx->image_id, raw,
            spu_task_trace_be32(ctx->ls + 0x27BC),
            spu_task_trace_be32(ctx->ls + 0x27D4), ctx->ls, NULL);
    /* SPU_SYSCALL_IMG=<n>: every syscall of image n, uncapped (the 24-line
     * budget is spent by FMOD's WAIT_SIGNAL loop long before a later task). */
    static int s_si = -2;
    if (s_si == -2) { const char* e = getenv("SPU_SYSCALL_IMG"); s_si = e ? atoi(e) : -1; }
    { static int _n = 0; if (_n++ < 24 || ctx->image_id == s_si)
        fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d link/r0=0x%05X wobj@2FDC=0x%02X%02X%02X%02X\n",
                num, raw, ctx->gpr[4]._u32[0], ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                ctx->ls[0x2FDC], ctx->ls[0x2FDD], ctx->ls[0x2FDE], ctx->ls[0x2FDF]); }
    /* The task-API argument is passed in LOCAL STORE at 0x2FD0, not in r4: the
     * caller does shufb(arg,...) -> stqd 0x2FD0 and only then sets r3 = number
     * (see func_00026DE0 / func_000272AC in image 22). Logging r4 shows caller
     * leftovers and hides what the task actually asked for. */
    { static int s_sa = -1; if (s_sa < 0) s_sa = getenv("YDKJ_SYSCALL_ARG") ? 1 : 0;
      if (s_sa) { static int _n = 0; if (_n++ < 40) {
        const uint8_t* a = &ctx->ls[0x2FD0];
        fprintf(stderr, "[spu] syscall num=%u LS[0x2FD0]=%02X%02X%02X%02X %02X%02X%02X%02X"
                        " %02X%02X%02X%02X %02X%02X%02X%02X img=%d\n",
                num, a[0],a[1],a[2],a[3], a[4],a[5],a[6],a[7],
                a[8],a[9],a[10],a[11], a[12],a[13],a[14],a[15], ctx->image_id);
        fflush(stderr); } } }
    if (num == 0) {
        ctx->task_exit_requested = 1;
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);
        return;
    }
    /* WAIT_SIGNAL(2): REAL semantics (RPCS3 spursTasksetProcessSyscall) --
     * consume the task's bit in the guest taskset's `signalled` bitset, or
     * SLEEP this task's host thread until _cellSpursSendSignal /
     * cellSpursEventFlagSet delivers one (the FMOD mixer's flag-A wait path:
     * the task registers its wait slot in the flag struct with atomics, then
     * syscalls WAIT_SIGNAL; the PPU-side Set satisfies the slot and signals).
     * Returning "success" here without waiting made the task spin on empty
     * work state forever -- the LBP boot deadlock. taskset/taskId come from
     * the SpursTasksetContext this runtime planted at LS 0x2700. */
    if (num == 2) {
        uint32_t ts  = ((uint32_t)ctx->ls[0x27BC] << 24) | ((uint32_t)ctx->ls[0x27BD] << 16) |
                       ((uint32_t)ctx->ls[0x27BE] << 8)  |  (uint32_t)ctx->ls[0x27BF];
        uint32_t tid = ((uint32_t)ctx->ls[0x27D4] << 24) | ((uint32_t)ctx->ls[0x27D5] << 16) |
                       ((uint32_t)ctx->ls[0x27D6] << 8)  |  (uint32_t)ctx->ls[0x27D7];
        /* The guest names its wait OBJECT in the task-API argument at LS 0x2FD0
         * (low nibble is flags). It reads its received event bits from
         * object+0x30, so this is both who to wake and where to deliver. */
        uint32_t wobj = (((uint32_t)ctx->ls[0x2FDC] << 24) |
                         ((uint32_t)ctx->ls[0x2FDD] << 16) |
                         ((uint32_t)ctx->ls[0x2FDE] << 8)  |
                          (uint32_t)ctx->ls[0x2FDF]) & ~0xFu;
        /* SPURS_EF_SPU_REPLY=1 -- DIAGNOSTIC BISECT, NOT A FIX.
         *
         * The guest task is supposed to set its SPU->PPU flag (object+0x100)
         * when a work cycle completes; it never does, so the PPU blocks on that
         * flag forever and can never enqueue the next unit of work. That is a
         * cycle: we cannot see what the PPU would do next without breaking it.
         *
         * On RE-ENTRY to WAIT_SIGNAL for the same object the previous cycle has
         * finished, so set the flag on the task`s behalf and watch where the PPU
         * goes. This FABRICATES guest state -- the repo`s own history says faked
         * success is the most expensive kind of bug (see the unresolved-NID note
         * in ppu_hle.cpp), so it stays opt-in and must never become the default.
         * If the PPU advances, the answer is what the task must do to earn it. */
        { static int s_rp = -1;
          if (s_rp < 0) { const char* e = getenv("SPURS_EF_SPU_REPLY");
                          s_rp = e ? atoi(e) : 0; }   /* 2 = reply on EVERY entry */
          if (s_rp && wobj) {
              static uint32_t seen[8]; static int seen_n = 0;
              int again = 0;
              for (int i = 0; i < seen_n; i++) if (seen[i] == wobj) { again = 1; break; }
              if (s_rp >= 2) again = 1;   /* force: answer "would the PPU move at all?" */
              if (!again) { if (seen_n < 8) seen[seen_n++] = wobj; }
              else {
                  extern void spurs_ef_set_from_spu(uint32_t, uint16_t);
                  spurs_ef_set_from_spu(wobj + 0x100u, 1);
                  static int _n = 0;
                  if (_n++ < 8)
                      fprintf(stderr, "[spu] SPU-REPLY: set flag 0x%08X for object "
                                      "0x%08X (cycle complete)\n", wobj + 0x100u, wobj);
              }
          } }

        if (ts) {
            { /* Round 26: WAIT_SIGNAL-class park while pc sits in a staged
             * jobmod span -- the walk-in module's yield/park/exit answer
             * (discriminator III: module parks without exiting). */
              uint32_t _wp = (uint32_t)ctx->pc & SPU_LS_MASK;
              for (int _i = 0; _i < 4; _i++)
                  if (ctx->resident_code[_i].size
                      && _wp >= ctx->resident_code[_i].lsa
                      && _wp < ctx->resident_code[_i].lsa
                               + ctx->resident_code[_i].size) {
                      extern int spu_walkin_lift(uint32_t, int, int);
                      static int _wn = 0;
                      if (_wn++ < 32 || (_wn % 128) == 0)
                          fprintf(stderr, "[jobstop] waitsig img=%d lifted=%d "
                                  "pc=0x%05X r3=0x%08X task=%u\n",
                                  ctx->resident_code[_i].image_id,
                                  spu_walkin_lift(_wp,
                                      ctx->resident_code[_i].image_id,
                                      (int)ctx->resident_code[_i].rel),
                                  _wp, ctx->gpr[3]._u32[0], tid),
                          fflush(stderr);
                      break;
                  } }
            extern int spu_taskset_wait_signal(uint32_t, uint32_t);
            /* Park = OS-level wait on this SPU's host thread. Under the lockstep
             * gate the thread HOLDS the global run token here; parking without
             * releasing it starves every other lifted SPU (observed: FMOD task 1
             * parks in WAIT_SIGNAL holding the token -> the mixer never runs
             * again -> the PPU audio pump blocks forever on flag 0x94F600). */
            /* Publish that this task is parked so a PPU-side event-flag Set can
             * reach it even though it registered no wait slot in the flag. */
            extern void spu_taskset_parked_add(uint32_t, uint32_t, uint32_t);
            extern void spu_taskset_parked_del(uint32_t, uint32_t);
            extern int spu_taskset_consume_wake(uint32_t);
            if (spu_taskset_consume_wake(ts)) {   /* a Set beat us to the park */
                spu_ef_deliver_owed(ctx, wobj);
                ctx->gpr[3]._u32[0] = 0;
                { static int _wq = 0;
                  if (ctx->image_id == 2 && _wq++ < 32)
                    fprintf(stderr, "[taskset-wait] ret=%u task=%u taskset=0x%08X "
                            "path=consumed-fast (signal latched before park)\n",
                            ctx->gpr[3]._u32[0], tid, ts); }
                return;
            }
            yz_lockstep_block_begin(ctx);
            /* Round-7: this syscall IS the re-park. If the task's resume
             * window is open, dump what it read in the quantum that just
             * ended (woke on a signal, ran, found its progress gate unset,
             * came back). The pc/lr pair names where the guest gave up. */
            { extern int spu_taskset_ring_repark(uint32_t, uint32_t);
              if (spu_taskset_ring_repark(ts, tid))
                  fprintf(stderr, "[taskset] re-park pc=0x%05X lr=0x%05X "
                          "ev=0x%X mask=0x%X (WAIT_SIGNAL re-entry)\n",
                          (uint32_t)ctx->pc & SPU_LS_MASK,
                          ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                          ctx->event_status, ctx->event_mask); }
            { static int s_pid = 0;   /* round 22(b): WHO parks -- image,
                 * pc, span hint. Task 0 of taskset 0x1027DC00 identity. */
              if (s_pid++ < 8 || (s_pid % 128) == 0)
                  fprintf(stderr, "[taskset] park img=%d pc=0x%05X ts=0x%08X "
                          "task=%u\n", ctx->image_id,
                          (uint32_t)ctx->pc & SPU_LS_MASK, ts, tid),
                  fflush(stderr); }
            spu_taskset_parked_add(ts, tid, wobj);
            spu_taskset_wait_signal(ts, tid);
            spu_taskset_parked_del(ts, tid);
            yz_lockstep_block_end(ctx);
            spu_ef_deliver_owed(ctx, wobj);
            { static int _wp = 0;
              if (ctx->image_id == 2 && _wp++ < 32)
                fprintf(stderr, "[taskset-wait] ret=%u task=%u taskset=0x%08X "
                        "path=parked+woke\n", 0u, tid, ts); }
        }
        ctx->gpr[3]._u32[0] = 0;
        { static int _wn = 0;
          if (ctx->image_id == 2 && !ts && _wn++ < 32)
            fprintf(stderr, "[taskset-wait] ret=0 task=%u taskset=NONE "
                    "path=no-taskset (immediate return)\n", tid); }
        return;
    }
    /* YIELD(1)/POLL(3)/RECV_WKL_FLAG(4):
     * report success and resume (return -> lifted caller continues at its link). */
    ctx->gpr[3]._u32[0] = 0;
}

/* ---------------------------------------------------------------------------
 * Micro-interpreter for RUNTIME-GENERATED stub code (SMC).
 *
 * The WWS jobmanager's interrupt handler GENERATES a register save/restore
 * stub above the static image (LS 0x3FEC0: a chain of stqd/lqd plus a `bi`
 * terminator, written at interrupt entry) and calls it. No lift can exist for
 * bytes that only come into being at runtime, so when the indirect dispatch
 * finds no registered function we interpret the LIVE LS bytes directly for
 * the small instruction set such stubs use, and exit back into lifted code at
 * the first branch (trampoline re-dispatch). Returns 1 if it executed to a
 * branch, 0 on an unknown opcode (caller falls through to BRANCH-TO-0). */
static int spu_smc_microstep(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    for (int steps = 0; steps < 4096; steps++) {
        uint32_t w = ((uint32_t)ctx->ls[pc] << 24) | ((uint32_t)ctx->ls[pc+1] << 16) |
                     ((uint32_t)ctx->ls[pc+2] << 8) | ctx->ls[pc+3];
        uint32_t op11 = w >> 21, op9 = w >> 23, op8 = w >> 24, op7 = w >> 25;
        uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
        int32_t  i10 = (int32_t)(w << 8) >> 22;               /* bits 14-23 sext */
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            /* bi / bisl / iret / bisled (+E/D interrupt bits) */
            if (op11 == 0x1AB &&                       /* bisled: only on event */
                (ctx->event_status & ctx->event_mask) == 0) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            uint32_t tgt = (op11 == 0x1AA) ? ctx->srr0
                                           : ctx->gpr[ra]._u32[0];
            if (op11 == 0x1A9 || op11 == 0x1AB)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op11 >= 0x128 && op11 <= 0x12B) {
            /* biz / binz / bihz / bihnz rt,ra (+E/D bits, taken-only) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;                     /* biz  */
            case 0x129: taken = (cv != 0); break;                     /* binz */
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;          /* bihz */
            default:    taken = ((cv & 0xFFFF) != 0); break;          /* bihnz*/
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            /* br / bra / brsl / brasl */
            uint32_t tgt = (op9 == 0x060 || op9 == 0x062)
                         ? ((uint32_t)i16 << 2)
                         : (pc + ((uint32_t)i16 << 2));
            if (op9 == 0x066 || op9 == 0x062)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x040 || op9 == 0x042 || op9 == 0x044 || op9 == 0x046) {
            /* brz / brnz / brhz / brhnz rt,label (relative) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op9) {
            case 0x040: taken = (cv == 0); break;
            case 0x042: taken = (cv != 0); break;
            case 0x044: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            ctx->pc = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op8 == 0x24) {                                    /* stqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {                                    /* lqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {                   /* stqa / lqa (abs) */
            /* Absolute address field is in WORD units: <<2. A <<4 here made
             * every microstepped stqa/lqa hit 4x the intended LS cell -- the
 * PM module-call tail 0x30A0 `lqa r5,0x14E0` read 0x5380 instead,
 * jumped to virgin LS 0x39004, and the unlifted guard halt-killed the
 * lane -> WWS boot stall (PmHaltRace numeric proof vs pm_ls.txt:
 * word 0x30829C05, field 0x538, 0x538<<2=0x14E0). */
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {                   /* stqr / lqr (rel) */
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {                                    /* xori rt,ra,i10 */
            /* Runtime-generated job stubs toggle their own instruction words.
             * Use the signed RI10 immediate, just like statically lifted xori. */
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {                                    /* ai rt,ra,i10 */
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {                 /* sf / a (word) */
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }  /* il  */
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; } /* ilhu */
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];            /* iohl */
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; } /* ila */
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }  /* nop/lnop */
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }  /* sync/dsync */
        /* Branch hints, all no-ops for execution: hbr is the 11-bit-opcode RR
         * form (0x1AC), hbra and hbrr are the 7-bit-opcode RI18 form (0x08,
         * 0x09) -- the same op7 group as ila (0x21) above, NOT op9. Testing
         * op9 here never matched either (hbrr 0x12033296 has op9 0x24), so a
         * runtime-generated stub carrying a branch hint decoded to UNKNOWN,
         * the microstep bailed, and the SPU fell into branch-to-0. */
        /* sagemono reached the same fix independently in #166, from a different
         * witness (hbrr 0x1200048C rather than 0x12033296). Same line, same
         * diagnosis, arrived at separately -- which is about as good as
         * corroboration gets for a decode bug. */
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; } /* hbr/hbra/hbrr */

        /* Hand off to the real interpreter rather than giving up.
         *
         * This microstepper knows a dozen opcodes -- enough for the
         * runtime-generated stubs it was written for -- and spu_interp.c is a
         * full, selftested SPU interpreter that was already sitting here
         * unused on this path. Bailing on the first ordinary instruction is
         * strictly worse than running it.
         *
         * It matters for overlay code: a SPURS job policy module DMAs its job
         * bodies into local store and branches to them, so there is no lifted
         * body to find and the fallback IS the execution path. Guitar Hero
         * III`s job kernel dies here on an `ilh` at LS 0x6638 -- a perfectly
         * ordinary instruction the interpreter handles -- and with it the
         * decompression job it was running never completes. */
        { static int _n = 0;
          if (_n++ < 8)
              fprintf(stderr, "[spu-smc] microstep img=%d pc=0x%05X word 0x%08X not in the"
                      " microstep set -- handing to the interpreter (steps=%d)\n",
                      ctx->image_id, pc, w, steps); }
        ctx->pc = pc;
        /* Stop at the drain's return point too: it sits mid-function, so it is
         * no lifted entry, and a job's return would otherwise run on through
         * the caller's code (GH3's song-freeze; see drain_ret_pc). */
        uint32_t end_pc = spu_interp_run_until(ctx, pc, ctx->drain_ret_pc);
        return end_pc || (ctx->drain_ret_pc &&
               (end_pc & SPU_LS_MASK) == (ctx->drain_ret_pc & SPU_LS_MASK)) ? 1 : 0;
    }
    { static int _n = 0; if (_n++ < 4)
        fprintf(stderr, "[spu-smc] microstep img=%d runaway (4096 steps from 0x%05X)\n",
                ctx->image_id, ctx->pc & SPU_LS_MASK); }
    return 0;
}

/* A staged callable can yield before reaching its link. Keep its live-byte
 * span and original link until the actual return; dispatching a yielded PC
 * through the base image's registry can select code overwritten by the DMA.
 * The normal dispatcher still handles below-span services/interrupt handlers. */
static void spu_staged_interp_resume(spu_context* ctx)
{
    spu_interp_run_until(ctx, ctx->pc & SPU_LS_MASK, ctx->interp_return_pc);
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      spu_irq_regs_maybe_restore(ctx); }
    if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) {
        ctx->interp_span_lo = ctx->interp_span_hi = ctx->interp_return_pc = 0;
        spu_halt(ctx);
        return;
    }
    if (ctx->interp_return_pc &&
        (ctx->pc & SPU_LS_MASK) == ctx->interp_return_pc) {
        ctx->interp_span_lo = ctx->interp_span_hi = ctx->interp_return_pc = 0;
        return;   /* the caller's drain may now pop its host call bracket */
    }
    g_spu_trampoline_fn = spu_indirect_branch;
}

/* ---- Native policy-module services ----
 * Under SPU_NATIVE_OWNED_JOBS=1 a resident job runs compiled while the policy
 * module stays interpreted, so every service the job calls into the policy
 * text (a bisl through a register) runs in the interpreter. A title may
 * register those services (spu_pm_services_register): the policy image, where
 * its text sits in LS, the guest EA and length of the pristine text with the
 * FNV-1a of the whole registered text, the entry points, and the LS code
 * ranges the services reach. The text range may also hold live data, so only
 * the code ranges are compared and admitted.
 *
 * SPU_PM_NATIVE_SVC=1 (default 0): when compiled job code calls a registered
 * entry -- the link register equals the call bracket's return pc and lies in a
 * resident job span -- and every code range in LS still equals the pristine
 * text (copied from guest memory once and checked against the fingerprint),
 * the compiled entry runs. Until that bracket ends the owned gate also admits
 * the code ranges; every other owned-gate condition still applies, and any
 * other target is interpreted as before.
 *
 * SPU_PM_SVC_VERIFY=1 (diagnostic; needs SPU_PM_NATIVE_SVC=1): the call runs
 * interpreted as without the switch while every channel operation and its LS
 * effect is recorded. The compiled service is then replayed on a private
 * context and LS that are fed the recorded results; the operation stream, all
 * registers, pc, interrupt enable and the whole LS are compared and a mismatch
 * is logged. The real context keeps the interpreted result. */
static struct {
    int registered, image_id, nentries, ncode;
    uint32_t ls_base, text_len, tmpl_ea, tmpl_len;
    uint64_t text_fnv;
    uint32_t entries[SPU_PM_SERVICES_MAX_ENTRIES];
    uint32_t code[SPU_PM_SERVICES_MAX_RANGES][2];
    uint8_t* text;
} s_svc;
int g_spu_pm_svc_on;
static int s_svc_verify;
// 0 = uninitialized, 1 = copying, 2 = verified, -1 = rejected.
static volatile LONG s_svc_once;
static volatile LONG s_svc_native, s_svc_vok, s_svc_vbad, s_svc_vincomplete,
                     s_svc_voverflow, s_svc_text_differs, s_svc_not_job_call;
/* The bracket a native service was entered from: the caller's context, the
 * job's return pc and the bracket depth. */
typedef struct { const spu_context* ctx; uint32_t ret; int depth; } svc_scope;
static SPU_THREAD_LOCAL svc_scope t_svc;

static int pm_svc_ready(void);
static int pm_svc_switch(const char* name)
{
    const char* value = getenv(name);
    if (!value || !strcmp(value, "0")) return 0;
    if (!strcmp(value, "1")) return 1;
    fprintf(stderr, "invalid %s: expected 0 or 1\n", name);
    abort();
}
void spu_pm_services_register(const spu_pm_services* d)
{
    int on = pm_svc_switch("SPU_PM_NATIVE_SVC");
    int verify = pm_svc_switch("SPU_PM_SVC_VERIFY");
    memset(&s_svc, 0, sizeof s_svc);
    if (!d || d->nentries <= 0 || d->nentries > SPU_PM_SERVICES_MAX_ENTRIES ||
        d->ncode <= 0 || d->ncode > SPU_PM_SERVICES_MAX_RANGES || !d->text_len ||
        d->ls_base >= SPU_LS_SIZE || d->text_len > SPU_LS_SIZE - d->ls_base ||
        !d->tmpl_len || d->tmpl_len > d->text_len) {
        fprintf(stderr, "[pm-svc] invalid service registration: native services off\n");
        g_spu_pm_svc_on = 0;
        return;
    }
    for (int i = 0; i < d->ncode; ++i) {
        uint32_t lo = d->code[i][0], hi = d->code[i][1];
        if (lo < d->ls_base || hi <= lo || hi > d->ls_base + d->text_len || (lo & 3) || (hi & 3)) {
            fprintf(stderr, "[pm-svc] code range %d outside the registered text: native services off\n", i);
            g_spu_pm_svc_on = 0;
            return;
        }
        s_svc.code[i][0] = lo; s_svc.code[i][1] = hi;
    }
    for (int i = 0; i < d->nentries; ++i) s_svc.entries[i] = d->entries[i] & SPU_LS_MASK;
    s_svc.image_id = d->image_id; s_svc.nentries = d->nentries; s_svc.ncode = d->ncode;
    s_svc.ls_base = d->ls_base; s_svc.text_len = d->text_len;
    s_svc.tmpl_ea = d->tmpl_ea; s_svc.tmpl_len = d->tmpl_len; s_svc.text_fnv = d->text_fnv;
    s_svc.registered = 1;
    s_svc_verify = on && verify;
    g_spu_pm_svc_on = on;
    s_svc_once = 0;
    if (on && vm_base) pm_svc_ready();   /* guest memory already loaded: copy now */
}
void spu_pm_services_counters(uint64_t out[SPU_PM_SERVICES_COUNTERS])
{
    out[0] = (uint64_t)s_svc_native; out[1] = (uint64_t)s_svc_vok; out[2] = (uint64_t)s_svc_vbad;
    out[3] = (uint64_t)s_svc_vincomplete; out[4] = (uint64_t)s_svc_voverflow;
    out[5] = (uint64_t)s_svc_text_differs; out[6] = (uint64_t)s_svc_not_job_call;
}
/* Registration with guest memory loaded, else the first call: copy the pristine
 * text out of guest memory (zero beyond the template) and check it against the
 * registered fingerprint. 1 = usable. */
static int pm_svc_ready(void)
{
    LONG state = InterlockedCompareExchange(&s_svc_once, 1, 0);
    if (state != 0) {
        while (state == 1) {
            Sleep(0);
            state = InterlockedCompareExchange(&s_svc_once, 0, 0);
        }
        return state == 2;
    }
    int ok = 0;
    uint64_t h = 1469598103934665603ULL;
    uint8_t* t = NULL;
    extern uint32_t ppu_vm_size;   /* 0: the whole 32-bit space is backed */
    uint64_t end = (uint64_t)s_svc.tmpl_ea + s_svc.tmpl_len;
    if (s_svc.registered && vm_base && end <= (ppu_vm_size ? (uint64_t)ppu_vm_size : 0x100000000ull) &&
        (t = (uint8_t*)calloc(1, s_svc.text_len)) != NULL) {
        memcpy(t, vm_base + s_svc.tmpl_ea, s_svc.tmpl_len);
        for (uint32_t i = 0; i < s_svc.text_len; ++i) h = (h ^ t[i]) * 1099511628211ULL;
        ok = h == s_svc.text_fnv;
        if (ok) s_svc.text = t; else free(t);
    }
    fprintf(stderr, "[pm-svc] image %d text ea=0x%08X len=0x%X fnv=%016llX expect=%016llX: native services %s%s\n",
            s_svc.image_id, s_svc.tmpl_ea, s_svc.text_len, (unsigned long long)h,
            (unsigned long long)s_svc.text_fnv, ok ? "on" : "off", ok && s_svc_verify ? " (verify)" : "");
    InterlockedExchange(&s_svc_once, ok ? 2 : -1);
    return ok;
}
/* A scope lasts while its bracket does: deeper brackets, or the same bracket
 * still returning to the job. Anything else means the bracket ended. */
static int pm_svc_active(const spu_context* ctx)
{
    if (t_svc.ctx != ctx) return 0;
    if (ctx->host_depth > t_svc.depth ||
        (ctx->host_depth == t_svc.depth && (ctx->drain_ret_pc & SPU_LS_MASK) == t_svc.ret)) return 1;
    t_svc.ctx = NULL;
    return 0;
}
int spu_pm_svc_can_run(const spu_context* ctx, uint32_t pc)
{
    if (!pm_svc_active(ctx)) return 0;
    for (int i = 0; i < s_svc.ncode; ++i)
        if (pc >= s_svc.code[i][0] && pc < s_svc.code[i][1]) return 1;
    return 0;
}
void spu_pm_svc_drain_done(spu_context* ctx, uint32_t return_pc)
{
    if (t_svc.ctx == ctx && t_svc.ret == (return_pc & SPU_LS_MASK) && ctx->host_depth == t_svc.depth)
        t_svc.ctx = NULL;
}
/* Is this indirect branch a compiled job's call into a registered entry, with
 * the code ranges in LS equal to the pristine text? */
static int pm_svc_call_ok(spu_context* ctx, uint32_t pc)
{
    if (ctx->image_id != s_svc.image_id || ctx->host_depth <= 0) return 0;
    int entry = 0;
    for (int i = 0; i < s_svc.nentries; ++i) entry |= s_svc.entries[i] == pc;
    if (!entry || !pm_svc_ready()) return 0;
    uint32_t ret = ctx->drain_ret_pc & SPU_LS_MASK;
    int job_call = 0;
    if (ret >= 4 && (ctx->gpr[0]._u32[0] & SPU_LS_MASK) == ret) {
        uint32_t call = ret - 4;
        for (unsigned i = 0; i < sizeof(ctx->resident_code) / sizeof(ctx->resident_code[0]); ++i) {
            uint32_t base = ctx->resident_code[i].lsa, size = ctx->resident_code[i].size;
            int image = ctx->resident_code[i].image_id;
            if (!image || call < base || call - base >= size) continue;
            if (image > 0 && size <= SPU_LS_SIZE - base && ret - base < size) {
                const uint8_t* bytes = ctx->ls + call;
                uint32_t instruction = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
                                       (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
                job_call = (instruction & 0xffe0007fu) == 0x35200000u;
            }
            break;
        }
    }
    if (!job_call) { InterlockedIncrement(&s_svc_not_job_call); return 0; }
    for (int i = 0; i < s_svc.ncode; ++i) {
        uint32_t lo = s_svc.code[i][0], n = s_svc.code[i][1] - lo;
        if (memcmp(ctx->ls + lo, s_svc.text + (lo - s_svc.ls_base), n) != 0) {
            if (InterlockedIncrement(&s_svc_text_differs) <= 8) {
                uint32_t o = lo; while (ctx->ls[o] == s_svc.text[o - s_svc.ls_base]) ++o;
                fprintf(stderr, "[pm-svc] LS 0x%05X differs from the registered text (entry 0x%05X): interpreted\n", o, pc);
            }
            return 0;
        }
    }
    return 1;
}

/* Verifier: record the interpreted call, replay the compiled one. */
#define SV_MAXOP 512
#define SV_MAXRUN 4096
#define SV_MAXBYTES (256u * 1024u)
enum { SV_WR, SV_RD, SV_CNT };
typedef struct { uint8_t kind, ch; u128 val; uint32_t run0, nrun; } sv_op;
typedef struct { uint32_t lsa, n, off; } sv_run;
typedef struct {
    spu_context* ctx; int overflow; unsigned nop, nrun; uint32_t nbytes; uint8_t* prev;
    sv_op op[SV_MAXOP]; sv_run run[SV_MAXRUN]; uint8_t bytes[SV_MAXBYTES];
} sv_log;
static SPU_THREAD_LOCAL sv_log* t_rec;          /* recording the interpreted call */
static SPU_THREAD_LOCAL sv_log* t_rp;           /* replaying into the private context */
static SPU_THREAD_LOCAL spu_context* t_rp_ctx;
static SPU_THREAD_LOCAL unsigned t_rp_i, t_rp_bad;
/* Writes that only latch MFC parameters cannot change LS. */
static int sv_ls_effect(int kind, uint32_t ch)
{
    return !(kind == SV_WR && (ch == MFC_LSA || ch == MFC_EAH || ch == MFC_EAL || ch == MFC_Size ||
                               ch == MFC_TagID || ch == MFC_WrTagMask));
}
static sv_op* sv_rec_begin(sv_log* L, spu_context* ctx, int kind, uint32_t ch)
{
    if (L->nop >= SV_MAXOP) { L->overflow = 1; return NULL; }
    sv_op* o = &L->op[L->nop++];
    memset(o, 0, sizeof *o); o->kind = (uint8_t)kind; o->ch = (uint8_t)ch; o->run0 = L->nrun;
    if (sv_ls_effect(kind, ch)) memcpy(L->prev, ctx->ls, SPU_LS_SIZE);
    return o;
}
/* Keep the LS bytes this operation changed (DMA completion, atomics). */
static void sv_rec_end(sv_log* L, spu_context* ctx, sv_op* o)
{
    if (!o || !sv_ls_effect(o->kind, o->ch)) return;
    const uint8_t* a = L->prev; const uint8_t* b = ctx->ls;
    for (uint32_t blk = 0; blk < SPU_LS_SIZE && !L->overflow; blk += 4096) {
        if (!memcmp(a + blk, b + blk, 4096)) continue;
        for (uint32_t i = blk, end = blk + 4096; i < end;) {
            if (a[i] == b[i]) { ++i; continue; }
            uint32_t s = i;
            while (i < end && a[i] != b[i]) ++i;
            if (L->nrun >= SV_MAXRUN || L->nbytes + (i - s) > SV_MAXBYTES) { L->overflow = 1; break; }
            L->run[L->nrun].lsa = s; L->run[L->nrun].n = i - s; L->run[L->nrun].off = L->nbytes; L->nrun++;
            memcpy(L->bytes + L->nbytes, b + s, i - s); L->nbytes += i - s;
        }
    }
    o->nrun = L->nrun - o->run0;
}
/* Next recorded operation for the private context, with its LS effect applied. */
static sv_op* sv_replay(spu_context* sh, int kind, uint32_t ch)
{
    sv_log* L = t_rp;
    if (t_rp_i >= L->nop) { t_rp_bad |= 1; return NULL; }
    sv_op* o = &L->op[t_rp_i++];
    if (o->kind != kind || o->ch != ch) { t_rp_bad |= 2; return NULL; }
    for (uint32_t r = o->run0; r < o->run0 + o->nrun; ++r)
        memcpy(sh->ls + L->run[r].lsa, L->bytes + L->run[r].off, L->run[r].n);
    return o;
}
static void pm_svc_verify_call(spu_context* ctx, uint32_t pc)
{
    static SPU_THREAD_LOCAL sv_log* L;
    static SPU_THREAD_LOCAL uint8_t *sls, *prev;
    static SPU_THREAD_LOCAL spu_context* sh;
    if (!L) {
        L = (sv_log*)malloc(sizeof *L); sls = (uint8_t*)malloc(SPU_LS_SIZE);
        prev = (uint8_t*)malloc(SPU_LS_SIZE); sh = (spu_context*)malloc(sizeof *sh);
    }
    if (!L || !sls || !prev || !sh) { spu_native_owned_interpret(ctx); return; }
    *sh = *ctx; memcpy(sls, ctx->ls, SPU_LS_SIZE);
    uint32_t stop = ctx->drain_ret_pc & SPU_LS_MASK;
    L->ctx = ctx; L->overflow = 0; L->nop = L->nrun = 0; L->nbytes = 0; L->prev = prev;
    t_rec = L;
    spu_native_owned_interpret(ctx);
    /* A service that yields to a kernel vector returns with a trampoline
     * pending; finish it in the caller's drain, still recording. */
    if ((ctx->pc & SPU_LS_MASK) != stop && g_spu_trampoline_fn &&
        !(ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)))
        spu_drain_call(ctx, stop);
    t_rec = NULL;
    if ((ctx->pc & SPU_LS_MASK) != stop || g_spu_trampoline_fn ||
        (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT))) {
        InterlockedIncrement(&s_svc_vincomplete);
        return;
    }
    if (L->overflow) { InterlockedIncrement(&s_svc_voverflow); return; }
    void (*saved_tf)(spu_context*) = g_spu_trampoline_fn;
    svc_scope saved_scope = t_svc;
    sh->ls = sls;
    t_rp = L; t_rp_ctx = sh; t_rp_i = 0; t_rp_bad = 0;
    t_svc.ctx = sh; t_svc.ret = stop; t_svc.depth = sh->host_depth;
    spu_lifted_fn fn = spu_lifted_lookup(sh, pc);
    if (fn) {
        sh->pc = pc; g_spu_trampoline_fn = 0;
        fn(sh);
        spu_drain_call(sh, stop);
    } else t_rp_bad |= 8;
    t_rp = NULL; t_rp_ctx = NULL; t_svc = saved_scope; g_spu_trampoline_fn = saved_tf;
    unsigned bad = t_rp_bad;
    if (t_rp_i != L->nop) bad |= 16;
    if ((sh->pc & SPU_LS_MASK) != (ctx->pc & SPU_LS_MASK)) bad |= 32;
    int reg = -1;
    for (int r = 0; r < 128 && reg < 0; ++r) if (memcmp(&sh->gpr[r], &ctx->gpr[r], sizeof(u128))) reg = r;
    if (reg >= 0) bad |= 64;
    if (sh->int_enable != ctx->int_enable) bad |= 128;
    uint32_t lsd = SPU_LS_SIZE;
    if (memcmp(sh->ls, ctx->ls, SPU_LS_SIZE)) { lsd = 0; while (sh->ls[lsd] == ctx->ls[lsd]) ++lsd; bad |= 256; }
    if (!bad) InterlockedIncrement(&s_svc_vok);
    else if (InterlockedIncrement(&s_svc_vbad) <= 16)
        fprintf(stderr, "[pm-svc] VERIFY MISMATCH entry=0x%05X flags=0x%X ops=%u/%u first_reg=%d ls=0x%05X pc=0x%05X/0x%05X\n",
                pc, bad, t_rp_i, L->nop, reg, lsd, sh->pc & SPU_LS_MASK, ctx->pc & SPU_LS_MASK);
    if (((s_svc_vok + s_svc_vbad) & 0xFFFF) == 0)
        fprintf(stderr, "[pm-svc] verify ok=%ld bad=%ld incomplete=%ld overflow=%ld\n", (long)s_svc_vok,
                (long)s_svc_vbad, (long)s_svc_vincomplete, (long)s_svc_voverflow);
}
/* spu_indirect_branch: 1 = the call was run here. */
static int pm_svc_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (!pm_svc_call_ok(ctx, pc)) return 0;
    spu_lifted_fn fn = spu_lifted_lookup(ctx, pc);
    if (!fn) return 0;
    svc_scope saved = t_svc;
    t_svc.ctx = ctx; t_svc.ret = ctx->drain_ret_pc & SPU_LS_MASK; t_svc.depth = ctx->host_depth;
    if (!spu_native_owned_can_run(ctx, pc)) { t_svc = saved; return 0; }   /* IRQ, watches, observers */
    if (s_svc_verify) { t_svc = saved; pm_svc_verify_call(ctx, pc); return 1; }
    InterlockedIncrement(&s_svc_native);
    ctx->pc = pc;
    fn(ctx);
    return 1;
}
/* Channel entry points: the verifier's replay and recording, then the channel. */
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    if (s_svc_verify) {
        if (t_rp_ctx == ctx) {
            sv_op* o = sv_replay(ctx, SV_WR, channel);
            if (o && memcmp(&o->val, &value, sizeof value)) t_rp_bad |= 4;
            return;
        }
        if (t_rec && t_rec->ctx == ctx) {
            sv_op* o = sv_rec_begin(t_rec, ctx, SV_WR, channel);
            if (o) o->val = value;
            spu_wrch_channel(ctx, channel, value);
            sv_rec_end(t_rec, ctx, o);
            return;
        }
    }
    spu_wrch_channel(ctx, channel, value);
}
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    if (s_svc_verify) {
        if (t_rp_ctx == ctx) {
            u128 r; sv_op* o = sv_replay(ctx, SV_RD, channel);
            if (o) r = o->val; else memset(&r, 0, sizeof r);
            return r;
        }
        if (t_rec && t_rec->ctx == ctx) {
            sv_op* o = sv_rec_begin(t_rec, ctx, SV_RD, channel);
            u128 r = spu_rdch_channel(ctx, channel);
            if (o) o->val = r;
            sv_rec_end(t_rec, ctx, o);
            return r;
        }
    }
    return spu_rdch_channel(ctx, channel);
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    if (s_svc_verify) {
        if (t_rp_ctx == ctx) {
            sv_op* o = sv_replay(ctx, SV_CNT, channel);
            return o ? o->val._u32[0] : 0;
        }
        if (t_rec && t_rec->ctx == ctx) {
            sv_op* o = sv_rec_begin(t_rec, ctx, SV_CNT, channel);
            uint32_t r = spu_rchcnt_channel(ctx, channel);
            if (o) o->val._u32[0] = r;
            sv_rec_end(t_rec, ctx, o);
            return r;
        }
    }
    return spu_rchcnt_channel(ctx, channel);
}
/* ---- end native policy-module services ---- */

void spu_indirect_branch(spu_context* ctx)
{
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    /* Interrupt-return register restore (see spu_drain.c spu_irq_regs_save):
     * the WWS handler's save/restore shim lives at unlifted top-of-LS, so we
     * enforce the preserve-all-registers hardware contract here -- the first
     * interrupts-enabled dispatch at the saved srr0 is the irete. */
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      if (spu_irq_regs_maybe_restore(ctx)) {
          static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
          static int _r = 0;
          if (s_it && _r++ < 200)
              fprintf(stderr, "[irq] IRET restore at srr0=0x%05X\n", ctx->pc & SPU_LS_MASK);
      } }
    if (ctx->interp_span_lo < ctx->interp_span_hi &&
        ctx->pc >= ctx->interp_span_lo && ctx->pc < ctx->interp_span_hi) {
        spu_staged_interp_resume(ctx);
        return;
    }
    /* SPURS kernel services (policy-module runs only): the HLE kernel plants
     * these two reserved addresses as exitToKernelAddr / selectWorkloadAddr in
     * the SpursKernelContext (spurs_policy.c). */
    if (ctx->policy_mode) {

        if (ctx->pc == SPURS_PM_EXIT_TO_KERNEL_LS) {
            /* Module exit: the workload returned to the kernel (drained/yield).
             * Print gated: fires once per policy run = thousands/sec. */
            g_spurs_pm_exited = 1;
            { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_PM_TRACE") ? 1 : 0;
              if (s_t) { static unsigned long _n = 0; unsigned long n = ++_n;
                if (n <= 64 || (n & 0xFFF) == 0)
                    fprintf(stderr, "[spurs-pm] exit-to-kernel#%lu (r3=0x%08X polls=%u)\n",
                            n, ctx->gpr[3]._u32[0], g_spurs_pm_polls); } }
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (ctx->pc == SPURS_PM_SELECT_WORKLOAD_LS) {
            /* Keep the current virtual SPU's workload selected. */
            unsigned n = ++g_spurs_pm_polls;
            if (spu_lane_trace() && (n <= 4 || (n % 4096) == 0))
                fprintf(stderr, "[spurs-pm] poll #%u (r3=0x%08X) -> continue\n",
                        n, ctx->gpr[3]._u32[0]);
            ctx->gpr[3] = spu_make_preferred_u32(spu_pm_current_workload(ctx));
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }

    /* Kernel-home landing probe (MS1 wwsjob PM): the run halted at LS 0x4 with
     * no lifted code and no stub -- log what the PM was asking the kernel for
     * before the synthesised stop ends it. */
    if (ctx->policy_mode) {
        uint32_t _pl = ctx->pc & SPU_LS_MASK;
        if (_pl < 0xA00 && _pl != SPURS_PM_EXIT_TO_KERNEL_LS
                       && _pl != SPURS_PM_SELECT_WORKLOAD_LS) {
            static int _ln = 0;
            if (_ln++ < 16) {
                fprintf(stderr, "[pm-land] kernel-home landing pc=0x%05X lr=0x%05X "
                        "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X "
                        "r7=%08X srr0=0x%05X irq_ret=0x%05X ie=%d st=%X\n",
                        _pl, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                        ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0],
                        ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0],
                        ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0],
                        ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0],
                        ctx->srr0 & SPU_LS_MASK, ctx->irq_resume_pc & SPU_LS_MASK,
                        (int)ctx->int_enable, ctx->status);
                fprintf(stderr, "      last dispatched PCs (oldest first):");
                for (unsigned q = 0; q < 8; q++) {
                    unsigned idx = (g_spu_pch_n + q) & 7u;
                    if (g_spu_pch_n > q || g_spu_pch[idx])
                        fprintf(stderr, " 0x%05X", g_spu_pch[idx]);
                }
                fprintf(stderr, "%c", 10);
                fprintf(stderr, "      LS[0x00]:");
                for (unsigned o = 0; o < 0x40; o += 4) {
                    uint32_t v = ((uint32_t)ctx->ls[o] << 24) | ((uint32_t)ctx->ls[o+1] << 16)
                               | ((uint32_t)ctx->ls[o+2] << 8) | ctx->ls[o+3];
                    fprintf(stderr, " %08X", v);
                }
                fprintf(stderr, "%c", 10);
            }
        }
    }

    /* M10 op-dispatch probe (MS1 wwsjob PM, state 2 kExecuteCommands): the
     * cmdlist scanner (0x1048) decodes ops from the fetched cmdlist and
     * dispatches handlers via the jump table at LS 0x1590 -- op0/6/7=0x21C0,
     * op1=0x1AB0 (claim + jump [0x14C0]), op2=0x1B88 (buffer GET/PUT),
     * op3=0x2098, op4=0x2138 (store), op5=0x2184. The scan's verdict lands
     * in r3 when the drain resumes the caller at 0x2244: r3!=0 advances
     * loadJobState 2->3 (the transition that never commits). Log every op
     * entry (label space) and the 0x2244 resume with its r3. */
    if (ctx->policy_mode) {
        uint32_t _pp = ctx->pc & SPU_LS_MASK;
        static int _perpc[16];
        int _pi = 0;
        int _is_op = 0;
        switch (_pp) {
        case 0x21C0: _pi = 0; _is_op = 1; break;
        case 0x1AB0: _pi = 1; _is_op = 1; break;
        case 0x1B88: _pi = 2; _is_op = 1; break;
        case 0x2098: _pi = 3; _is_op = 1; break;
        case 0x2138: _pi = 4; _is_op = 1; break;
        case 0x2184: _pi = 5; _is_op = 1; break;
        case 0x2180: _pi = 6; _is_op = 1; break;
        case 0x1AB4: _pi = 7; _is_op = 1; break;
        case 0x2244: _pi = 8; _is_op = 1; break;
        case 0x2250: _pi = 9; _is_op = 1; break;
        case 0x26B8: _pi = 10; _is_op = 1; break;
        default: break;
        }
        if (_is_op && _perpc[_pi] < 40) {
            _perpc[_pi]++;
            fprintf(stderr, "[pm-op] pc=0x%05X r3=%08X r4=%08X r5=%08X r6=%08X "
                    "r11=%08X r12=%08X L14C0=%02X%02X%02X%02X ie=%d\n",
                    _pp, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0],
                    ctx->gpr[11]._u32[0], ctx->gpr[12]._u32[0],
                    ctx->ls[0x14C0], ctx->ls[0x14C1], ctx->ls[0x14C2],
                    ctx->ls[0x14C3], (int)ctx->int_enable);
            fflush(stderr);
        }
    }
    /* Synthetic HLE tasks have no resident taskset policy at 0xA70. Real
     * tasksets use the SAME syscall address, so the API table alone is not
     * evidence that this is an HLE context. LLE tasks must enter Sony's policy
     * to yield/select workloads instead of parking an entire SPU host thread.
     * Image 22 is the legacy standalone HLE CRI task runner. */
    if (ctx->pc == SPURS_TASKSET_PM_SYSCALL_LS) {
        uint32_t sc = ((uint32_t)ctx->ls[0x27C4] << 24) | ((uint32_t)ctx->ls[0x27C5] << 16)
                    | ((uint32_t)ctx->ls[0x27C6] << 8)  | ctx->ls[0x27C7];
        /* A task dispatched standalone (no policy module in this local store)
         * runs on the SpursTasksetContext spu_workload.c plants, whose
         * syscallAddr is this address; nothing is lifted at 0xA70 for it, so
         * the only alternative to the HLE is the synthesised-stop exit below
         * -- which is how LBP's audio tasks (images 6/7) ended on their first
         * syscall and left the loading thread waiting on their event flag. */
        int lifted = spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->image_id) ||
            (ctx->resident_task && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_task)) ||
            (ctx->resident_ovl  && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_ovl));
        int standalone = !ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS && !lifted;
        if (ctx->image_id == 22 || standalone ||
            (ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS)) {
            spu_spurs_taskset_syscall(ctx);
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }
    /* YDKJ_CRI_R4: the taskset policy entry (LS 0xA00, image 23) writes r4 into
     * SpursTasksetContext.taskset @LS 0x27B8 (per RPCS3 cellSpursSpu.cpp). Our
     * kernel->policy handoff doesn't convey the taskset EA, so the policy DMAs
     * the taskset from garbage -> waiting!=0 -> wrong resume path -> savedContextLr=0.
     * Inject r4 = taskset EA (0x0F000000) at the policy entry dispatch (this is the
     * exact point before the entry reads r4, after the kernel's arg setup). */
    if (ctx->image_id == SPU_RESOLVER_ONLY_IMAGE_ID && !getenv("YDKJ_NO_CRI_R4")) {
        static int s_r4 = -1; if (s_r4 < 0) s_r4 = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_r4) {
            /* Force ctxt->taskset @LS 0x27B8 = the REAL game taskset EA on every
             * image-23 branch, so the policy's atomic reads + context-EA computation
             * use the actual taskset (the r4 handoff sets it to garbage 0x0000FFFF via
             * a path we can't intercept). Was hardcoded 0x0F000000, which mismatched
             * the game's real taskset (0x45F1B000) -> policy DMA'd garbage. */
            extern uint32_t g_ydkj_real_taskset_ea;
            uint32_t ts = g_ydkj_real_taskset_ea ? g_ydkj_real_taskset_ea : 0x0F000000u;
            ctx->ls[0x27B8]=0x00; ctx->ls[0x27B9]=0x00; ctx->ls[0x27BA]=0x00; ctx->ls[0x27BB]=0x00;
            ctx->ls[0x27BC]=(uint8_t)(ts>>24); ctx->ls[0x27BD]=(uint8_t)(ts>>16); ctx->ls[0x27BE]=(uint8_t)(ts>>8); ctx->ls[0x27BF]=(uint8_t)ts;
            if (ctx->pc == 0xA00u) { static int _n=0; if (_n++ < 4)
                fprintf(stderr, "[cri-r4] policy entry pc=0xA00: forced ctxt->taskset LS[0x27B8]=0x0F000000\n"); }
        }
    }
    /* SPU_IBCOV: image-3 (Bink SPU) PC-page coverage. Track which 0x1000-byte LS
     * pages the task's indirect branches land in; dump the set periodically. If
     * coverage stays in the kernel/wait region (~0x13xxx) the decode routine never
     * runs; if it spans a wide high range, decode executes but doesn't output. */
    { static int s_cov = -1; if (s_cov < 0) s_cov = getenv("SPU_IBCOV") ? 1 : 0;
      if (s_cov && ctx->image_id == 3) {
        static uint8_t pages[64] = {0};   /* 64 pages * 0x1000 = 256KB LS */
        static uint64_t hits = 0;
        uint32_t pg = (ctx->pc & 0x3FFFF) >> 12;
        int newp = 0;
        if (pg < 64 && !pages[pg]) { pages[pg] = 1; newp = 1; }
        ++hits;
        if (newp || (hits % 200000) == 0) {
            char line[400]; int p = snprintf(line, sizeof line, "[ibcov3] hits=%llu tgt=0x%05X pages:", (unsigned long long)hits, ctx->pc & 0x3FFFF);
            for (int i = 0; i < 64; i++) if (pages[i]) p += snprintf(line+p, sizeof(line)-p, " 0x%X", i<<12);
            fprintf(stderr, "%s\n", line); }
      } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_last = 0; static uint64_t s_run = 0;
        if (ctx->pc == s_last) s_run++; else { s_last = ctx->pc; s_run = 1; }
        if ((++s_n % 2000000) == 0)
          fprintf(stderr, "[ibranch] %llu indirect branches; current target=0x%05X run=%llu\n",
                  (unsigned long long)s_n, ctx->pc, (unsigned long long)s_run); } }
    /* Policy-entry trace: the SPURS policy at LS 0xA00 branches on
     * r8 = word at LS[r6] (must be 32 for the path that sets the dispatch ptr
     * LS[0x780]). Log r3..r6 + the word the kernel handed it, to see why the
     * wrong branch is taken. Env YDKJ_POLTRACE. */
    if (ctx->pc == 0xA00u) {
        static int64_t pt=-2; if (pt==-2){ const char* e=getenv("YDKJ_POLTRACE"); pt=e?1:0; }
        if (pt) { static int _p=0; if (_p++ < 8) {
            /* re-lifted policy entry uses r80 (kernel-set context base): r44=LS[r80+0xC0] */
            uint32_t r80=ctx->gpr[80]._u32[0] & SPU_LS_MASK;
            const uint8_t* q = ctx->ls + ((r80+0xC0)&SPU_LS_MASK);
            uint32_t ctxw = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
            fprintf(stderr, "[POLTRACE] policy@0xA00 r3=%08X r4=%08X r80=%08X  LS[r80+0xC0]=%08X\n",
                ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], r80, ctxw);
            fflush(stderr);
        } }
    }
    /* Resolve both fresh launches and mid-function resumes from TaskInfo.
     * A scheduler call temporarily leaves the task region, but its return PC
     * is usually not an ELF entry. Prefer the policy's selected ELF so tasks
     * sharing an entry address cannot shadow one another. Keep the legacy
     * entry-only mapping for runners that have not registered ELF metadata. */
    if (ctx->pc < SPU_TASKSET_TASK_LO) {
        ctx->resident_task = 0;
    } else {
        int ti = spu_taskset_resident_image(ctx);
        if (ti && !ctx->resident_task && ctx->host_depth &&
            s_spu_halt_armed && !ctx->policy_mode) {
            /* The policy restored a task's guest registers and branches to
             * its saved PC. Its host frames still belong to the scheduler or
             * an earlier task invocation; none can satisfy this task's return.
             * Resume at depth zero so SPU_RET follows the restored guest link. */
            ctx->resident_task = ti;
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
        if (!ti && !ctx->resident_task) ti = spu_taskset_task_image(ctx->pc);
        if (ti) ctx->resident_task = ti;
    }
    /* The launched task owns the shared task-code region: resolve it there FIRST
     * so a co-resident task at the same LS base cannot shadow it. (resident_task
     * is 0 outside the region, so this only ever fires for genuine task code.) */
    /* Retain the interpreted policy path before legacy SMC microsteps.
     * Only an existing first-owner registry hit may enter native job code,
     * apart from a registered policy service called by such code. */
    if (g_spu_pm_svc_on && spu_native_owned_policy(ctx) && pm_svc_branch(ctx)) return;
    if (spu_native_owned_policy(ctx) &&
        (!spu_native_owned_can_run(ctx,ctx->pc) || !spu_lifted_lookup(ctx,ctx->pc))) {
        spu_native_owned_interpret(ctx); return;
    }
    spu_fn fn = NULL;
    int code_owner = 0;
    int span_rel = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (ctx->resident_code[slot].image_id && ctx->pc >= ctx->resident_code[slot].lsa &&
            ctx->pc - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size) {
            code_owner = ctx->resident_code[slot].image_id;
            span_rel = (int)ctx->resident_code[slot].rel;
            fn = spu_lookup(ctx->pc - ctx->resident_code[slot].rel,
                           code_owner);
            break;
        }
    }
    /* Round-11: did the staged jobmod ever DISPATCH? Every transfer into a
     * claimed resident_code span logs the resolver verdict: a lift hit runs
     * natively; a miss falls to the interpreter span below (and with no
     * span it is the "job claimed, never entered" signature). Lift hits are
     * every native job transfer, so they log only with
     * SPU_JOBMOD_DISPATCH_LOG=1: printing them cost the vehicle job about an
     * eighth of its time in stderr writes and the CRT lock (#327). Misses
     * always log. Each verdict has its own counter. */
    if (code_owner) {
        static SPU_THREAD_LOCAL int s_hit_log = -1;
        if (s_hit_log < 0) {
            const char* e = getenv("SPU_JOBMOD_DISPATCH_LOG");
            if (!e || !e[0] || (e[0] == '0' && !e[1])) s_hit_log = 0;
            else if (e[0] == '1' && !e[1]) s_hit_log = 1;
            else { fprintf(stderr, "SPU_JOBMOD_DISPATCH_LOG must be 0 or 1, got '%s'\n", e); abort(); }
        }
        static SPU_THREAD_LOCAL unsigned s_hits, s_misses;
        unsigned _d = fn ? (s_hit_log ? ++s_hits : 0u) : ++s_misses;
        if (_d && (_d <= 16 || (_d % 64u) == 0)) {
            fprintf(stderr, "[pm-jobmod] DISPATCH img=%d target=0x%05X "
                    "resolver=%s rel=%d lr=0x%05X r3=0x%08X r4=0x%08X\n",
                    code_owner, ctx->pc & SPU_LS_MASK,
                    fn ? "lift" : "MISS->interp", span_rel,
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
            fflush(stderr);
        }
    }
    if (!code_owner && ctx->resident_task)
        fn = spu_lookup(ctx->pc, ctx->resident_task);
    /* Resident overlay next: streamed code overwrote that LS range, so its lift
     * is the truth there -- the base image's stale bytes at the same addresses
     * may also be registered (historical junk lifts) and must lose. */
    if (!code_owner && !fn && ctx->resident_ovl) fn = spu_lookup(ctx->pc, ctx->resident_ovl);
    /* img_ls_base retry (policy modules): absolute LS-space targets. */
    if (!code_owner && !fn) fn = (spu_fn)spu_lifted_lookup(ctx, ctx->pc);
    /* The job returned through the link register we planted: it is finished.
     * Its outermost frame ends in `bi $r0`, and r0 was 0 -- so without this the
     * return landed on LS 0, which is the job's OWN entry, and it ran a second
     * lap with dead registers: re-reading its parameters through a now-zero
     * context pointer and spinning on a DMA to what were really its own
     * opcodes. The work is already done by the time it returns. */
    if (!ctx->policy_mode && ctx->pc == SPU_JOB_RETURN_LS) {
        static int _n = 0;
        if (_n++ < 4)
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager \n",
                    ctx->image_id);
        spu_halt(ctx);
        return;
    }
    /* Branch into high LS with no lifted code there.
     *
     * This was added believing the jobs called a resident SPURS job-manager
     * kernel at 0x16100 / 0x18160 / 0x1A100. That was WRONG. Those "absolute
     * branches" were the disassembler decoding ASCII as instructions: the
     * bytes behind one of them are 30 2C 20 43, i.e. "0, C" from the string
     * "ch0, F:0:400, 100, Channel 0 level, %". Every job embeds the same
     * parameter-description text, which is why the same phantom target
     * appeared in 9 of 12 unrelated images and looked like a shared ABI.
     *
     * The guard is kept because branching into unlifted high local store is
     * still an error worth stopping at rather than executing whatever is
     * there -- but it is a backstop, not a kernel interface. */
    /* SPU exit: the CRT does not branch to a `stop` in the image -- it BUILDS one
     * in memory and jumps to it. ps1_netemu's GPU core ends with
     *
     *     ori $r80,$r3,0 / andi $r80,$r80,255 / iohl $r80,0x2000
     *     stqd $r80,0x10($r1) / sync / ai $r3,$r1,16 / bi $r3
     *
     * i.e. `stop (0x2000 | status)` assembled onto the stack and executed there.
     * That target is a stack address with no lifted code, so the unlifted-branch
     * guard below called a clean exit "branched into unlifted LS 0x3FFB0 -- ending
     * the job" and reported stop_code 0, which reads as a runaway and sent a long
     * chase after phantom memory corruption. Decode the target word instead: a
     * `stop` is opcode 0 in the top 11 bits, so this is unambiguous. */
    if (!fn) {
        uint32_t p0 = ctx->pc & SPU_LS_MASK;
        if (p0 + 3 < SPU_LS_SIZE) {
            uint32_t w = ((uint32_t)ctx->ls[p0] << 24) | ((uint32_t)ctx->ls[p0+1] << 16) |
                         ((uint32_t)ctx->ls[p0+2] << 8) | ctx->ls[p0+3];
            if ((w >> 21) == 0u) {                 /* stop / stopd */
                /* Hardening (Fix A): the primary boot-race cause was the
                 * microstep lqa/stqa scale bug (fixed above) sending the PM
                 * module-call tail into virgin LS 0x39004, where this block
                 * read zero words as "synthesised stop 0x0000" and killed
                 * both PM lanes ([pm-end] halted=1 pc=0x39004,
                 * native_usercheck.err:4502-4506 -> WWS minActive stall ->
                 * PPU spin on 0x40044480). A policy module never ends a run
                 * with a zero stop: its sanctioned exits are the kernel
                 * vectors (0x9C0/0x9D0) and real stop codes. If some future
                 * path strands an interrupt excursion on zero words again,
                 * bail recoverably (restore the TAKE register file, resume
                 * at srr0) instead of halting; non-policy contexts keep the
                 * loud exit. */
                if (ctx->policy_mode && w == 0u && ctx->irq_saved &&
                    spu_pm_recoverable_bail(ctx, p0, 0)) {
                    spu_staged_interp_resume(ctx);
                    return;
                }
                ctx->stop_code = w & 0x3FFFu;
                ctx->status = SPU_STATUS_STOPPED_BY_STOP;
                static int _n = 0;
                if (_n++ < 8)
                    fprintf(stderr, "[spu] img=%d exit: synthesised stop 0x%04X at LS 0x%05X\n",
                            ctx->image_id, ctx->stop_code, p0);
                spu_halt(ctx);
                return;
            }
        }
    }

    /* Staged-codec content probe (flashATRAC / flashMP3 self-staging
     * overlays). A SPURS task can DMA a LIFTED codec into its own LS at any
     * runtime base and branch to base+e_entry (img 1 stages flashATRAC at
     * 0x32880, img 2 at 0x33880 and 0x2F780). Before the unlifted-branch
     * guard ends/interprets the job, hash the LS window each registered
     * stage probe names (spu_workload.c: FNV-1a-64 over the codec's primary
     * PT_LOAD bytes at the implied base pc-entry, exact-match against the
     * lifted image's fingerprint -- the same content key
     * spu_workload_dispatch uses). On a hit with a LIFT LINKED AT that
     * base (per-base codec lifts registered with lift_base = base), claim a
     * resident_code slot and dispatch natively; otherwise log and serve the
     * staged bytes through the interpreter below (module-relative lifts
     * cannot run at a base). */
    if (!fn && ctx->pc >= SPU_JM2_KERNEL_BASE) {
        uint32_t probe_base = 0, probe_lift = 0, probe_span = 0;
        int probe_img = 0;
        if (spu_workload_stage_probe(ctx->ls, ctx->pc & SPU_LS_MASK,
                                     &probe_base, &probe_img,
                                     &probe_lift, &probe_span)) {
        { static int _interp_msk = -1;
          if (_interp_msk < 0) { _interp_msk = 0; const char* e = getenv("SPU_PROBE_INTERP");
            /* A/B knob: comma list of image ids to force through the staged
             * interpreter even when a link-base lift exists (native lift
             * under suspicion). "1" (legacy) forces ALL images. */
            if (e) for (const char* p = e; *p; ) { long v = strtol(p, (char**)&p, 10);
                if (v == 1 && (*p == 0)) { _interp_msk = -2; break; } /* bare "1" = all */
                if (v > 0 && v < 32) _interp_msk |= 1 << v;
                while (*p == ',' || *p == ' ') ++p; } }
          if ((_interp_msk == -2) || (_interp_msk & (1 << probe_img))) probe_lift = 0; }
         if (probe_lift && probe_lift == probe_base) {
        /* Link-base lift (lift_base = the implied staging base): the lift's
         * labels, links and function table are compiled AT this base, so it
         * can run natively exactly like the PM jobmods' link-base 0x4000
         * lifts. Claim a resident_code slot (content key: the probe hash;
         * source_ea 0 = no EA anchor, any GET into the span re-probes) and
         * dispatch through it. rel = base - lift_base = 0 at the lift's own
         * base; the slot's resolver fast path serves later in-span branches
         * without re-hashing. */
        /* Span exclusivity: two runtime-staged modules can land on
         * OVERLAPPING windows (MS1 audio: flashMP3 stages at 0x2F780,
         * 0x888 inside the compressor's [0x2EF80,0x2FD00) claim -- and the
         * compressor's +0x888 entry 0x2F808 sits inside the codec's span).
         * A resident slot's fast path asserts "this span IS my image's
         * content"; with an overlap both assertions cannot hold, and the
         * first-slot-wins resolver then serves one module's function for
         * the OTHER module's branch (observed: codec entry branch runs the
         * compressor lift with codec register state -> MP3 lane dead,
         * 100% silence). The fresh probe hit is the content authority for
         * its window: evict any other slot overlapping it. The evicted
         * module re-probes on its next miss (content decides, correct
         * either way -- the interpreter serves anything unresolved). */
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (!ctx->resident_code[slot].image_id) continue;
            uint32_t obase = ctx->resident_code[slot].lsa;
            uint32_t oend  = obase + ctx->resident_code[slot].size;
            if (probe_base < oend && probe_base + probe_span > obase) {
                { static int _n = 0;
                  if (_n++ < 16)
                      fprintf(stderr, "[spu-ovl] staged-codec probe HIT: image %d "
                              "span [0x%05X,0x%05X) overlaps image %d span "
                              "[0x%05X,0x%05X) -- evicting stale claim\n",
                              probe_img, probe_base, probe_base + probe_span,
                              ctx->resident_code[slot].image_id, obase, oend); }
                ctx->resident_code[slot].image_id = 0;
            }
        }
        int pick = -1;
        for (unsigned slot = 0; slot < 4; ++slot)
            if (!ctx->resident_code[slot].image_id ||
                ctx->resident_code[slot].lsa == probe_base) { pick = (int)slot; break; }
        spu_fn pfn = pick >= 0
            ? spu_lookup(ctx->pc - (probe_base - probe_lift), probe_img) : NULL;
        if (pfn) {
            ctx->resident_code[pick].lsa = probe_base;
            ctx->resident_code[pick].size = probe_span;
            ctx->resident_code[pick].source_ea = 0;
            ctx->resident_code[pick].image_id = probe_img;
            ctx->resident_code[pick].rel = probe_base - probe_lift;
            { static int _n = 0;
              if (_n++ < 16)
                  fprintf(stderr, "[spu-ovl] staged-codec probe HIT: image %d "
                          "staged at LS 0x%05X span 0x%X -- dispatching natively "
                          "(link-base lift 0x%05X)\n",
                          probe_img, probe_base, probe_span, probe_lift); }
            fn = pfn;   /* common tail: musttail dispatch into the lift */
        } }
        if (!fn)
        { static int _n = 0;
          if (_n++ < 16)
              fprintf(stderr, "[spu-ovl] staged-codec probe HIT: image %d "
                      "staged at LS 0x%05X span 0x%X -- serving via interpreter "
                      "(no lift linked at this base)\n",
                      probe_img, probe_base, probe_span);
        }
        /* probe_lift == 0 (module-relative lift, e.g. flashMP3) or the
         * link-base lookup missed: NO resident_code claim -- a module-
         * relative lift's absolutes are module offsets (spu_ls_write128
         * 0x5450, spu_link 0x98); dispatching it at a runtime base writes
         * those offsets into UNRELATED LS (wave5 round-4: silence). Fall
         * through with fn == NULL: the SPU_INTERP_UNLIFTED guard interprets
         * the staged bytes and rejoins the compiled path on return. */
        } else {
            /* Probe-miss forensics: persist the LS window around the branch
             * target once per pc so the staged bytes can be diffed offline
             * against the codec PT_LOAD slice (partial/late staging, self
             * modification, or a different image entirely). */
            static uint32_t dumped[4]; static int n_dumped = 0;
            uint32_t pc = ctx->pc & SPU_LS_MASK;
            int seen = 0;
            for (int i = 0; i < n_dumped; i++) if (dumped[i] == pc) seen = 1;
            if (!seen && n_dumped < 4) {
                dumped[n_dumped++] = pc;
                uint32_t lo = pc > 0x400u ? pc - 0x400u : 0u;
                uint32_t span = 0x8400u;
                if (lo + span > SPU_LS_SIZE) span = SPU_LS_SIZE - lo;
                char path[96];
                const char* miss_dir = getenv("SPU_STAGED_MISS_DIR");
                snprintf(path, sizeof path, "%s/spu_staged_miss_%05X.bin",
                         miss_dir ? miss_dir : "recomp/out/debug_feasibility", pc);
                FILE* f = fopen(path, "wb");
                if (f) { fwrite(ctx->ls + lo, 1, span, f); fclose(f);
                         fprintf(stderr, "[spu-ovl] probe miss at 0x%05X: dumped LS [0x%05X,+0x%X) to %s\n",
                                 pc, lo, span, path); }
            }
        }
    }

    /* (policy lanes included: the wwsjob PM stages its job code high in LS and
     * bisl's into it with a label-space link -- the span interpreter below is
     * what runs it and hands the return back to spu_drain_call.) */
    if (!fn && ctx->pc >= SPU_JM2_KERNEL_BASE) {
        static uint32_t seen[16]; static int n_seen = 0;
        int known = 0;
        for (int i = 0; i < n_seen; i++) if (seen[i] == ctx->pc) { known = 1; break; }
        if (!known && n_seen < 16) {
            seen[n_seen++] = ctx->pc;
            fprintf(stderr, "[spu] img=%d branched into unlifted LS 0x%05X "
                    "(lr=0x%05X) -- ending the job\n",
                    ctx->image_id, ctx->pc, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            { fprintf(stderr, "      last dispatched PCs (oldest first):");
              for (unsigned q = 0; q < 8; q++) {
                  unsigned idx = (g_spu_pch_n + q) & 7u;
                  if (g_spu_pch_n > q || g_spu_pch[idx]) fprintf(stderr, " 0x%05X", g_spu_pch[idx]);
              }
              fprintf(stderr, "%c", 10); }
            /* Is there real code at the target, or is the pc garbage? Eight
             * words at the target and at the return address separate "the lift
             * missed a function" from "this branch should never have happened". */
            { uint32_t a[2]; a[0] = ctx->pc & SPU_LS_MASK;
              a[1] = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
              for (int k = 0; k < 2; k++) {
                  for (uint32_t o = 0; o < 32 && a[k] + o + 3 < SPU_LS_SIZE; o += 4)
                      fprintf(stderr, " %02X%02X%02X%02X",
                              ctx->ls[a[k]+o], ctx->ls[a[k]+o+1],
                              ctx->ls[a[k]+o+2], ctx->ls[a[k]+o+3]);
                  fprintf(stderr, "\n");
              } }
            fflush(stderr);
        }
        /* SPU_INTERP_UNLIFTED: if real code is present at the target, run it
         * through the interpreter instead of ending the job. No lift can exist
         * for a module the title loads at runtime -- You Don't Know Jack's FMOD
         * mixer relocates a DSP plugin into local store and calls it, and MS1's
         * msngSPURS_ATRAC task stages the flashATRAC codec at LS 0x33880 and
         * calls its entry (0x33910 = base + e_entry 0x90). The interpreter
         * rejoins the compiled path as soon as it reaches a lifted address,
         * which is what the module's return does. Default ON (the staged-code
         * case can never have a static lift); set SPU_INTERP_UNLIFTED=0 to
         * restore the loud stop for a genuinely bad pc. */
        { uint32_t p = ctx->pc & SPU_LS_MASK;
          uint32_t w0 = ((uint32_t)ctx->ls[p] << 24) | ((uint32_t)ctx->ls[p+1] << 16) |
                        ((uint32_t)ctx->ls[p+2] << 8) | ctx->ls[p+3];
          static int s_iu = -1;
          if (s_iu < 0) { const char* e = getenv("SPU_INTERP_UNLIFTED");
              s_iu = (e && e[0] == '0' && !e[1]) ? 0 : 1; }
          /* Hardening (Fix A2): a policy lane's SN handler bisl's into
           * register-save/restore shims it generates at the top of LS
           * (0x3F1C0 / 0x3F1F0 in native_usercheck.err:4458-4489). Those are
           * runtime-built bytes with no lift; before the microstep lqa/stqa
           * scale fix (above) they decoded corrupt and the flow derailed
           * into virgin LS. Keep a policy-mode escape on the stub page (top
           * 4 KB): route the target through the host recoverable bail, which
           * finishes the stall-and-notify service the handler was taken for
           * (ack parked lists + the SN event; WrListStallAck/WrEventAck
           * semantics) and resumes at srr0 -- the same contract as the
           * 0x9C0/0x9D0 kernel-vector interception above. Non-policy staged
           * code (codecs, DSP plugins) keeps the interpreter path. */
          if (ctx->policy_mode && p >= SPU_LS_SIZE - 0x1000u &&
              spu_pm_recoverable_bail(ctx, p, 1)) {
              spu_staged_interp_resume(ctx);
              return;
          }
          if (s_iu && w0) {
              static int _n = 0;
              if (_n++ < 8)
                  fprintf(stderr, "[spu] img=%d interpreting unlifted LS 0x%05X "
                          "(runtime-loaded code)\n", ctx->image_id, p);
              /* No rejoin inside the staged span: the base image's own table
               * can cover the same addresses (img 2 has a function at 0x34328
               * = codec+0xAA8), and those entries are not what is in the LS
               * now. Returns to the loader below the span still rejoin. */
              /* A staged job entered via bisl returns through the link the
               * caller planted in r0. On PM lanes (img_ls_base != 0) that link
               * is a file-relative label with no lifted entry (the
               * continuation is mid-function), so the rejoin test can never
               * fire: without an explicit stop point the interpreter sails
               * past the return and wanders the PM image at the wrong offset
               * (MS1: job staged at 0x16230 returned to 0x26A8, ran on
               * through unrelated bytes and died at a synthesised stop at
               * 0x4/0x9D4, killing the Execute phase). Stop at the link so
               * spu_drain_call(return_pc) in the lifted caller resumes the
               * compiled path where the bisl left off. */
            uint32_t _link = ctx->gpr[0]._u32[0] & 0x3FFFC;
            ctx->interp_span_lo = p; ctx->interp_span_hi = SPU_LS_SIZE;
            /* Honor the link on EVERY staged entry, not just PM lanes: a
             * taskset context (img_ls_base==0) whose lifted caller bisl'd a
             * runtime-staged codec returns to a MID-FUNCTION link (MS1:
             * flashATRAC called at 0x32910, link 0x72B8) -- the below-span
             * rejoin only fires at exact lifted entries, so without this stop
             * the interpreter sails past the return and the caller's
             * spu_drain_call bracket never pops (every later audio round ran
             * interpreted, 15-63 ms). Zero link = entered without bisl: keep
             * the old open-ended span. */
            ctx->interp_return_pc = _link ? _link : 0;
              spu_staged_interp_resume(ctx);
              return;
          } }
        spu_halt(ctx);
        return;
    }
    /* WWS job-code overlay match: the PM (image 2) streams a job module into the
     * code buffer (base = LS[0x1320]) and branches to base+entryOffset. Its 16-byte
     * ila header identifies which of the 89 lifted job modules it is; match it and
     * mark that overlay resident so the module's lifted functions dispatch. The
     * module arrives via a DMA LIST, so the single-GET note_get hook never fired. */
    if (!fn && ctx->image_id == 2 && ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t codeBase = ((uint32_t)ctx->ls[0x1320] << 24) | ((uint32_t)ctx->ls[0x1321] << 16) |
                            ((uint32_t)ctx->ls[0x1322] << 8) | ctx->ls[0x1323];
        if (codeBase + 16 <= SPU_LS_SIZE) {
            extern int spu_overlay_match_sig(const uint8_t hdr[16]);
            int img = spu_overlay_match_sig(&ctx->ls[codeBase]);
            if (img) {
                if (ctx->resident_ovl != img) {
                    ctx->resident_ovl = img;
                    static int _n = 0; if (_n++ < 32)
                        fprintf(stderr, "[spu-ovl] WWS job module resident: codeBase=0x%05X "
                                "pc=0x%05X -> overlay image %d\n", codeBase, ctx->pc & SPU_LS_MASK, img);
                }
                fn = spu_lookup(ctx->pc, img);
            }
        }
    }
    /* WWS job-code fallback: the jobmanager PM (image 2) stages each job's
     * code into an LS buffer and branches into it. The staged blobs are
     * lifted as their own images at their LS residence addresses (image 1 =
     * job_wws_7BD900 at the 0x14400-region buffer); the PM's own table can
     * never contain them. On a policy-module miss inside the job-buffer
     * range, retry against the job image before falling to the interpreter. */
    /* WWS job-code fallback -- but ONLY when real code is staged at the target.
     * A PM job whose code buffer is empty (the {0xA400,0} null-code placeholder)
     * must NOT resolve to image-1's unrelated function that happens to share the
     * file offset: that ran garbage, the job did no work, and its ring slot
     * never advanced -> the loader's job ring deadlocked. An empty buffer falls
     * through to the null-job handling below. */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t w0 = ((uint32_t)ctx->ls[ctx->pc] << 24) | ((uint32_t)ctx->ls[ctx->pc+1] << 16) |
                      ((uint32_t)ctx->ls[ctx->pc+2] << 8) | ctx->ls[ctx->pc+3];
        if (w0 != 0)
            fn = spu_lookup(ctx->pc, 1);
    }
    /* Null-code PM job (empty buffer): return control to the dispatcher's own
     * host-call bracket so its post-job continuation runs the completion
     * bookkeeping -- exactly as a real job that did nothing and returned.
     * (Falling to the SMC microstep would log a spurious BRANCH-TO-0 on the
     * 0x00000000 word; this is the same outcome, quiet.) */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000 &&
        ((uint32_t)ctx->ls[ctx->pc] | ctx->ls[ctx->pc+1] |
         ctx->ls[ctx->pc+2] | ctx->ls[ctx->pc+3]) == 0) {
        static int _n = 0;
        if (_n++ < 8) {
            /* Ground-truth dump of the WWS code-buffer resolution at dispatch
             * (wwsjob-rosetta offsets). jobEntry = lsaJobCodeBuffer + [buf+0x10];
             * lsaJobCodeBuffer = Gbt_bufferPageNum<<10 from the RunJob command.
             * If the code buffer really is 0x4000 but empty -> the code LOAD is
             * the bug; if lsaJobCodeBuffer should be 0x14400 -> the RunJob
             * buffer-set resolution is. Dump both + the bufferSetArray rows. */
            uint32_t lsaCode = (uint32_t)ctx->ls[0x1330]<<24 | ctx->ls[0x1331]<<16 |
                               ctx->ls[0x1332]<<8 | ctx->ls[0x1333];
            uint32_t runJob  = (uint32_t)ctx->ls[0x12E0]<<24 | ctx->ls[0x12E1]<<16 |
                               ctx->ls[0x12E2]<<8 | ctx->ls[0x12E3];
            const uint8_t* bs = ctx->ls + 0xDF0;   /* g_WwsJob_bufferSetArray */
            fprintf(stderr, "[pm-jobres] pc=0x%05X lsaJobCodeBuffer(0x1330)=0x%08X "
                    "runJobNum(0x12E0)=0x%08X code@14400:%02X%02X%02X%02X\n"
                    "           bufferSetArray[0xDF0]: %02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, lsaCode, runJob,
                    ctx->ls[0x14400],ctx->ls[0x14401],ctx->ls[0x14402],ctx->ls[0x14403],
                    bs[0],bs[1],bs[2],bs[3], bs[4],bs[5],bs[6],bs[7],
                    bs[8],bs[9],bs[10],bs[11], bs[12],bs[13],bs[14],bs[15]);
        }
        return;   /* enclosing SPU_DRAIN bracket resumes the dispatcher */
    }
    /* JOB-BODY EXECUTION probe: the WWS job manager enters a staged job via
     * `bie pJobCode` (changeloadtorunjob.spu), an INDIRECT branch to
     * lsaJobCodeBuffer + entryOffset. A successful dispatch with the pc inside
     * the job code buffer and a jobmod overlay resident (image ids 200+) is the
     * proof the lifted job module actually runs. Env SPU_JOBEXEC=1. */
    /* SPU_JOBTRACE=1: log EVERY dispatcher entry while a job module (overlay
     * id >= 200) is resident -- the job's cross-function control-flow trail.
     * The pm-flow drain hook can't see a job that performs no channel ops
     * (LBP's frozen job runs as a complete no-op: entry, no DMAs, return);
     * this names its early-out path instead. */
    { static int s_jt = -1;
      if (s_jt < 0) { const char* e = getenv("SPU_JOBTRACE"); s_jt = e ? 1 : 0; }
      if (s_jt && ctx->resident_ovl >= 200) { static int _n = 0;
        uint32_t p = ctx->pc & SPU_LS_MASK;
        /* Full trail, but only start logging once the job body proper begins
         * (pc in the job code buffer >= 0x4000) so the cap isn't spent on the
         * PM code the job calls back into -- and keep logging thereafter. */
        static int _armed = 0;
        if (p >= 0x4000) _armed = 1;
        if (_armed && _n++ < 1500) {
            fprintf(stderr, "[jobtrace] pc=0x%05X lr=0x%05X%s",
                    p, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    fn ? "" : " (no lift)");
            /* JobApi entries: args r3-r5. Post-job return 0x3308: result r3 +
             * the job's saved GetBufferTag result r80. */
            if (p == 0x2F30 || p == 0x1700 || p == 0x1770 || p == 0x17C8)
                fprintf(stderr, " args r3=%08X.%08X r4=%08X r5=%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
            if (p == 0x3308 || p == 0x3258)
                fprintf(stderr, " r3=%08X.%08X.%08X.%08X r80=%08X.%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                        ctx->gpr[80]._u32[0], ctx->gpr[80]._u32[1]);
            /* Mid-GetBufferTag: r8 = the set/buf id bytes computed at 0x2F30
             * entry; if wrong here, something clobbered it ACROSS the
             * GetLogicalBuffer call (interrupt delivery?). */
            if (p == 0x2F6C)
                fprintf(stderr, " r8=%08X r10=%08X r13=%08X r15=%08X int=%d",
                        ctx->gpr[8]._u32[0], ctx->gpr[10]._u32[0],
                        ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0],
                        ctx->int_enable);
            if (p == 0x3030)   /* final BufferTag in r7 (result) */
                fprintf(stderr, " BufferTag r7=%08X.%08X",
                        ctx->gpr[7]._u32[0], ctx->gpr[7]._u32[1]);
            fprintf(stderr, "\n");
        }
      } }
    /* LBP_HLE_JOBDONE=1: HLE the WWS job COMPLETION. Our lifted SPURS PM never
     * reaches the store/DecrementDependency phase (the multi-SPU barrier never
     * converges), so a job's completion word -- passed by the PPU as param[4],
     * an EA in the 0x470Axxxx pool, embedded in the job's command list at LS
     * ~0xC00 -- is never written, and the PPU JobManagerWorker (guest-fn
     * 0x521BD4) spins on it forever (loading freeze). When a job body actually
     * dispatches (overlay >=200, entry 0x4A40), scan its staged command list
     * for the completion EA and write it nonzero, unblocking the worker. This
     * fakes ONLY the done-signal (not the job's data DMA), so it tests how far
     * the boot gets once loading stops deadlocking. */
    if (ctx->pc == 0x4A40 && ctx->resident_ovl >= 200) {
        static int s_hd = -1;
        if (s_hd < 0) { const char* e = getenv("LBP_HLE_JOBDONE"); s_hd = e ? 1 : 0; }
        if (s_hd) {
            extern uint8_t* vm_base;
            int wrote = 0;
            for (uint32_t o = 0xC00; o + 4 <= 0xE80 && wrote < 4; o += 4) {
                uint32_t w = (ctx->ls[o]<<24)|(ctx->ls[o+1]<<16)|(ctx->ls[o+2]<<8)|ctx->ls[o+3];
                if ((w & 0xFFFF0000u) == 0x470A0000u && vm_base) {
                    /* write nonzero to the completion word in main RAM (BE) */
                    if (vm_base[w]==0 && vm_base[w+1]==0 && vm_base[w+2]==0 && vm_base[w+3]==0) {
                        vm_base[w+3] = 1;
                        static int _n = 0; if (_n++ < 24)
                            fprintf(stderr, "[hle-jobdone] wrote completion 0x%08X=1 (job overlay %d)\n",
                                    w, ctx->resident_ovl);
                        wrote++;
                    }
                }
            }
        }
    }
    if (fn && ctx->pc >= 0x4000 && ctx->resident_ovl >= 200) {
        static int s_je = -1;
        if (s_je < 0) { const char* e = getenv("SPU_JOBEXEC"); s_je = e ? 1 : 0; }
        if (s_je) { static int _n = 0; if (_n++ < 24)
            fprintf(stderr, "[jobexec] DISPATCH job code pc=0x%05X overlay=%d "
                    "runJobNum=0x%02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->resident_ovl,
                    ctx->ls[0x12E0], ctx->ls[0x12E1], ctx->ls[0x12E2], ctx->ls[0x12E3]);
          /* SPU_JOBEXEC_DUMP=<path>: write the full 256KB LS the first time a job
           * entry (pc==codeBuffer+entryOffset) dispatches, to diff vs the RPCS3
           * oracle SPU4 dump (why our job bails where the oracle's runs). */
          static int s_dumped = 0;
          if (!s_dumped && ctx->pc == 0x4A40) {
              const char* dp = getenv("SPU_JOBEXEC_DUMP");
              if (dp) { FILE* f = fopen(dp, "wb");
                  if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f);
                      fprintf(stderr, "[jobexec] dumped LS at job entry -> %s\n", dp); }
                  s_dumped = 1; } } }
    }
    /* M12 [pm-mod]: resolver outcome for the staged-jobmod dispatch. The
     * op1 launch does `bi LS[0x14C0]` = 0x40244380 & 0x3FFFF = 0x4380; the
     * [pm-jobmod] hook proves REGISTRATION, not DISPATCH. This names which
     * slot/lift the resolver picks (or why fn==NULL) at the launch moment. */
    { static int s_m = -1;
      if (s_m < 0) s_m = getenv("SPU_PM_CLM") ? 1 : 0;
      if (s_m && ctx->policy_mode && ctx->image_id == 200 &&
          (ctx->pc & SPU_LS_MASK) >= 0x4000 &&
          (ctx->pc & SPU_LS_MASK) < 0x4000 + 0x12540) {
          static int _n = 0;
          if (_n++ < 40) {
              fprintf(stderr, "[pm-mod] #%d resolve pc=0x%05X fn=%s owner=%d "
                      "slots:",
                      _n - 1, ctx->pc & SPU_LS_MASK, fn ? "FOUND" : "NULL",
                      code_owner);
              for (unsigned _s = 0; _s < 4; _s++)
                  fprintf(stderr, " [%d]img=%d lsa=0x%X rel=%d sz=0x%X", _s,
                          ctx->resident_code[_s].image_id,
                          ctx->resident_code[_s].lsa,
                          ctx->resident_code[_s].rel,
                          ctx->resident_code[_s].size);
              fprintf(stderr, " r0=%08X r3=%08X w0=%02X%02X%02X%02X\n",
                      ctx->gpr[0]._u32[0], ctx->gpr[3]._u32[0],
                      ctx->ls[ctx->pc & SPU_LS_MASK],
                      ctx->ls[(ctx->pc & SPU_LS_MASK) + 1],
                      ctx->ls[(ctx->pc & SPU_LS_MASK) + 2],
                      ctx->ls[(ctx->pc & SPU_LS_MASK) + 3]);
              fflush(stderr);
          }
      } }
    if (fn) {
        spu_check_stack_reset(ctx, fn);
        PS3_SPU_RESOLVED(ctx, fn);
        /* MUSTTAIL: a guest loop that iterates through an indirect branch (the
         * Bink decoder's per-command dispatch does) must not grow the host
         * stack -- a plain call here leaked a resolver+callee frame per
         * iteration and blew the thread stack ~4k iterations into the first
         * really-decoding movie frame (silent 0x80000001 death). */
#if defined(__clang__)
        __attribute__((musttail)) return fn(ctx);
#else
        fn(ctx);
        return;
#endif
    }
    /* wwsjob JOB-CODE entry probe: the PM stages each job's code into a
     * buffer above its static image (0x3700..) and branches into it. No lift
     * exists at those pcs (the code arrives at runtime), so the branch lands
     * here. Log the entry + leading bytes -- the bytes identify WHICH job
     * blob was staged (match against lifted job images for dispatch). */
    /* Staged-job "return to kernel": a jm2-protocol job binary ends by
     * jumping to LS 0 (the jm2 kernel's home on real hardware). Inside the
     * wwsjob PM's LS, address 0 holds the interrupt VECTOR instead -- letting
     * the jump proceed runs the interrupt handler as the job's continuation
     * and wedges the pipeline (lanes freeze mid-queue). The PM called the job
     * with a return link in r0 (0x31C8 sets 0x3258); route the kernel-return
     * there: the PM resumes its post-job flow, which is exactly what the jm2
     * kernel hand-back does on hardware. */
    if (ctx->image_id == 2 && ctx->policy_mode && ctx->pc == 0 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) >= 0xA00 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) < 0x3700) {
        static int _n = 0;
        if (_n++ < 8)
            fprintf(stderr, "[wws-jobret] staged job returned to kernel (LS 0); "
                    "resuming PM at link 0x%05X\n", ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        spu_indirect_branch(ctx);      /* re-dispatch at the rewritten pc */
        return;
    }

    /* A SPURS *job* that branches back to LS 0 has RETURNED TO THE JOB MANAGER.
     * That is how a jm2 job signals completion: its crt tail-jumps to the
     * resident manager, which on hardware loads the next job and sets up r3/r4
     * afresh. We load the job at LS 0 and have no manager, so the jump lands on
     * the job's OWN entry and it runs a second lap -- with dead registers.
     *
     * Tokyo Jungle showed exactly that: lap one is correct at every step and
     * does the real work, then the crt jumps to 0 and lap two re-reads its
     * parameters through a now-zero context pointer (hence the reads of LS 0x20
     * and the DMA addresses that were really its own opcodes) and spins forever.
     * The work was already finished before the jump; the second lap is noise.
     *
     * The initial entry is called directly, not through here, so any arrival at
     * LS 0 in this path is a re-entry. Treat it as job end. */
    if (!ctx->policy_mode && ctx->pc == 0 && ctx->image_id > 0) {
        static int _n = 0;
        if (_n++ < 8)
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager "
                    "(branch to LS 0) -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    if (ctx->image_id == 2 &&
        ((ctx->pc >= 0x3700 && ctx->pc < 0x3FE80) ||
         (ctx->pc < 0xA00 && ctx->pc != SPURS_PM_EXIT_TO_KERNEL_LS &&
          ctx->pc != SPURS_PM_SELECT_WORKLOAD_LS))) {
        static int _n = 0;
        if (_n < 12) {
            _n++;
            const uint8_t* p = ctx->ls + (ctx->pc & SPU_LS_MASK);
            fprintf(stderr, "[wws-jobentry] pc=0x%05X lr=0x%05X bytes:"
                    " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                    p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            /* Companion state: the OTHER load target (the real 0x100-byte
             * loads land at 0x14400 -- possibly the actual job code, with the
             * entry computed from the wrong slot's base) + the job records
             * the entry math reads (0x1320 base block, 0x1440 batch record). */
            { const uint8_t* q = ctx->ls + 0x14400;
              const uint8_t* r1 = ctx->ls + 0x1320;
              const uint8_t* r2 = ctx->ls + 0x1440;
              fprintf(stderr, "[wws-jobentry]   LS14400: %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1320: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1440: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      q[0],q[1],q[2],q[3], q[4],q[5],q[6],q[7],
                      r1[0],r1[1],r1[2],r1[3], r1[4],r1[5],r1[6],r1[7],
                      r1[8],r1[9],r1[10],r1[11], r1[12],r1[13],r1[14],r1[15],
                      r2[0],r2[1],r2[2],r2[3], r2[4],r2[5],r2[6],r2[7]); }
            fflush(stderr);
        }
    }
    /* No lifted function at this PC: it may be RUNTIME-GENERATED code (the
     * WWS jobmanager writes a save/restore stub above its static
     * image and calls it). Interpret the live LS bytes; on success the next
     * branch re-enters lifted code via the trampoline. */
    if (spu_smc_microstep(ctx))
        return;
    /* Cap the unresolved-branch log PER IMAGE: a global cap let one noisy
     * image (the FMOD mixer's overlay calls) exhaust it and silently hide
     * every other image's misses -- LBP's loading jobs skipped their command
     * handlers for a whole session without a single log line. */
    { enum { BT0_MAX_IMG = 64, BT0_PER_IMG = 12 };
      static int _bt0[BT0_MAX_IMG];
      unsigned img = (ctx->image_id >= 0 && ctx->image_id < BT0_MAX_IMG)
                     ? (unsigned)ctx->image_id : 0;
      if (_bt0[img]++ < BT0_PER_IMG)
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    /* One-shot: the FMOD null-handler DSP node carries a PPU descriptor EA at
     * node+0x14 (observed 0x93C3C0). Dump it to identify which plugin/unit
     * type never got its SPU code streamed (env SPU_DSPDESC=<hex ea>). */
    { static int _d = -1; static uint32_t _ea = 0;
      if (_d < 0) { const char* e = getenv("SPU_DSPDESC");
        _ea = e ? (uint32_t)strtoul(e, 0, 16) : 0; _d = _ea ? 1 : 0; }
      if (_d == 1) { _d = 2;
        extern uint8_t* vm_base;
        fprintf(stderr, "[dspdesc] RAM[0x%08X]:", _ea);
        for (int k = 0; k < 0x60; k += 4)
            fprintf(stderr, " %02X%02X%02X%02X", vm_base[_ea+k], vm_base[_ea+k+1],
                    vm_base[_ea+k+2], vm_base[_ea+k+3]);
        fprintf(stderr, "\n"); fflush(stderr); } }
    /* SPU_MISS_DUMP_IMG=<n>: reserve the deep-dump budget for image n's misses
     * (the global 2-shot budget was always consumed by an earlier image's
     * misses, hiding the one under investigation). Unset = old behavior. */
    { static int s_img = -2; static uint32_t s_pcmin = 0;
      if (s_img == -2) { const char* e = getenv("SPU_MISS_DUMP_IMG"); s_img = e ? atoi(e) : -1;
        const char* p = getenv("SPU_MISS_DUMP_PCMIN"); s_pcmin = p ? (uint32_t)strtoul(p,0,0) : 0; }
      static int _n=0;
      uint32_t misspc = ctx->pc & SPU_LS_MASK;
      if ((s_img < 0 || ctx->image_id == s_img) && misspc >= s_pcmin && _n++ < 2) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
        /* State-diff oracle: dump full LS + all GPRs at the branch-to-0 so it can
         * be compared byte-for-byte against the RPCS3 savestate LS of the same
         * (cri_mpv) task. Path from YDKJ_SPU_LSDUMP, else ./recomp_spu_ls.bin. */
        const char* dp = getenv("YDKJ_SPU_LSDUMP");
        if (!dp || !*dp) dp = "recomp_spu_ls.bin";
        FILE* lf = fopen(dp, "wb");
        if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                  fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        /* echo the dispatch chain values the way func_00026DE0 computes them */
        { uint32_t bec0 = ctx->ls[0xBEC0]<<24 | ctx->ls[0xBEC1]<<16 | ctx->ls[0xBEC2]<<8 | ctx->ls[0xBEC3];
          fprintf(stderr, "[SPU] LS[0xBEC0].w0=0x%08X  LS[0x2d4e0:16]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            bec0,
            ctx->ls[0x2d4e0],ctx->ls[0x2d4e1],ctx->ls[0x2d4e2],ctx->ls[0x2d4e3],
            ctx->ls[0x2d4e4],ctx->ls[0x2d4e5],ctx->ls[0x2d4e6],ctx->ls[0x2d4e7],
            ctx->ls[0x2d4e8],ctx->ls[0x2d4e9],ctx->ls[0x2d4ea],ctx->ls[0x2d4eb],
            ctx->ls[0x2d4ec],ctx->ls[0x2d4ed],ctx->ls[0x2d4ee],ctx->ls[0x2d4ef]); }
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

/* Bounded by design: a --trace lift emits one call PER INSTRUCTION, so an image
 * that spins (a persistent SPURS policy module polling its job queue never
 * returns) would otherwise write stderr until the disk fills. The cap keeps the
 * useful part -- the entry path plus enough revolutions to show what the loop
 * tests -- and the tail IS the loop. SPU_TRACE_MAX=0 restores unbounded.
 * SPU_TRACE_FILE redirects off stderr so the trace does not interleave with the
 * boot log. */
/* Trace output is OPT-IN at runtime: a --trace lift bakes the calls into the
 * generated C forever, so a trace-lifted image shipped in a title build (LBP's
 * job kernel, kept from the WWS verification) would otherwise spew the
 * per-instruction log into every boot. Enable with SPU_TRACE=1 (stderr) or
 * SPU_TRACE_FILE=<path>. */
static long long s_trace_left = -1;     /* -1 uninit, -2 unbounded, 0 off */
/* Set when spu_trace_pc suppressed its line (gates below): the paired
 * spu_trace_rt call for the same instruction must also stay silent. */
static SPU_THREAD_LOCAL int s_trace_suppress;

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    /* SPU_TRACE_AT_WALL=1: hold the trace until the PPU's job-barrier probe
     * arms g_barrier_sync_watch, so the budget covers the post-ticket claim/
     * stage pass instead of boot-time idle polling. */
    { static int s_aw = -1;
      if (s_aw < 0) s_aw = getenv("SPU_TRACE_AT_WALL") ? 1 : 0;
      if (s_aw) { extern uint32_t g_barrier_sync_watch;
                  if (!g_barrier_sync_watch) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_FLOWCTX=1: trace ONLY the run the SPURS_PM_FLOW tracer armed
     * (the stalled queue's spu0 dispatch) -- post-wall idle dispatches of the
     * other 13 jobmanager wids otherwise exhaust the budget in under a second. */
    { static int s_fc = -1;
      if (s_fc < 0) s_fc = getenv("SPU_TRACE_FLOWCTX") ? 1 : 0;
      if (s_fc) { extern void* volatile g_pm_flow_ctx;
                  if (g_pm_flow_ctx != (void*)ctx) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_IMG=<n>: trace only contexts running image n (e.g. 1 = the
     * WWS job binary, to catch the crt corrupting the job's r3/r4 args). */
    { static int64_t s_ti = -2;
      if (s_ti == -2) { const char* e = getenv("SPU_TRACE_IMG");
                        s_ti = e ? atoll(e) : -1; }
      if (s_ti >= 0 && ctx->image_id != (int)s_ti) { s_trace_suppress = 1; return; } }
    s_trace_suppress = 0;
    if (s_trace_left < 0) {
        if (s_trace_left == -1) {
            const char* p = getenv("SPU_TRACE_FILE");
            if ((!p || !*p) && !getenv("SPU_TRACE")) { s_trace_left = 0; return; }
            if (p && *p && !s_trace_fp) spu_trace_init(p);
            const char* m = getenv("SPU_TRACE_MAX");
            s_trace_left = (m && *m) ? atoll(m) : 200000;
            if (s_trace_left == 0) s_trace_left = -2;   /* -2 = unbounded */
        }
    }
    if (!s_trace_fp) s_trace_fp = stderr;
    if (s_trace_left == 0) return;
    if (s_trace_left > 0 && --s_trace_left == 0) {
        fprintf(s_trace_fp, "-- SPU_TRACE_MAX reached; trace stopped --\n");
        fflush(s_trace_fp);
        return;
    }
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (s_trace_suppress) return;          /* paired _pc line was gated out */
    if (s_trace_left == 0) return;         /* tracing off/stopped (see _pc) */
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
