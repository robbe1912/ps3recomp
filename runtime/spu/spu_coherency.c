/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */

unsigned char g_spu_coh_bitmap[SPU_COH_BITMAP_BYTES];

/* Zero until the first GETLLAR anywhere. Keeps the PPU store fast path off
 * the bitmap entirely for a title that runs no SPU code. Exported as
 * g_spu_coh_armed (see spu_coherency.h): the inline store paths gate on it
 * with one predicted compare instead of a call into this file. Written once,
 * under the lock; read unlocked, where a stale zero costs at most one missed
 * event on the very first reservation of the run. */
volatile int g_spu_coh_armed;

/* The SPU contexts that have reserved a line. Sized to match the MFC engine
 * registry in spu_channels.c, which is the ceiling on live SPU contexts. */
#define SPU_COH_MAX_CTX 8
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];
typedef struct coh_overflow {
    spu_context* ctx;
    struct coh_overflow* next;
} coh_overflow;
static coh_overflow* s_coh_overflow;

unsigned long g_spu_lr_raise = 0;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    g_spu_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    g_spu_coh_armed = 1;

    if (!ctx) return;
    /* Retained virtual contexts outnumber physical SPUs. Find an existing
     * owner before reusing a hole; duplicate registration would outlive a
     * single unregister. All registry access holds the lock-line lock. */
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) return;
    for (coh_overflow* n = s_coh_overflow; n; n = n->next)
        if (n->ctx == ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_coh_ctxs[i] == NULL) { s_coh_ctxs[i] = ctx; return; }
    }
    coh_overflow* n = (coh_overflow*)malloc(sizeof(*n));
    if (!n) {
        fprintf(stderr, "[spu-coh] cannot register reservation owner %p\n", (void*)ctx);
        fflush(stderr);
        abort();
    }
    n->ctx = ctx;
    n->next = s_coh_overflow;
    s_coh_overflow = n;
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) { s_coh_ctxs[i] = NULL; break; }
    for (coh_overflow** link = &s_coh_overflow; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        coh_overflow* n = *link;
        *link = n->next;
        free(n);
        break;
    }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (s_coh_ctxs[i] && c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = NULL;
        }
    }
    for (coh_overflow** link = &s_coh_overflow; *link;) {
        coh_overflow* n = *link;
        uintptr_t c = (uintptr_t)n->ctx;
        if (c < lo || c >= hi) { link = &n->next; continue; }
        fprintf(stderr, "[spu-coh] leaked overflow ctx %p (img=%d) dropped at thread exit\n",
                (void*)c, n->ctx->image_id);
        *link = n->next;
        free(n);
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!g_spu_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return spu_coh_bitmap_test(addr);
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
static void coh_notify_overflow(uint32_t line, const void* self)
{
    for (coh_overflow* n = s_coh_overflow; n; n = n->next) {
        spu_context* c = n->ctx;
        if ((const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
    coh_notify_overflow(line, self);
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
    /* Same notification semantics for every retained virtual context. */
    coh_notify_overflow(line, NULL);
}

#ifdef __cplusplus
}
#endif
