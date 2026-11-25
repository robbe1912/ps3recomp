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
 * This header gives the lifted translation units a fast path with the
 * identical external behaviour. Each accessor has three parts:
 *
 *   - The gate (ppu_fm_readN, ppu_fm_writeN): one load of the flag word
 *     ppu_fm_slow and one load of one byte of the page table ppu_fm_kind.
 *     PPU_FM_INLINE_FAST 1, the default, puts the gate and the fast body at
 *     each call site. PPU_FM_INLINE_FAST 0 keeps one out-of-line copy of
 *     each accessor per unit.
 *   - The out-of-line function (ppu_fm_readN_out, ppu_fm_writeN_out): one
 *     copy per unit. A set flag bit or a flagged page sends the access
 *     there. A set bit sends the whole access to the out-of-line family.
 *     A flagged page gets the exact tests, in the order of the first diet:
 *     alias window, bounds, null page, raw-SPU MMIO window (read32 and
 *     write32), GCM ref-poll address (read32). Each test that is true sends
 *     the whole access to the family. Then comes the fast body.
 *   - The fast body (ppu_fm_readN_body, ppu_fm_writeN_body), always inline:
 *     the read sync, the load or the store with its byte swap, and for the
 *     reads the hot-spin detector. The stores test the reserved-line bitmap.
 *
 * A diagnostic that is armed sends every access to the out-of-line family
 * (vm_readN, vm_writeN), so every knob behaves exactly as before, down to
 * the print order.
 *
 * ppu_fm_arm() (runtime/ppu/ppu_loader.cpp, called from ppu_hle_init) sets
 * the flag word and fills the page table. Before it runs, each bit of the
 * flag word is set, so each access takes the out-of-line function.
 *
 * PPU_FM3 (1 by default): the read accessors hold no label address, so
 * Clang inlines them into the lifted code. PPU_FM3 0 is the layout before
 * this step. See the comment at the read accessors.
 *
 * The hot-spin detector (200000 consecutive same-EA reads -> SwitchToThread)
 * stays in the fast body: it is fair-share behavior, not a diagnostic, and
 * ppu_spin_yield() is what keeps a hard-spinning guest thread from starving
 * the render pump. Its fire half is out-of-line (ppu_fm_hotread*).
 * ppu_fm_spin_det turns the detector off; it is set once at boot.
 *
 * PPU_FM_COUNT (opt-in, off by default) counts the calls of each accessor per
 * second and per host thread. With it every access takes the out-of-line
 * family, and the out-of-line function counts the call. The fast path has
 * no code of the counter.
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

/* Thread-local qualifier, same rule as PPU_THREAD_LOCAL in ppu_recomp.h (this
 * header is included before that macro is defined there). */
#ifndef PPU_FM_TLS
#  if defined(_MSC_VER)
#    define PPU_FM_TLS __declspec(thread)
#  else
#    define PPU_FM_TLS __thread
#  endif
#endif

/* A static inline function of this header that must stay out of line in the
 * lifted code: one copy per unit, no code at the call sites. */
#if defined(__GNUC__) || defined(__clang__)
#  define PPU_FM_NOINLINE __attribute__((noinline))
#  define PPU_FM_ALWAYS_INLINE __attribute__((always_inline))
#else
#  define PPU_FM_NOINLINE __declspec(noinline)
#  define PPU_FM_ALWAYS_INLINE __forceinline
#endif

/* Where the gate and the fast body of an accessor run. 1, the default:
 * inline at each call site of the lifted code. 0: in one out-of-line copy
 * of each accessor per unit; each call site is a call. */
#ifndef PPU_FM_INLINE_FAST
#  define PPU_FM_INLINE_FAST 1
#endif
#if PPU_FM_INLINE_FAST
#  define PPU_FM_FAST PPU_FM_ALWAYS_INLINE
#else
#  define PPU_FM_FAST PPU_FM_NOINLINE
#endif

/* PPU_FM3 selects the layout of the read accessors. 1, the default: the
 * read accessors hold no label address, so Clang inlines them into the
 * lifted code as it inlines the write accessors. 0: the layout before
 * this step, with the label address. */
#ifndef PPU_FM3
#  define PPU_FM3 1
#endif

/* PPU_FM4 selects the lean gate. 1, the default: the call site holds one
 * test of the flag word and of the page byte, then the access. The exact
 * tests that the gate of PPU_FM3 made inline are decisions of the page
 * byte: the bounds, the GCM ref-poll address and the read sync. 0: the
 * layout of PPU_FM3. The lean gate needs PPU_FM3, so PPU_FM3 0 forces it
 * off. See the comment at the lean gate. */
#ifndef PPU_FM4
#  define PPU_FM4 1
#endif
#if !PPU_FM3
#  undef PPU_FM4
#  define PPU_FM4 0
#endif

/* PPU_FM3: the caller address that a read accessor of that layout passes
 * to the body. Inline layout: null, so the body takes the fire function
 * (ppu_fm_fireN), which reads its own return address: the call site in
 * the lifted function. One copy per unit (PPU_FM_INLINE_FAST 0): the
 * return address of the accessor. That is the call site in the lifted
 * function too. The report gets the same guest function in each layout. */
#if PPU_FM_INLINE_FAST
#  define PPU_FM_RA3() ((void*)0)
#else
#  define PPU_FM_RA3() PPU_FM_RA_OUT()
#endif

