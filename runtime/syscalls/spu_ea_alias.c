/*
 * ps3recomp - LV1 SPU effective-address alias window (firmware dual mapping).
 *
 * LV1 maps every memory-container page for SPU access TWICE: the primary VA
 * the PPU sees, and a second window at +0x05000000 (the "SPU color" alias the
 * SPURS kernel uses for its work-area EAs -- the SPU-side code computes
 * base|0x05000000 directly and never issues a syscall for it). On firmware
 * both VAs resolve to the same physical pages.
 *
 * This runtime's VM is flat, so an unimplemented alias sends every SPU
 * completion PUT into unrelated committed pages: the PPU protocol memory
 * never hears back, the title wedges wherever the race lands, and whatever
 * allocation owns the +0x05000000 pages is silently corrupted (MotorStorm
 * BCES00006 dead boots: 8432 completion PUTs to 0x452443D0 while the ticket
 * header lives at 0x40244380).
 *
 * Fix: registration-based EA normalization on the SPU paths only. When an
 * SPU MFC/atomic EA carries the 0x05000000 window bit, the primary EA is a
 * registered allocation, and the EA as-is is NOT, the address is the same
 * memory under the alias and is normalized down. Anything else -- a real
 * allocation at the aliased address, an unregistered primary -- is left
 * untouched, so behavior only changes exactly where firmware would have
 * resolved both VAs to the same bytes.
 *
 * Self-contained on purpose: sys_memory.c registers its handouts; the SPU
 * DMA/atomic layers query. Unit-testable straight-in
 * (runtime/syscalls/tests/test_spu_ea_alias.c).
 */

#include <stdint.h>
#include <stdatomic.h>

/* Allocation and mapper paths can register concurrently. Serialize writers
 * here while leaving all guest-address readers lock-free. */
