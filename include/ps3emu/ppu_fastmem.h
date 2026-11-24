/* ppu_fastmem.h -- inline fast paths for the lifted-code memory accessors (F-tr-08).
 *
 * The lifter emits one vm_read / vm_write call per guest load/store. Those
 * functions live in runtime/ppu/ppu_loader.cpp and carry a decade of debugging
 * instrumentation behind lazy getenv gates: read watches, write watches, value
 * watches, PT truncation tracking, hotmaps, spin backtraces. Out-of-line and
 * without LTO, every guest access pays a call plus that ladder of checks --
 * on The Simpsons, vm_oob alone sampled at 5.4% of the whole process, and
 * titles that are CPU-bound in lifted code burn most of their budget here.
 *
 * This header gives the lifted translation units an INLINE fast path with the
 * identical external behaviour:
 *
 *   - When a diagnostic is armed (see ppu_fm_arm(), called from ppu_hle_init),
 *     the access is delegated 1:1 to the out-of-line family, so every knob
 *     behaves exactly as before, down to the print order.
 *   - When nothing is armed, the access is: alias-window normalize, one
 *     bounds compare (ppu_vm_size == 0 on Windows, so predicted off), one
 *     null-page compare, the raw-SPU MMIO window compare (read32/write32
 *     only, matching the slow paths), the GCM ref-poll address compare
 *     (read32), the hot-spin detector, then the load/bswap.
 *
 * The hot-spin detector (200000 consecutive same-EA reads -> SwitchToThread)
 * stays inline in the fast path: it is fair-share behavior, not a diagnostic,
 * and ppu_spin_yield() is what keeps a hard-spinning guest thread from
 * starving the render pump. Its fire half is out-of-line (ppu_fm_hotread*).
 * ppu_fm_spin_det turns the detector off; it is set once at boot.
 *
 * The lifted sources cannot be edited, so the per-game ppu_recomp.h redirects
 * the accessor NAMES to these inline bodies after including this header:
 *
 *     #include "ps3emu/ppu_fastmem.h"
 *     #ifndef PPU_FASTMEM_NO_RENAME
 *     #  define vm_read32 ppu_fm_read32
 *     ...
 *     #endif
 *
 * runtime/ppu/ppu_loader.cpp -- which DEFINES the out-of-line family the slow
 * paths delegate to -- defines PPU_FASTMEM_NO_RENAME before including
 * ppu_recomp.h so its definitions keep their real names.
 *
 * C and C++ clean: the lifter also writes C (--single-file, smoke tests).
 */
#ifndef PS3EMU_PPU_FASTMEM_H
#define PS3EMU_PPU_FASTMEM_H

#include <stdint.h>
#include <string.h>
#include "ps3emu/guest_read_sync.h"

/* Byte swaps. MSVC C has no __builtin_bswap*, only intrinsics. */
#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#  define PPU_FM_BSWAP16(x) _byteswap_ushort((unsigned short)(x))
#  define PPU_FM_BSWAP32(x) _byteswap_ulong((unsigned long)(x))
#  define PPU_FM_BSWAP64(x) _byteswap_uint64((unsigned __int64)(x))
#else
#  define PPU_FM_BSWAP16(x) __builtin_bswap16((uint16_t)(x))
#  define PPU_FM_BSWAP32(x) __builtin_bswap32((uint32_t)(x))
#  define PPU_FM_BSWAP64(x) __builtin_bswap64((uint64_t)(x))

#endif
/* Guest memory accesses below go through volatile-qualified pointers: the
 * compiler must never cache a guest word across an iteration (guest spin
 * loops wait on words other threads -- PPU, SPU, RSX -- write; the old
 * extern-call family enforced the re-read by opacity, inlining removes
 * that for free). volatile x86 cost is zero: still one mov. */
#define PPU_FM_LD8(p)    (*(const volatile uint8_t*)(p))
#define PPU_FM_LD16(p)   (*(const volatile uint16_t*)(p))
#define PPU_FM_LD32(p)   (*(const volatile uint32_t*)(p))
#define PPU_FM_LD64(p)   (*(const volatile uint64_t*)(p))
#define PPU_FM_ST8(p,v)  (*(volatile uint8_t*)(p) = (v))
#define PPU_FM_ST16(p,v) (*(volatile uint16_t*)(p) = (v))
#define PPU_FM_ST32(p,v) (*(volatile uint32_t*)(p) = (v))
#define PPU_FM_ST64(p,v) (*(volatile uint64_t*)(p) = (v))