/* Caller address for the fire helpers.
 * PPU_FM_RA_OUT() is the return address of the out-of-line function. In
 * each layout it is the call site in the guest function.
 * PPU_FM_RA() belongs to a read that the gate passes in the accessor. With
 * PPU_FM_INLINE_FAST 1 the accessor is inline at the call site, so its
 * return address is that of the guest function. The address of the label
 * PPU_FM_SITE at the site stands in; it lies in the same guest function.
 * GCC and Clang have label addresses and the builtin. MSVC has
 * _ReturnAddress() only. */
#if defined(__GNUC__) || defined(__clang__)
#  define PPU_FM_RA_OUT() __builtin_return_address(0)
#  if PPU_FM_INLINE_FAST
#    define PPU_FM_RA() (void*)&&ppu_fm_site_
#    define PPU_FM_SITE ppu_fm_site_:
#  else
#    define PPU_FM_RA() __builtin_return_address(0)
#    define PPU_FM_SITE
#  endif
#else
#  define PPU_FM_RA_OUT() _ReturnAddress()
#  define PPU_FM_RA()    _ReturnAddress()
#  define PPU_FM_SITE
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- runtime state (defined in runtime/ppu/ppu_loader.cpp / memory/vm.h) ---- */

extern uint8_t*  vm_base;             /* flat guest window; translation = vm_base + ea */
extern uint32_t  ppu_vm_size;         /* 0 on Windows: whole 32-bit space is backed */
extern uint32_t  ppu_hle_inject_base; /* VM_HLE_INJECT_BASE: GCM control block base */

/* Flag word: one bit for each reason to leave the fast path. ppu_fm_arm()
 * sets it at boot. Each bit of the initial value is set, so each access
 * takes the out-of-line function until then.
 *
 *   DIAG   a memory diagnostic is armed, or PPU_FM=0. Reads and writes go
 *          to the out-of-line family.
 *   WATCH  SPU_ATOM_EA is set. Stores go to the family, as before, where
 *          g_barrier_sync_watch is in scope. Reads keep the fast path, as
 *          before.
 *   COUNT  PPU_FM_COUNT is on. Reads and writes go to the family, and the
 *          out-of-line function counts the call.
 *
 * VOLATILE, not negotiable: the gate inlines into lifted guest code. Before
 * it every access was an opaque extern call, which forced the compiler to
 * re-read any state an access consults. Inlined, a lifted loop with no
 * other calls (a wait-for-writer spin, a fill loop) lets the optimizer
 * cache these flags in registers -- and g_spu_coh_armed legitimately flips
 * 0->1 at RUNTIME (first SPU GETLLAR), mid-session, from another thread. A
 * cached zero silently disables the SPU lock-line store protocol for the
 * whole loop. Same class of bug for the guest word itself: `while
 * (read32(x) != v)` with the load hoisted out of the loop never observes
 * the writer (convicted as the t=63s menu stall: HOTREAD frozen on
 * 0x40044414 with a constant value). volatile keeps every observation live;
 * on x86 it costs nothing -- the win was removing the call, not removing
 * the load. */
#define PPU_FM_S_DIAG  1u
#define PPU_FM_S_WATCH 2u
#define PPU_FM_S_COUNT 4u
#define PPU_FM_S_READ  (PPU_FM_S_DIAG | PPU_FM_S_COUNT)
#define PPU_FM_S_WRITE (PPU_FM_S_DIAG | PPU_FM_S_WATCH | PPU_FM_S_COUNT)
#define PPU_FM_S_ALL  (PPU_FM_S_DIAG | PPU_FM_S_WATCH | PPU_FM_S_COUNT)
extern volatile uint32_t ppu_fm_slow;

/* Page table: one byte per 4 KB guest page, zero-initialized. ppu_fm_arm()
 * flags the pages that hold an address with a slow-path reason: the null
 * pages, the raw-SPU MMIO window, the GCM ref-poll page, and each page
 * that is not entirely inside ppu_vm_size. The table only selects the
 * route. The out-of-line function makes the exact tests, so a flagged
 * page that holds a fast address gets the fast body there.
 *
 * PPU_FM4 1 has two writers at run time. The read sync (K_SYNC) sets its
 * bits with one atomic operation after it wrote its own state. The alias
 * table (runtime/syscalls/spu_ea_alias.c) sets the bits of a page of its
 * window when the page answer of that page changes; it makes each access
 * of the page go out of line until the new answer is stored
 * (ppu_fm_window_begin, ppu_fm_window_end in runtime/ppu/ppu_loader.cpp).
 * The lean gate reads the byte with an acquire load at each access
 * (ppu_fm_page). The paths before PPU_FM4 read the byte with a plain
 * load. They have no writer at run time, and ppu_fm_arm() writes the
 * table before guest code runs. */
#define PPU_FM_PAGE_SHIFT 12
#define PPU_FM_PAGE_SIZE  (1u << PPU_FM_PAGE_SHIFT)
#define PPU_FM_PAGES      (1u << (32 - PPU_FM_PAGE_SHIFT))
/* Route bits of a page byte: a set bit sends each access of that kind to
 * the out-of-line function. The values let the lean gate fold its two
 * masks into one AND (PPU_FM_LEAN): no route bit shares a position with
 * a flag bit of the other kind, or with an owner bit. */
#define PPU_FM_K_READ  0x08u
#define PPU_FM_K_WRITE 0x10u
#if PPU_FM4
/* Owner bits of the lean gate. They name the writer that set the route
 * bits. The gate tests only the route bits. */