#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_alias_writer_lock = SRWLOCK_INIT;
static void spu_alias_record_lock(void) { AcquireSRWLockExclusive(&s_alias_writer_lock); }
static void spu_alias_record_unlock(void) { ReleaseSRWLockExclusive(&s_alias_writer_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_alias_writer_lock = PTHREAD_MUTEX_INITIALIZER;
static void spu_alias_record_lock(void) { pthread_mutex_lock(&s_alias_writer_lock); }
static void spu_alias_record_unlock(void) { pthread_mutex_unlock(&s_alias_writer_lock); }
#endif

#define SPU_ALIAS_MAX 32

typedef struct spu_alias_range {
    uint32_t addr;
    uint32_t size;
} spu_alias_range;

static spu_alias_range s_alias[SPU_ALIAS_MAX];
static _Atomic int     s_alias_n;

static int spu_alias_in_table(uint32_t ea)
{
    const int count = atomic_load_explicit(&s_alias_n, memory_order_acquire);
    for (int i = 0; i < count; i++)
        if (ea >= s_alias[i].addr && ea < s_alias[i].addr + s_alias[i].size)
            return 1;
    return 0;
}

/* Per-page answers for the window the PPU fast path sends here.
 *
 * Every lifted guest access in [0x45000000, 0x50000000) calls
 * sysmem_spu_alias_fixup, and a title's heap can live there, so the two
 * linear scans above ran on ordinary loads and stores. The answer is a pure
 * function of the recorded ranges, which change only in sysmem_alias_record
 * (rare, under the internal writer lock), so each 4 KiB page caches it:
 *   IDENTITY  every EA in the page maps to itself
 *   ALIAS     every EA in the page maps to EA - 0x05000000
 *   MIXED     the page or its primary is partly covered: scan as before
 * Zero (identity) is correct for an empty table. Atomic page publication
 * lets concurrent readers see the old or new answer while registration is
 * in progress. The atomic range count publishes fully initialized entries
 * before a mixed-page reader can scan them. The internal blocking mutex
 * serializes writers; published range entries are immutable. */
#define SPU_ALIAS_PAGE_SHIFT 12u
#define SPU_ALIAS_WINDOW_LO  0x45000000u
#define SPU_ALIAS_WINDOW_HI  0x50000000u
#define SPU_ALIAS_PAGES      ((SPU_ALIAS_WINDOW_HI - SPU_ALIAS_WINDOW_LO) >> SPU_ALIAS_PAGE_SHIFT)
enum { SPU_ALIAS_PAGE_IDENTITY = 0, SPU_ALIAS_PAGE_ALIAS = 1, SPU_ALIAS_PAGE_MIXED = 2 };
static _Atomic uint8_t s_alias_page[SPU_ALIAS_PAGES];

/* How much of [lo, lo + 4 KiB) the recorded ranges cover, with the same
 * 32-bit end arithmetic as spu_alias_in_table: 0 none, 1 partly, 2 all. */
static int spu_alias_page_cover(uint32_t lo)
{
    const uint64_t page_end = (uint64_t)lo + (1u << SPU_ALIAS_PAGE_SHIFT);
    uint64_t start[SPU_ALIAS_MAX], end[SPU_ALIAS_MAX];
    int n = 0;
    const int count = atomic_load_explicit(&s_alias_n, memory_order_acquire);
    for (int i = 0; i < count; i++) {
        uint64_t a = s_alias[i].addr, b = (uint32_t)(s_alias[i].addr + s_alias[i].size);
        if (b <= a) continue;                        /* wrapped: matches nothing */
        if (a < lo) a = lo;
        if (b > page_end) b = page_end;
        if (a >= b) continue;
        int k = n++;                                 /* insertion sort by start */
        while (k > 0 && start[k - 1] > a) { start[k] = start[k - 1]; end[k] = end[k - 1]; --k; }
        start[k] = a; end[k] = b;
    }
    if (!n) return 0;
    uint64_t reach = lo;
    for (int k = 0; k < n; k++) {
        if (start[k] > reach) return 1;              /* a hole */
        if (end[k] > reach) reach = end[k];
    }
    return reach >= page_end ? 2 : 1;
}

static void spu_alias_page_update(uint32_t page)
{
    const uint32_t lo = SPU_ALIAS_WINDOW_LO + (page << SPU_ALIAS_PAGE_SHIFT);
    const int self = spu_alias_page_cover(lo);
    const int prim = spu_alias_page_cover(lo - 0x05000000u);
    uint8_t state;
    if (self == 2 || prim == 0) state = SPU_ALIAS_PAGE_IDENTITY;
    else if (prim == 2 && self == 0) state = SPU_ALIAS_PAGE_ALIAS;
    else state = SPU_ALIAS_PAGE_MIXED;
    atomic_store_explicit(&s_alias_page[page], state, memory_order_release);
}

/* Refresh the pages whose answer [addr, addr + size) can change: the range
 * itself (as EAs) and its +0x05000000 image (as aliases of it). */
static void spu_alias_pages_refresh(uint32_t addr, uint32_t size)
{
    const uint64_t a0 = addr, a1 = (uint64_t)addr + size;
    const uint64_t spans[2][2] = { { a0, a1 }, { a0 + 0x05000000u, a1 + 0x05000000u } };
    for (int s = 0; s < 2; s++) {
        uint64_t lo = spans[s][0], hi = spans[s][1];
        if (lo < SPU_ALIAS_WINDOW_LO) lo = SPU_ALIAS_WINDOW_LO;
        if (hi > SPU_ALIAS_WINDOW_HI) hi = SPU_ALIAS_WINDOW_HI;
        if (lo >= hi) continue;
        const uint32_t first = (uint32_t)((lo - SPU_ALIAS_WINDOW_LO) >> SPU_ALIAS_PAGE_SHIFT);
        const uint32_t last = (uint32_t)((hi - 1 - SPU_ALIAS_WINDOW_LO) >> SPU_ALIAS_PAGE_SHIFT);
        for (uint32_t p = first; p <= last; p++) spu_alias_page_update(p);
    }
}

/* sys_memory.c records each handed-out allocation window here. */
void sysmem_alias_record(uint32_t addr, uint32_t size)
{
    spu_alias_record_lock();
    const int count = atomic_load_explicit(&s_alias_n, memory_order_relaxed);
    if (count < SPU_ALIAS_MAX && size) {
        s_alias[count].addr = addr;
        s_alias[count].size = size;
        atomic_store_explicit(&s_alias_n, count + 1, memory_order_release);
        spu_alias_pages_refresh(addr, size);
    }
    spu_alias_record_unlock();
}

/* Normalize an SPU/PPU-side EA through the LV1 dual mapping. Returns the EA
 * the PPU-side protocol memory lives at; identity when the alias does not
 * apply.
 *
 * The window is ARITHMETIC: firmware maps each container page at its primary
 * VA and again at exactly +0x05000000 (guest code computes base+0x05000000;
 * MotorStorm's CreateTask args 0x41ED0000->0x46ED0000 prove ADD, and the
 * original WWS case 0x40244380->0x452443D0 agrees since OR==ADD there).
 * A mask (ea & ~0x05000000) only reproduces this when the primary lacks the
 * window bits itself; for primaries like 0x41EBC100 the OR-form (0x45EBC100)
 * and ADD-form (0x46EBC100) diverge and the mask silently matched neither. */
uint32_t sysmem_spu_alias_fixup(uint32_t ea)
{
    /* MFC storage alias, EA bit 31 (bg_275): the CBE MFC addresses main
     * storage through a second mapping at 0x80000000|ea -- EA[31] is
     * ignored for main-storage targets, and Sony code posts these aliased
     * EAs routinely (MotorStorm's SPURS claims its job queue at
     * 0x80440440 while the queue lives at 0x00440440 -- the low-heap
     * layout right after the EBOOT). RPCS3 needs no explicit rule because
     * vm::_ptr(eal) (SPUThread.cpp do_dma_transfer, line ~2058) resolves
     * through a VM whose mapping already covers the alias; our flat VM
     * must normalize. Raw-SPU problem state (0xE0000000+) keeps its bit31
     * MMIO meaning -- the < 0x90000000 bound leaves it untouched, and the
     * PS3 kernel range 0x90000000+ is never an SPU DMA target. Applied
     * BEFORE the +0x05000000 window so both aliases compose. */
    if ((ea & 0x80000000u) != 0u && ea < 0x90000000u)
        return 0x40000000u | (ea & 0x3FFFFFFFu); /* bg_276: 0x00440440 is
         * EBOOT .text; the posted queue arena lives at 0x4004xxxx+, so the
         * bit31 mapping is into the sys_memory container arena, not low
         * memory. [mfc-alias] probe in spu_channels.c verifies live. */
    /* Registration determines the valid alias extent; larger allocation
     * arenas may place a registered primary above the old 0x50000000 end. */
    if (ea < 0x45000000u) return ea;
    uint32_t prim = ea - 0x05000000u;
    if (ea < SPU_ALIAS_WINDOW_HI) {
        const uint8_t state = atomic_load_explicit(
            &s_alias_page[(ea - SPU_ALIAS_WINDOW_LO) >> SPU_ALIAS_PAGE_SHIFT],
            memory_order_acquire);
        if (state == SPU_ALIAS_PAGE_IDENTITY) return ea;
        if (state == SPU_ALIAS_PAGE_ALIAS) return prim;
    }
    if (spu_alias_in_table(prim) && !spu_alias_in_table(ea)) return prim;
    return ea;
}

/* Allocator collision guard (called from sys_memory.c under the bump lock):
 * a candidate real allocation must not sit on the +0x05000000 alias image of
 * any recorded allocation, and must not place its own alias image on top of
 * one either. On firmware both VAs of one page are the SAME physical memory,
 * so two different allocations whose VAs differ by exactly 0x05000000 cannot
 * exist; the observed 17MB block [0x45F00000,0x47000000) landing on the 56MB
 * block's image was an impossible layout and made every EA in it ambiguous. */
int sysmem_alias_va_conflicts(uint32_t addr, uint32_t size)
{
    const int count = atomic_load_explicit(&s_alias_n, memory_order_acquire);
    for (int i = 0; i < count; i++) {
        uint32_t a = s_alias[i].addr, b = a + s_alias[i].size;
        uint32_t ia = a + 0x05000000u, ib = b + 0x05000000u; /* its image */
        if (addr < ib && ia < addr + size) return 1; /* candidate on its image */
        if (addr + 0x05000000u < b && a < addr + 0x05000000u + size) return 1; /* ours on an alloc */
    }
    return 0;
}