#if defined(__GNUC__) || defined(__clang__)
#  define PPU_FM_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define PPU_FM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define PPU_FM_LIKELY(x)   (x)
#  define PPU_FM_UNLIKELY(x) (x)
#endif

/* Caller return address for the fire helpers. GCC/Clang have the builtin;
 * MSVC has _ReturnAddress(). */
#if defined(_MSC_VER) && !defined(__clang__)
#  define PPU_FM_RA() _ReturnAddress()
#else
#  define PPU_FM_RA() __builtin_return_address(0)
#endif

/* Thread-local qualifier, same rule as PPU_THREAD_LOCAL in ppu_recomp.h (this
 * header is included before that macro is defined there). */
#ifndef PPU_FM_TLS
#  if defined(_MSC_VER)
#    define PPU_FM_TLS __declspec(thread)
#  else
#    define PPU_FM_TLS __thread
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- runtime state (defined in runtime/ppu/ppu_loader.cpp / memory/vm.h) ---- */

extern uint8_t*  vm_base;             /* flat guest window; translation = vm_base + ea */
extern uint32_t  ppu_vm_size;         /* 0 on Windows: whole 32-bit space is backed */
extern uint32_t  ppu_hle_inject_base; /* VM_HLE_INJECT_BASE: GCM control block base */

/* Non-zero once ANY memory diagnostic is armed at boot (ppu_fm_arm) or the
 * runtime kill switch fired. While zero, the fast paths below are exact.
 *
 * VOLATILE, not negotiable: the fast paths inline into lifted guest code.
 * Before them every access was an opaque extern call, which forced the
 * compiler to re-read any state an access consults. Inlined, a lifted loop
 * with no other calls (a wait-for-writer spin, a fill loop) lets the
 * optimizer cache these flags in registers -- and g_spu_coh_armed
 * legitimately flips 0->1 at RUNTIME (first SPU GETLLAR), mid-session,
 * from another thread. A cached zero silently disables the SPU lock-line
 * store protocol for the whole loop. Same class of bug for the guest word
 * itself: `while (read32(x) != v)` with the load hoisted out of the loop
 * never observes the writer (convicted as the t=63s menu stall: HOTREAD
 * frozen on 0x40044414 with a constant value). volatile keeps every
 * observation live; on x86 it costs nothing -- the win was removing the
 * call, not removing the load. */
extern volatile uint32_t ppu_fm_diag;

/* Hot-spin detector switch. 1 by default: the detector runs. ppu_fm_arm sets
 * it to 0 only for PPU_FM_SPINDET=0. volatile: see ppu_fm_diag. */
extern volatile uint32_t ppu_fm_spin_det;

/* Decide at boot (ppu_hle_init) whether any memory diagnostic is armed;
 * sets ppu_fm_diag when one is. PPU_FM=0 is honored as a kill switch.
 * PPU_FM_SPINDET=0 turns the hot-spin detector off.
 * Defined in ppu_loader.cpp. */
void ppu_fm_arm(void);

/* Skip-freeze barrier probe window (debug tool, set by hand at runtime).
 * volatile: flipped while the game runs. */
extern volatile uint32_t g_barrier_sync_watch;

/* SPU coherency: stays 0 until SPU code takes its first line reservation.
 * volatile: flips at RUNTIME mid-session (see above). */
extern volatile int    g_spu_coh_armed;
extern int       spu_coh_is_reserved(uint32_t addr);

/* Reserved-line bitmap of runtime/spu/spu_coherency.h, declared here without
 * its include chain: that header pulls the SPU context into lifted guest
 * units. When the header is in scope, its test is the test. */
#ifndef SPU_COH_BITMAP_BYTES
#define PPU_FM_COH_LINE_SHIFT   7
#define PPU_FM_COH_BITMAP_BYTES (1u << ((32 - PPU_FM_COH_LINE_SHIFT) - 3))
extern unsigned char g_spu_coh_bitmap[PPU_FM_COH_BITMAP_BYTES];