#define PPU_FM_K_SYNC  0x20u   /* a page of a marked range of the read sync */
/* Not a route bit. A page of the alias window whose page answer is ALIAS.
 * The call site subtracts PPU_FM_ALIAS_DELTA, tests the page byte of the
 * primary address, and runs the fast body there. The lean gate folds the
 * bit into the mask of its one test, so a plain page has no instruction
 * for it. Valid when no route bit is set: a set route bit sends the
 * access out of line, which has the exact answer. */
#define PPU_FM_K_ALIAS 0x40u
#endif
extern unsigned char ppu_fm_kind[PPU_FM_PAGES];

/* Hot-spin detector switch. 1 by default: the detector runs. ppu_fm_arm sets
 * it to 0 only for PPU_FM_SPINDET=0. volatile: see ppu_fm_slow. */
extern volatile uint32_t ppu_fm_spin_det;

/* Decides at boot (ppu_hle_init) which paths are slow: sets ppu_fm_slow and
 * fills ppu_fm_kind. Call it again after a change of ppu_vm_size or
 * ppu_hle_inject_base. PPU_FM=0 is honored as a kill switch.
 * PPU_FM_SPINDET=0 turns the hot-spin detector off.
 * Defined in ppu_loader.cpp. */
void ppu_fm_arm(void);

/* PPU_FM_COUNT: opt-in call counter of the accessors. ppu_fm_arm sets the
 * switch. The out-of-line function of each accessor then counts the call
 * and gives it to the out-of-line family. The lines go to stderr; see
 * ppu_loader.cpp. */
#define PPU_FM_C_READ8   0
#define PPU_FM_C_READ16  1
#define PPU_FM_C_READ32  2
#define PPU_FM_C_READ64  3
#define PPU_FM_C_WRITE8  4
#define PPU_FM_C_WRITE16 5
#define PPU_FM_C_WRITE32 6
#define PPU_FM_C_WRITE64 7
#define PPU_FM_C_COUNT   8   /* number of columns, then the total */
extern void ppu_fm_count_hit(unsigned accessor);

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
#if PPU_FM3
/* PPU_FM3: the page answer of each 4 KB page of the alias window, for the
 * gate of the accessors. sysmem_spu_alias_fixup fills it
 * (runtime/syscalls/spu_ea_alias.c). A byte is PPU_FM_ALIAS_IDENTITY,
 * PPU_FM_ALIAS_ALIAS or PPU_FM_ALIAS_MIXED. The two trees must agree on
 * the number of pages. */
#define PPU_FM_ALIAS_LO       0x45000000u
#define PPU_FM_ALIAS_HI       0x50000000u
#define PPU_FM_ALIAS_DELTA    0x05000000u
#define PPU_FM_ALIAS_PAGES    ((PPU_FM_ALIAS_HI - PPU_FM_ALIAS_LO) >> PPU_FM_PAGE_SHIFT)
#define PPU_FM_ALIAS_IDENTITY 0u
#define PPU_FM_ALIAS_ALIAS    1u
#define PPU_FM_ALIAS_MIXED    2u
extern volatile unsigned char sysmem_spu_alias_page[PPU_FM_ALIAS_PAGES];
#endif

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
/* helpers                                                                  */
/* ----------------------------------------------------------------------- */

/* The LV1 dual-mapping alias window. ppu_fm_arm flags its pages; the
 * out-of-line function makes the test and calls the fixup. */
static __inline int ppu_fm_in_alias(uint32_t e)
{
    return e >= 0x45000000u && e < 0x50000000u;
}

static __inline uint32_t ppu_fm_alias(uint32_t e)
{
    return PPU_FM_UNLIKELY(ppu_fm_in_alias(e)) ? sysmem_spu_alias_fixup(e) : e;
}

/* Bounds check: ppu_vm_size == 0 on Windows (whole 32-bit space backed) so
 * this folds to one predicted compare; POSIX keeps the real range check.
 * Callers DELEGATE to the slow path on OOB so the report still prints. */
static __inline int ppu_fm_oob(uint32_t ea, uint32_t n)
{
    if (PPU_FM_LIKELY(ppu_vm_size == 0)) return 0;
    return (uint64_t)ea + n > ppu_vm_size;
}

/* The GCM ref-poll address, on a page boundary for the page table. */
static __inline int ppu_fm_is_refpoll_page(uint32_t e)
{
    return e == ((ppu_hle_inject_base + 0x2008u) & ~(PPU_FM_PAGE_SIZE - 1u));
}

/* The gate of each accessor. True sends the access to the out-of-line
 * function. The flag bits that apply to the kind, and the page byte. */
#define PPU_FM_SLOW(ea, kind, mask)                                        \
    ((ppu_fm_slow & (mask)) | (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind)))

#if PPU_FM3
/* PPU_FM3: the gate of an accessor on the flag word that it has read. Same
 * two terms as PPU_FM_SLOW. */
#define PPU_FM_GATE(flags, ea, kind, mask)                                   \
    (((flags) & (mask)) | (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind)))

/* The page answer of one page of the window. Acquire, as the load of the
 * fixup. */
static __inline unsigned char ppu_fm_alias_state(uint32_t ea)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return (unsigned char)_InterlockedCompareExchange8(
        (volatile char*)&sysmem_spu_alias_page[(ea - PPU_FM_ALIAS_LO) >> PPU_FM_PAGE_SHIFT], 0, 0);
#else
    return __atomic_load_n(&sysmem_spu_alias_page[(ea - PPU_FM_ALIAS_LO) >> PPU_FM_PAGE_SHIFT],
                           __ATOMIC_ACQUIRE);
#endif
}

/* Proofs that the inline answer needs none of the tests of the out-of-line
 * function: the window and its primary image (EA - 0x05000000) hold no null
 * address and no raw SPU problem-state register. */
typedef char ppu_fm_alias_no_null[(PPU_FM_ALIAS_LO - PPU_FM_ALIAS_DELTA >= 0x10000u) ? 1 : -1];
typedef char ppu_fm_alias_no_raw[(SPU_RAW_BASE >= PPU_FM_ALIAS_HI) ? 1 : -1];

/* PPU_FM3: the alias answer of the gate. The access is on a page that the
 * page table flags for the kind. Returns 1 and sets *ea to the address of
 * the body when the access stays inline: IDENTITY keeps the EA, ALIAS gives
 * the primary (EA - 0x05000000). Returns 0 when a flag bit of mask is set,
 * when the page is outside the window, when the page is MIXED and when the
 * access is out of bounds: the out-of-line function then makes the tests
 * and the reports. n is the width of the access. */
static __inline int ppu_fm_window(uint32_t flags, uint32_t mask, uint32_t n, uint32_t* ea)
{
    if (flags & mask) return 0;
    uint32_t off = *ea - PPU_FM_ALIAS_LO;
    if (PPU_FM_UNLIKELY(off >= PPU_FM_ALIAS_HI - PPU_FM_ALIAS_LO)) return 0;
    unsigned char state = ppu_fm_alias_state(*ea);
    uint32_t e = state == PPU_FM_ALIAS_ALIAS ? *ea - PPU_FM_ALIAS_DELTA : *ea;
    if (state == PPU_FM_ALIAS_MIXED) return 0;
    if (PPU_FM_UNLIKELY(ppu_fm_oob(e, n))) return 0;
    *ea = e;
    return 1;
}
#endif

#if PPU_FM4
/* Sets bits of the page byte of one page, as one atomic operation. The
 * reader sees the byte with or without the bits, never a part of them.
 * The release order keeps the state of the writer (its marks, its bitmap
 * bit, its page answer) before the byte. A plain load comes first: the
 * usual case is that an earlier writer set the bits. */
static __inline void ppu_fm_page_or(uint32_t page, unsigned char bits)
{
    if ((ppu_fm_kind[page] & bits) == bits) return;
#if defined(_MSC_VER) && !defined(__clang__)
    _InterlockedOr8((volatile char*)&ppu_fm_kind[page], (char)bits);
#else
    __atomic_fetch_or(&ppu_fm_kind[page], bits, __ATOMIC_RELEASE);
#endif
}

/* The page byte of the lean gate: a volatile acquire load. Volatile: the
 * compiler keeps one load at each access, in program order. It cannot
 * cache the byte in a register across the iterations of a loop, and it
 * cannot move a load or a store of the access above the load of the byte.
 * Acquire: on a host that reorders loads, the access that follows sees the
 * state that the writer put before its release store. On x86 the load is
 * one plain mov. */
static __inline unsigned char ppu_fm_page(uint32_t index)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return (unsigned char)_InterlockedCompareExchange8((volatile char*)&ppu_fm_kind[index], 0, 0);
#else
    return __atomic_load_n((const volatile unsigned char*)&ppu_fm_kind[index], __ATOMIC_ACQUIRE);
#endif
}

/* PPU_FM4: the lean gate of an accessor. `k` is the page byte of the EA,
 * from ppu_fm_page. One test: the page byte and the flag word, each
 * reduced to the bits of the kind by one mask. The byte holds the answer
 * of each exact test that the gate of PPU_FM3 made inline. The proofs
 * below state that the mask keeps only the route bit of the kind from the
 * byte, and only the flag bits of the mask from the word. */
#define PPU_FM_LEAN(k, kind, mask)                                              \
    ((unsigned char)((k) | (unsigned char)ppu_fm_slow) & (unsigned char)((kind) | (mask)))
typedef char ppu_fm_lean_read_mask[((PPU_FM_K_READ | PPU_FM_S_READ) & (PPU_FM_K_WRITE | PPU_FM_K_SYNC | PPU_FM_K_ALIAS)) == 0 ? 1 : -1];
typedef char ppu_fm_lean_write_mask[((PPU_FM_K_WRITE | PPU_FM_S_WRITE) & (PPU_FM_K_READ | PPU_FM_K_SYNC | PPU_FM_K_ALIAS)) == 0 ? 1 : -1];
typedef char ppu_fm_lean_alias_bit[(PPU_FM_K_ALIAS & (PPU_FM_K_READ | PPU_FM_K_WRITE | PPU_FM_K_SYNC | PPU_FM_S_ALL)) == 0 ? 1 : -1];
/* PPU_FM4: the alias answer of the call site. The site comes here with the
 * page byte of the EA, a set K_ALIAS, and no route bit and no flag bit
 * for the kind: the lean gate has made that test. The access runs on the
 * primary address, EA - 0x05000000, as the window of PPU_FM3 answers it.
 * The page byte of the primary address decides the route of the access:
 * a set route bit or flag bit for the kind returns 0, and the site sends
 * the original address to the out-of-line function. That function makes
 * the exact tests. The test ignores K_ALIAS of the primary page: the
 * answer of the window is one step, as in sysmem_spu_alias_fixup. */