/* Same text as spu_coh_bitmap_test. The caller tests g_spu_coh_armed first. */
static __inline int spu_coh_bitmap_test(uint32_t addr)
{
    uint32_t line = addr >> PPU_FM_COH_LINE_SHIFT;
    return (g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}
#endif

/* LV1 dual-mapping alias window normalize (rare: descriptors carrying
 * base|0x05000000 style VAs). */
extern uint32_t  sysmem_spu_alias_fixup(uint32_t ea);

/* ---- slow paths and fire helpers (runtime/ppu/ppu_loader.cpp) ---- */

extern uint8_t  vm_read8 (uint64_t addr);
extern uint16_t vm_read16(uint64_t addr);
extern uint32_t vm_read32(uint64_t addr);
extern uint64_t vm_read64(uint64_t addr);
extern void     vm_write8 (uint64_t addr, uint8_t  val);
extern void     vm_write16(uint64_t addr, uint16_t val);
extern void     vm_write32(uint64_t addr, uint32_t val);
extern void     vm_write64(uint64_t addr, uint64_t val);

/* GCM_REFPOLL: read-driven RSX fence publication. Runs only on an exact
 * address match (VM_HLE_INJECT_BASE + 0x2008), so the env gate lives
 * out-of-line. */
extern void     ppu_fm_refpoll(void);

/* Hot-spin fire halves (env-gated printing lives here; the yield itself is
 * part of fair-share and runs inside). raw_v is the pre-bswap guest word. */
extern void     ppu_fm_hotread8 (uint32_t ea, void* ra);
extern void     ppu_fm_hotread16(uint32_t ea, void* ra);
extern void     ppu_fm_hotread32(uint32_t ea, uint32_t raw_v, void* ra);
extern void     ppu_fm_hotread64(uint32_t ea, uint64_t raw_v, void* ra);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Raw SPU problem-state window test (static inline in spu_raw.h: two ops, no
 * branch on the common path). Kept as an include so the window constants
 * cannot drift between this fast path and the runtime. */
#include "../../runtime/spu/spu_raw.h"

/* ----------------------------------------------------------------------- */
/* inline fast paths                                                        */
/* ----------------------------------------------------------------------- */

/* ppu_alias_norm, inlined: one range compare; the fixup call is the rare path. */
static __inline uint32_t ppu_fm_alias(uint64_t a)
{
    uint32_t e = (uint32_t)a;
    if (PPU_FM_UNLIKELY(e >= 0x45000000u && e < 0x50000000u))
        return sysmem_spu_alias_fixup(e);
    return e;
}

/* Bounds check: ppu_vm_size == 0 on Windows (whole 32-bit space backed) so
 * this folds to one predicted compare; POSIX keeps the real range check.
 * Callers DELEGATE to the slow path on OOB so the report still prints. */
static __inline int ppu_fm_oob(uint32_t ea, uint32_t n)
{
    if (PPU_FM_LIKELY(ppu_vm_size == 0)) return 0;
    return (uint64_t)ea + n > ppu_vm_size;
}

/* Hot-spin detector bookkeeping, one TLS compare on the steady path. Mirrors
 * the slow paths: 200000 consecutive same-EA accesses through this width
 * fire once (yield + report), then restart the count. It runs only while
 * ppu_fm_spin_det is non-zero. */
#define PPU_FM_SPIN_DET(ea, fire_args)                                     \
    do {                                                                   \
        if (PPU_FM_LIKELY(!ppu_fm_spin_det)) break;                        \
        static PPU_FM_TLS uint32_t fm_last = 0xFFFFFFFFu;                  \
        static PPU_FM_TLS uint32_t fm_n = 0;                               \
        if ((uint32_t)(ea) == fm_last) {                                   \
            if (PPU_FM_UNLIKELY(++fm_n == 200000)) { fire_args; fm_n = 0; }\
        } else { fm_last = (uint32_t)(ea); fm_n = 0; }                     \
    } while (0)

static __inline uint8_t ppu_fm_read8(uint64_t a)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 1))) return vm_read8(a);
    if (PPU_FM_UNLIKELY(ppu_fm_diag))       return vm_read8(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read8(a); /* null-read report */
    vm_cpu_read_sync(ea, 1);
    PPU_FM_SPIN_DET(ea, ppu_fm_hotread8(ea, PPU_FM_RA()));
    return PPU_FM_LD8(vm_base + ea);
}