static __inline int ppu_fm_alias_ea(uint32_t kind, uint32_t mask, uint32_t* ea)
{
    uint32_t e = *ea - PPU_FM_ALIAS_DELTA;
    if (PPU_FM_LEAN(ppu_fm_page(e >> PPU_FM_PAGE_SHIFT), kind, mask)) return 0;
    *ea = e;
    return 1;
}
#endif

#if PPU_FM3
/* PPU_FM3: the fire half of a read that the gate passes inline. Out of line,
 * so the inline layout holds no label address. The caller address is the
 * call site in the lifted function. It lies in the guest function that the
 * label PPU_FM_SITE of the other layout names, so the report is the same. */
static __inline PPU_FM_NOINLINE void ppu_fm_fire8(uint32_t ea)
{
    ppu_fm_hotread8(ea, PPU_FM_RA_OUT());
}

static __inline PPU_FM_NOINLINE void ppu_fm_fire16(uint32_t ea)
{
    ppu_fm_hotread16(ea, PPU_FM_RA_OUT());
}

static __inline PPU_FM_NOINLINE void ppu_fm_fire32(uint32_t ea, uint32_t v)
{
    ppu_fm_hotread32(ea, v, PPU_FM_RA_OUT());
}

static __inline PPU_FM_NOINLINE void ppu_fm_fire64(uint32_t ea, uint64_t v)
{
    ppu_fm_hotread64(ea, v, PPU_FM_RA_OUT());
}

/* The fire statement of a read body. A null caller address is a read of
 * the inline layout (PPU_FM3): it takes the fire function above. */
#  define PPU_FM_FIRE(ra, inline_fire, out_fire) \
    do { if (ra) { out_fire; } else { inline_fire; } } while (0)
#else
#  define PPU_FM_FIRE(ra, inline_fire, out_fire) out_fire
#endif

/* Hot-spin detector bookkeeping, one TLS compare on the steady path. Mirrors
 * the slow paths: 200000 consecutive same-EA accesses through this width
 * fire once (yield + report), then restart the count. It runs only while
 * ppu_fm_spin_det is non-zero. The state is one TLS object per read width,
 * so the compiler computes its address one time. */
struct PPU_FM_SPIN {
    uint32_t last;
    uint32_t n;
};
#define PPU_FM_SPIN_DET(state, ea, fire_args)                               \
    do {                                                                   \
        struct PPU_FM_SPIN* fm_ = (state);                                 \
        if (PPU_FM_LIKELY(!ppu_fm_spin_det)) break;                        \
        if ((uint32_t)(ea) == fm_->last) {                                 \
            if (PPU_FM_UNLIKELY(++fm_->n == 200000)) { fire_args; fm_->n = 0; } \
        } else { fm_->last = (uint32_t)(ea); fm_->n = 0; }                 \
    } while (0)

/* ----------------------------------------------------------------------- */
/* fast bodies: always inline                                               */
/* ----------------------------------------------------------------------- */

static __inline PPU_FM_ALWAYS_INLINE uint8_t ppu_fm_read8_body(uint32_t ea, void* ra)
{
    vm_cpu_read_sync(ea, 1);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(ra, ppu_fm_fire8(ea), ppu_fm_hotread8(ea, ra)));
    return PPU_FM_LD8(vm_base + ea);
}

static __inline PPU_FM_ALWAYS_INLINE uint16_t ppu_fm_read16_body(uint32_t ea, void* ra)
{
    vm_cpu_read_sync(ea, 2);
    uint16_t v = PPU_FM_LD16(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(ra, ppu_fm_fire16(ea), ppu_fm_hotread16(ea, ra)));
    return (uint16_t)PPU_FM_BSWAP16(v);
}

static __inline PPU_FM_ALWAYS_INLINE uint32_t ppu_fm_read32_body(uint32_t ea, void* ra)
{
    vm_cpu_read_sync(ea, 4);
    uint32_t v = PPU_FM_LD32(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(ra, ppu_fm_fire32(ea, v), ppu_fm_hotread32(ea, v, ra)));
    return PPU_FM_BSWAP32(v);
}

static __inline PPU_FM_ALWAYS_INLINE uint64_t ppu_fm_read64_body(uint32_t ea, void* ra)
{
    vm_cpu_read_sync(ea, 8);
    uint64_t v = PPU_FM_LD64(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(ra, ppu_fm_fire64(ea, v), ppu_fm_hotread64(ea, v, ra)));
    return PPU_FM_BSWAP64(v);
}

#if PPU_FM4
/* PPU_FM4: the fast body of a read at the call site. The gate sends each
 * access that needs an exact test to the out-of-line function. So the body
 * has the load and the detector only. */
static __inline PPU_FM_ALWAYS_INLINE uint8_t ppu_fm_read8_lean(uint32_t ea)
{
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(PPU_FM_RA3(), ppu_fm_fire8(ea), ppu_fm_hotread8(ea, PPU_FM_RA3())));
    return PPU_FM_LD8(vm_base + ea);
}

static __inline PPU_FM_ALWAYS_INLINE uint16_t ppu_fm_read16_lean(uint32_t ea)
{
    uint16_t v = PPU_FM_LD16(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(PPU_FM_RA3(), ppu_fm_fire16(ea), ppu_fm_hotread16(ea, PPU_FM_RA3())));
    return (uint16_t)PPU_FM_BSWAP16(v);
}

static __inline PPU_FM_ALWAYS_INLINE uint32_t ppu_fm_read32_lean(uint32_t ea)
{
    uint32_t v = PPU_FM_LD32(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(PPU_FM_RA3(), ppu_fm_fire32(ea, v), ppu_fm_hotread32(ea, v, PPU_FM_RA3())));
    return PPU_FM_BSWAP32(v);
}

static __inline PPU_FM_ALWAYS_INLINE uint64_t ppu_fm_read64_lean(uint32_t ea)
{
    uint64_t v = PPU_FM_LD64(vm_base + ea);
    static PPU_FM_TLS struct PPU_FM_SPIN fm = { 0xFFFFFFFFu, 0 };
    PPU_FM_SPIN_DET(&fm, ea, PPU_FM_FIRE(PPU_FM_RA3(), ppu_fm_fire64(ea, v), ppu_fm_hotread64(ea, v, PPU_FM_RA3())));
    return PPU_FM_BSWAP64(v);
}
#endif

/* The fast body of a read in the out-of-line function. PPU_FM4: the read
 * sync is a test of that function, then the body of the call site follows.
 * So the detector rule is the same at each place. Before PPU_FM4: the body
 * with the read sync and the detector of those paths. */
#if PPU_FM4
#define PPU_FM_OUT_READ(name, n, ea) (vm_cpu_read_sync((ea), (n)), name##_lean(ea))
#else
#define PPU_FM_OUT_READ(name, n, ea) name##_body(ea, PPU_FM_RA_OUT())
#endif

/* The store bodies take the fixed address and the original address: a
 * reserved line goes to the family with the original address. */
static __inline PPU_FM_ALWAYS_INLINE void ppu_fm_write8_body(uint64_t a, uint32_t ea, uint8_t v)
{
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write8(a, v); return;    /* locked-line path */
    }
    PPU_FM_ST8(vm_base + ea, v);
}

static __inline PPU_FM_ALWAYS_INLINE void ppu_fm_write16_body(uint64_t a, uint32_t ea, uint16_t v)
{
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write16(a, v); return;
    }
    uint16_t b = (uint16_t)PPU_FM_BSWAP16(v);
    PPU_FM_ST16(vm_base + ea, b);
}

static __inline PPU_FM_ALWAYS_INLINE void ppu_fm_write32_body(uint64_t a, uint32_t ea, uint32_t v)
{
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write32(a, v); return;
    }
    uint32_t b = PPU_FM_BSWAP32(v);
    PPU_FM_ST32(vm_base + ea, b);
}

static __inline PPU_FM_ALWAYS_INLINE void ppu_fm_write64_body(uint64_t a, uint32_t ea, uint64_t v)
{
    if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {
        vm_write64(a, v); return;
    }
    uint64_t b = PPU_FM_BSWAP64(v);
    PPU_FM_ST64(vm_base + ea, b);
}

/* ----------------------------------------------------------------------- */
/* out-of-line functions: one copy per unit                                 */
/* ----------------------------------------------------------------------- */

static __inline PPU_FM_NOINLINE uint8_t ppu_fm_read8_out(uint64_t a)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_READ)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_READ8);
        return vm_read8(a);
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 1))) return vm_read8(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read8(a); /* null-read report */
    return PPU_FM_OUT_READ(ppu_fm_read8, 1, ea);
}

static __inline PPU_FM_NOINLINE uint16_t ppu_fm_read16_out(uint64_t a)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_READ)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_READ16);
        return vm_read16(a);
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 2))) return vm_read16(a);
    return PPU_FM_OUT_READ(ppu_fm_read16, 2, ea);
}

static __inline PPU_FM_NOINLINE uint32_t ppu_fm_read32_out(uint64_t a)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_READ)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_READ32);
        return vm_read32(a);
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 4))) return vm_read32(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read32(a); /* null-read report */
    /* Raw SPU problem state: outbound-mailbox reads POP, so they cannot be
     * served from memory. Delegate the whole access. */
    if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) return vm_read32(a);
    /* GCM_REFPOLL: read-driven fence publication BEFORE the read (default on). */
    if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) ppu_fm_refpoll();
    return PPU_FM_OUT_READ(ppu_fm_read32, 4, ea);
}

static __inline PPU_FM_NOINLINE uint64_t ppu_fm_read64_out(uint64_t a)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_READ)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_READ64);
        return vm_read64(a);
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 8))) return vm_read64(a);
    if (PPU_FM_UNLIKELY(ea < 0x10000u))     return vm_read64(a); /* null-read report */
    return PPU_FM_OUT_READ(ppu_fm_read64, 8, ea);
}

static __inline PPU_FM_NOINLINE void ppu_fm_write8_out(uint64_t a, uint8_t v)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_WRITE)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_WRITE8);
        vm_write8(a, v); return;
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 1))) { vm_write8(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write8(a, v); return; } /* null-store report/drop */
    ppu_fm_write8_body(a, ea, v);
}

static __inline PPU_FM_NOINLINE void ppu_fm_write16_out(uint64_t a, uint16_t v)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_WRITE)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_WRITE16);
        vm_write16(a, v); return;
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 2))) { vm_write16(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write16(a, v); return; }
    ppu_fm_write16_body(a, ea, v);
}

static __inline PPU_FM_NOINLINE void ppu_fm_write32_out(uint64_t a, uint32_t v)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_WRITE)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_WRITE32);
        vm_write32(a, v); return;
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 4))) { vm_write32(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write32(a, v); return; }
    /* Raw SPU problem state: run control / mailboxes / signal notify have
     * store side effects. Delegate so the plain store + side effect keep the
     * slow path's exact ordering. */
    if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) { vm_write32(a, v); return; }
    ppu_fm_write32_body(a, ea, v);
}