static __inline uint16_t ppu_fm_read16(uint64_t a)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 2))) return vm_read16(a);
    if (PPU_FM_UNLIKELY(ppu_fm_diag))       return vm_read16(a);
    vm_cpu_read_sync(ea, 2);
    uint16_t v = PPU_FM_LD16(vm_base + ea);
    PPU_FM_SPIN_DET(ea, ppu_fm_hotread16(ea, PPU_FM_RA()));
    return (uint16_t)PPU_FM_BSWAP16(v);
}

static __inline uint32_t ppu_fm_read32(uint64_t a)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 4))) return vm_read32(a);
    if (PPU_FM_UNLIKELY(ppu_fm_diag))       return vm_read32(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read32(a); /* null-read report */
    /* Raw SPU problem state: outbound-mailbox reads POP, so they cannot be
     * served from memory. Delegate the whole access. */
    if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) return vm_read32(a);
    /* GCM_REFPOLL: read-driven fence publication BEFORE the read (default on). */
    if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) ppu_fm_refpoll();
    vm_cpu_read_sync(ea, 4);
    uint32_t v = PPU_FM_LD32(vm_base + ea);
    PPU_FM_SPIN_DET(ea, ppu_fm_hotread32(ea, v, PPU_FM_RA()));
    return PPU_FM_BSWAP32(v);
}

static __inline uint64_t ppu_fm_read64(uint64_t a)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 8))) return vm_read64(a);
    if (PPU_FM_UNLIKELY(ppu_fm_diag))       return vm_read64(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read64(a); /* null-read report */
    vm_cpu_read_sync(ea, 8);
    uint64_t v = PPU_FM_LD64(vm_base + ea);
    PPU_FM_SPIN_DET(ea, ppu_fm_hotread64(ea, v, PPU_FM_RA()));
    return PPU_FM_BSWAP64(v);
}

static __inline void ppu_fm_write8(uint64_t a, uint8_t v)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 1))) { vm_write8(a, v); return; }
    if (PPU_FM_UNLIKELY(ppu_fm_diag || g_barrier_sync_watch)) { vm_write8(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write8(a, v); return; } /* null-store report/drop */
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write8(a, v); return;    /* locked-line path */
    }
    PPU_FM_ST8(vm_base + ea, v);
}

static __inline void ppu_fm_write16(uint64_t a, uint16_t v)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 2))) { vm_write16(a, v); return; }
    if (PPU_FM_UNLIKELY(ppu_fm_diag || g_barrier_sync_watch)) { vm_write16(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write16(a, v); return; }
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write16(a, v); return;
    }
    uint16_t b = (uint16_t)PPU_FM_BSWAP16(v);
    PPU_FM_ST16(vm_base + ea, b);
}

static __inline void ppu_fm_write32(uint64_t a, uint32_t v)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 4))) { vm_write32(a, v); return; }
    if (PPU_FM_UNLIKELY(ppu_fm_diag || g_barrier_sync_watch)) { vm_write32(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write32(a, v); return; }
    /* Raw SPU problem state: run control / mailboxes / signal notify have
     * store side effects. Delegate so the plain store + side effect keep the
     * slow path's exact ordering. */
    if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) { vm_write32(a, v); return; }
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write32(a, v); return;
    }
    uint32_t b = PPU_FM_BSWAP32(v);
    PPU_FM_ST32(vm_base + ea, b);
}

static __inline void ppu_fm_write64(uint64_t a, uint64_t v)
{
    uint32_t ea = ppu_fm_alias(a);
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 8))) { vm_write64(a, v); return; }
    if (PPU_FM_UNLIKELY(ppu_fm_diag || g_barrier_sync_watch)) { vm_write64(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write64(a, v); return; }
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write64(a, v); return;
    }
    uint64_t b = PPU_FM_BSWAP64(v);
    PPU_FM_ST64(vm_base + ea, b);
}

#endif /* PS3EMU_PPU_FASTMEM_H */