static __inline PPU_FM_NOINLINE void ppu_fm_write64_out(uint64_t a, uint64_t v)
{
    uint32_t ea = ppu_fm_alias((uint32_t)a);
    if (PPU_FM_UNLIKELY(ppu_fm_slow & PPU_FM_S_WRITE)) {
        if (ppu_fm_slow & PPU_FM_S_COUNT) ppu_fm_count_hit(PPU_FM_C_WRITE64);
        vm_write64(a, v); return;
    }
    if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, 8))) { vm_write64(a, v); return; }
    if (PPU_FM_UNLIKELY(ea < 0x1000u)) { vm_write64(a, v); return; }
    ppu_fm_write64_body(a, ea, v);
}

/* ----------------------------------------------------------------------- */
/* accessors: the gate, then the fast body                                  */
/* ----------------------------------------------------------------------- */

#if PPU_FM4
/* PPU_FM4: the lean gate. The site loads the page byte of the EA once, and
 * makes one test: the route bits of the byte for the kind, and the flag
 * bits that apply to the kind. A set bit sends the access to the
 * out-of-line function. That function has each exact test: the flag word,
 * the alias fixup, the bounds, the null pages, the raw SPU registers, the
 * GCM ref-poll address (read32) and the read sync. A test that is true
 * sends the access to the family. Else the access gets the fast body
 * there. The page byte holds the answer of the tests that the gate of
 * PPU_FM3 made inline:
 *   - bounds: ppu_fm_arm flags each page that is not entirely inside
 *     ppu_vm_size, and the page before the end. An access of up to 8
 *     bytes that starts in its last 7 bytes ends past ppu_vm_size;
 *   - GCM ref-poll: the page of the address, for reads;
 *   - read sync: vm_read_sync_mark_range sets K_SYNC and the read bit on
 *     the pages of its range, and the read bit on the page before the
 *   range. It calls a hook; ppu_fm_arm installs it;
 *   - alias window: a page of the window with the page answer ALIAS has
 *     K_ALIAS. The one test of the site includes that bit, so a plain
 *     page pays no instruction for it. The block of the test sends a set
 *     route bit or flag bit out of line, with the original address. Else
 *     the access runs on the primary address, EA - 0x05000000, after one
 *     test of the page byte of that address (ppu_fm_alias_ea).
 * Before ppu_fm_arm each bit of the flag word is set: each access takes
 * the out-of-line function. */

#define PPU_FM_LEAN_READ(name, type)                                          \
    static PPU_FM_FAST type name(uint64_t a)                                  \
    {                                                                         \
        uint32_t ea = (uint32_t)a;                                            \
        unsigned char k = ppu_fm_page(ea >> PPU_FM_PAGE_SHIFT);               \
        if (PPU_FM_UNLIKELY(PPU_FM_LEAN(k, PPU_FM_K_ALIAS | PPU_FM_K_READ,    \
                                        PPU_FM_S_READ))                       \
            && (PPU_FM_LEAN(k, PPU_FM_K_READ, PPU_FM_S_READ)                  \
                || !ppu_fm_alias_ea(PPU_FM_K_READ, PPU_FM_S_READ, &ea)))      \
            return name##_out(a);                                             \
        return name##_lean(ea);                                               \
    }

#define PPU_FM_LEAN_WRITE(name, type)                                         \
    static PPU_FM_FAST void name(uint64_t a, type v)                          \
    {                                                                         \
        uint32_t ea = (uint32_t)a;                                            \
        unsigned char k = ppu_fm_page(ea >> PPU_FM_PAGE_SHIFT);               \
        if (PPU_FM_UNLIKELY(PPU_FM_LEAN(k, PPU_FM_K_ALIAS | PPU_FM_K_WRITE,   \
                                        PPU_FM_S_WRITE))                      \
            && (PPU_FM_LEAN(k, PPU_FM_K_WRITE, PPU_FM_S_WRITE)                \
                || !ppu_fm_alias_ea(PPU_FM_K_WRITE, PPU_FM_S_WRITE, &ea))) {  \
            name##_out(a, v);                                                 \
            return;                                                           \
        }                                                                     \
        name##_body(a, ea, v);                                                \
    }


PPU_FM_LEAN_READ(ppu_fm_read8, uint8_t)
PPU_FM_LEAN_READ(ppu_fm_read16, uint16_t)
PPU_FM_LEAN_READ(ppu_fm_read32, uint32_t)
PPU_FM_LEAN_READ(ppu_fm_read64, uint64_t)
PPU_FM_LEAN_WRITE(ppu_fm_write8, uint8_t)
PPU_FM_LEAN_WRITE(ppu_fm_write16, uint16_t)
PPU_FM_LEAN_WRITE(ppu_fm_write32, uint32_t)
PPU_FM_LEAN_WRITE(ppu_fm_write64, uint64_t)
#undef PPU_FM_LEAN_READ
#undef PPU_FM_LEAN_WRITE
#elif PPU_FM3
/* PPU_FM3: the read accessors hold no label address, so Clang inlines them
 * into the lifted code. (Clang 17 does not inline a function that holds a
 * label address and a live TLS access: "blockaddress used outside of
 * callbr".) The gate gives the alias answer of a page of the window inline
 * (ppu_fm_window). The fire half takes the caller address in ppu_fm_fireN. */
static PPU_FM_FAST uint8_t ppu_fm_read8(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    uint32_t flags = ppu_fm_slow;
    if (PPU_FM_UNLIKELY(PPU_FM_GATE(flags, ea, PPU_FM_K_READ, PPU_FM_S_READ))
        && !ppu_fm_window(flags, PPU_FM_S_READ, 1, &ea))
        return ppu_fm_read8_out(a);
    return ppu_fm_read8_body(ea, PPU_FM_RA3());
}

static PPU_FM_FAST uint16_t ppu_fm_read16(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    uint32_t flags = ppu_fm_slow;
    if (PPU_FM_UNLIKELY(PPU_FM_GATE(flags, ea, PPU_FM_K_READ, PPU_FM_S_READ))
        && !ppu_fm_window(flags, PPU_FM_S_READ, 2, &ea))
        return ppu_fm_read16_out(a);
    return ppu_fm_read16_body(ea, PPU_FM_RA3());
}

static PPU_FM_FAST uint32_t ppu_fm_read32(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    uint32_t flags = ppu_fm_slow;
    if (PPU_FM_UNLIKELY(PPU_FM_GATE(flags, ea, PPU_FM_K_READ, PPU_FM_S_READ))) {
        if (!ppu_fm_window(flags, PPU_FM_S_READ, 4, &ea)) return ppu_fm_read32_out(a);
        /* GCM_REFPOLL: the fence runs before this read, and only the
         * out-of-line function has it. The page table flags the page of
         * the address, so each access of that page goes there. */
        if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) return ppu_fm_read32_out(a);
    }
    return ppu_fm_read32_body(ea, PPU_FM_RA3());
}

static PPU_FM_FAST uint64_t ppu_fm_read64(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    uint32_t flags = ppu_fm_slow;
    if (PPU_FM_UNLIKELY(PPU_FM_GATE(flags, ea, PPU_FM_K_READ, PPU_FM_S_READ))
        && !ppu_fm_window(flags, PPU_FM_S_READ, 8, &ea))
        return ppu_fm_read64_out(a);
    return ppu_fm_read64_body(ea, PPU_FM_RA3());
}
#else
static PPU_FM_FAST uint8_t ppu_fm_read8(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_READ, PPU_FM_S_READ)))
        return ppu_fm_read8_out(a);
    PPU_FM_SITE return ppu_fm_read8_body(ea, PPU_FM_RA());
}

static PPU_FM_FAST uint16_t ppu_fm_read16(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_READ, PPU_FM_S_READ)))
        return ppu_fm_read16_out(a);
    PPU_FM_SITE return ppu_fm_read16_body(ea, PPU_FM_RA());
}

static PPU_FM_FAST uint32_t ppu_fm_read32(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_READ, PPU_FM_S_READ)))
        return ppu_fm_read32_out(a);
    PPU_FM_SITE return ppu_fm_read32_body(ea, PPU_FM_RA());
}

static PPU_FM_FAST uint64_t ppu_fm_read64(uint64_t a)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_READ, PPU_FM_S_READ)))
        return ppu_fm_read64_out(a);
    PPU_FM_SITE return ppu_fm_read64_body(ea, PPU_FM_RA());
}
#endif /* PPU_FM3 */

#if PPU_FM3 && !PPU_FM4
/* PPU_FM3: the write accessors with the alias answer of the gate. The body
 * gets the answered EA, as in the out-of-line function. So the test of the
 * coherent store (g_spu_coh_armed) reads the line of the answered address. */
#define PPU_FM_WRITE3(name, type, n)                                         \
    static PPU_FM_FAST void name(uint64_t a, type v)                         \
    {                                                                        \
        uint32_t ea = (uint32_t)a;                                           \
        uint32_t flags = ppu_fm_slow;                                        \
        if (PPU_FM_UNLIKELY(PPU_FM_GATE(flags, ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE)) && \
            !ppu_fm_window(flags, PPU_FM_S_WRITE, n, &ea)) {                 \
            name##_out(a, v);                                                \
            return;                                                          \
        }                                                                    \
        name##_body(a, ea, v);                                               \
    }

PPU_FM_WRITE3(ppu_fm_write8,  uint8_t,  1)
PPU_FM_WRITE3(ppu_fm_write16, uint16_t, 2)
PPU_FM_WRITE3(ppu_fm_write32, uint32_t, 4)
PPU_FM_WRITE3(ppu_fm_write64, uint64_t, 8)
#undef PPU_FM_WRITE3
#elif !PPU_FM3
static PPU_FM_FAST void ppu_fm_write8(uint64_t a, uint8_t v)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE))) {
        ppu_fm_write8_out(a, v); return;
    }
    ppu_fm_write8_body(a, ea, v);
}

static PPU_FM_FAST void ppu_fm_write16(uint64_t a, uint16_t v)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE))) {
        ppu_fm_write16_out(a, v); return;
    }
    ppu_fm_write16_body(a, ea, v);
}

static PPU_FM_FAST void ppu_fm_write32(uint64_t a, uint32_t v)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE))) {
        ppu_fm_write32_out(a, v); return;
    }
    ppu_fm_write32_body(a, ea, v);
}

static PPU_FM_FAST void ppu_fm_write64(uint64_t a, uint64_t v)
{
    uint32_t ea = (uint32_t)a;
    if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE))) {
        ppu_fm_write64_out(a, v); return;
    }
    ppu_fm_write64_body(a, ea, v);
}
#endif /* PPU_FM3 */

#endif /* PS3EMU_PPU_FASTMEM_H */
