/* Synthetic contract test for runtime/syscalls/spu_ea_alias.c; no guest
 * payload. The production translation unit is included so the test can reset
 * its recorded ranges between layouts. Every answer of the production
 * sysmem_spu_alias_fixup (per-page table plus scan fallback) must equal the
 * pre-table reference below, which is the previous implementation verbatim,
 * after every single record, over authored and pseudo-random layouts:
 * unaligned, overlapping, adjacent, page-straddling, window-straddling and
 * wrapping ranges, zero sizes, more ranges than the table holds, and real
 * allocations sitting on another allocation's +0x05000000 image. Page-aligned
 * layouts must also be answered by the table alone (no MIXED pages), so a
 * correct-but-always-scanning table does not pass. tools/test_spu_ea_alias.py
 * builds this against mutated copies of the production file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "../spu_ea_alias.c"

/* Readers query while sys_memory appends under its writer lock. */
_Static_assert(_Generic(&s_alias_page[0], _Atomic uint8_t*: 1, default: 0),
               "Publish alias-page answers with C11 atomics");
_Static_assert(_Generic(&s_alias_n, _Atomic int*: 1, default: 0),
               "Publish initialized range entries with an atomic count");

static int ref_in_table(uint32_t ea)
{
    for (int i = 0; i < s_alias_n; i++)
        if (ea >= s_alias[i].addr && ea < s_alias[i].addr + s_alias[i].size)
            return 1;
    return 0;
}

static uint32_t ref_fixup(uint32_t ea)
{
    if ((ea & 0x80000000u) != 0u && ea < 0x90000000u)
        return 0x40000000u | (ea & 0x3FFFFFFFu);
    if (ea < 0x45000000u) return ea;
    uint32_t prim = ea - 0x05000000u;
    if (ref_in_table(prim) && !ref_in_table(ea)) return prim;
    return ea;
}

static void reset(void)
{
    s_alias_n = 0;
    memset(s_alias, 0, sizeof s_alias);
    for (uint32_t page = 0; page < SPU_ALIAS_PAGES; page++)
        atomic_store_explicit(&s_alias_page[page], 0, memory_order_relaxed);
}

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static unsigned long long s_checks, s_alias_answers, s_mixed_pages;

static void expect(uint32_t ea, const char* layout, int record)
{
    uint32_t got = sysmem_spu_alias_fixup(ea), want = ref_fixup(ea);
    ++s_checks;
    if (want != ea) ++s_alias_answers;
    if (got != want) {
        fprintf(stderr, "FAIL %s record %d: ea=%08X got=%08X want=%08X\n", layout, record, ea, got, want);
        exit(1);
    }
}

/* Dense probes around every interesting boundary, plus a random sweep. */
static void sweep(const char* layout, int record)
{
    static const int64_t deltas[] = { -4097, -4096, -4095, -17, -1, 0, 1, 15, 4095, 4096, 4097 };
    for (int i = 0; i < s_alias_n; i++) {
        uint64_t marks[4] = { s_alias[i].addr, (uint64_t)s_alias[i].addr + s_alias[i].size,
                              (uint64_t)s_alias[i].addr + 0x05000000u,
                              (uint64_t)s_alias[i].addr + s_alias[i].size + 0x05000000u };
        for (int m = 0; m < 4; m++)
            for (unsigned d = 0; d < sizeof deltas / sizeof deltas[0]; d++) {
                int64_t ea = (int64_t)marks[m] + deltas[d];
                if (ea >= 0 && ea <= 0xFFFFFFFFll) expect((uint32_t)ea, layout, record);
            }
    }
    static const uint32_t fixed[] = { 0x44FFFFFFu, 0x45000000u, 0x45000FFFu, 0x45001000u, 0x4FFFFFFFu,
                                      0x50000000u, 0x50000FFFu, 0x54FFFFFFu, 0x80440440u, 0x8FFFFFFFu,
                                      0x90000000u, 0xC5000000u, 0xFFFFFFFFu, 0 };
    for (unsigned k = 0; k < sizeof fixed / sizeof fixed[0]; k++) expect(fixed[k], layout, record);
    for (int k = 0; k < 4000; k++) expect(0x44FF0000u + rnd() % 0x0B020000u, layout, record);
    for (int k = 0; k < 500; k++) expect(rnd() ^ (rnd() << 16), layout, record);
}

static int count_state(int state)
{
    int n = 0;
    for (uint32_t p = 0; p < SPU_ALIAS_PAGES; p++) n += s_alias_page[p] == state;
    return n;
}

static void expect_state(uint32_t ea, int state, const char* layout)
{
    int got = s_alias_page[(ea - SPU_ALIAS_WINDOW_LO) >> SPU_ALIAS_PAGE_SHIFT];
    if (got != state) {
        fprintf(stderr, "FAIL %s: page of %08X is %d, want %d\n", layout, ea, got, state);
        exit(1);
    }
}

static void record(uint32_t addr, uint32_t size, const char* layout)
{
    sysmem_alias_record(addr, size);
    sweep(layout, s_alias_n);
}


/* One allocator writer publishes while four readers exercise full-page and
 * mixed-page answers. A published count admits only completed registrations. */
static atomic_int readers_ready, readers_go, readers_done, published_ranges, readers_observed;
static atomic_uint concurrent_probes, concurrent_failures;
#ifdef _WIN32
static DWORD WINAPI concurrent_reader(void* unused)
#else
static void* concurrent_reader(void* unused)
#endif
{
    (void)unused;
    atomic_fetch_add(&readers_ready, 1);
    while (!atomic_load(&readers_go)) {}
    while (!atomic_load(&readers_done)) {
        const int published = atomic_load(&published_ranges);
        for (int range = 0; range < SPU_ALIAS_MAX; range++) {
            const uint32_t ea = 0x45000010u + (uint32_t)range * 0x2000u;
            const uint32_t got = sysmem_spu_alias_fixup(ea);
            if ((range < published && got != ea - 0x05000000u) ||
                (got != ea && got != ea - 0x05000000u))
                atomic_fetch_add(&concurrent_failures, 1);
        }
        atomic_fetch_add(&concurrent_probes, SPU_ALIAS_MAX);
        atomic_store(&readers_observed, published);
    }
    return 0;
}

static void check_concurrent_registration(void)
{
    reset();
#ifdef _WIN32
    HANDLE readers[4];
    for (int i = 0; i < 4; i++) {
        readers[i] = CreateThread(NULL, 0, concurrent_reader, NULL, 0, NULL);
        if (!readers[i]) exit(1);
    }
#else
    pthread_t readers[4];
    for (int i = 0; i < 4; i++)
        if (pthread_create(&readers[i], NULL, concurrent_reader, NULL)) exit(1);
#endif
    while (atomic_load(&readers_ready) != 4) {}
    atomic_store(&readers_go, 1);
    while (atomic_load(&concurrent_probes) < 128) {}
    for (int range = 0; range < SPU_ALIAS_MAX; range++) {
        const uint32_t addr = 0x40000000u + (uint32_t)range * 0x2000u;
        sysmem_alias_record(addr + (range & 1 ? 1u : 0u), 0x1000u);
        atomic_store(&published_ranges, range + 1);
        while (atomic_load(&readers_observed) < range + 1) {}
    }
    atomic_store(&readers_done, 1);
#ifdef _WIN32
    for (int i = 0; i < 4; i++) {
        DWORD code;
        if (WaitForSingleObject(readers[i], 10000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(readers[i], &code) || code || !CloseHandle(readers[i])) exit(1);
    }
#else
    for (int i = 0; i < 4; i++)
        if (pthread_join(readers[i], NULL)) exit(1);
#endif
    if (atomic_load(&concurrent_failures)) {
        fprintf(stderr, "FAIL concurrent range publication\n");
        exit(1);
    }
    sweep("concurrent registrations complete", SPU_ALIAS_MAX);
    puts("PASS concurrent full/mixed-page publication with four readers");
}

int main(void)
{
    check_concurrent_registration();
    s_rng = 0x9E3779B97F4A7C15ull;
    reset(); sweep("empty", 0);

    /* The sys_memory bump layout: 24 MiB blocks from 0x40000000, so the
     * high ones' images and the low ones' bodies share the window. */
    reset();
    for (uint32_t i = 0; i < 8; i++) record(0x40000000u + i * 0x01800000u, 0x01800000u, "bump");
    expect_state(0x45000000u, SPU_ALIAS_PAGE_IDENTITY, "bump");   /* inside a real block */
    if (count_state(SPU_ALIAS_PAGE_MIXED) != 0 || count_state(SPU_ALIAS_PAGE_ALIAS) == 0) {
        fprintf(stderr, "FAIL bump: aligned layout left %d mixed and %d alias pages\n",
                count_state(SPU_ALIAS_PAGE_MIXED), count_state(SPU_ALIAS_PAGE_ALIAS));
        exit(1);
    }

    /* Recorded high-to-low; one page covered only by the union of two
     * ranges, the next by none. */
    reset(); record(0x40000800u, 0x00000800u, "union"); record(0x40000000u, 0x00000800u, "union");
    record(0x40002000u, 0x00001000u, "union"); record(0x40001800u, 0x00000800u, "union");
    expect_state(0x45000000u, SPU_ALIAS_PAGE_ALIAS, "union");
    expect_state(0x45001000u, SPU_ALIAS_PAGE_MIXED, "union");
    expect_state(0x45002000u, SPU_ALIAS_PAGE_ALIAS, "union");
    expect_state(0x45003000u, SPU_ALIAS_PAGE_IDENTITY, "union");

    /* A real allocation exactly on another's image, and one half on it. */
    reset(); record(0x41000000u, 0x00200000u, "image");
    record(0x46000000u, 0x00100000u, "image"); record(0x46180800u, 0x00100000u, "image");

    /* Unaligned, overlapping, adjacent and page-straddling neighbours. */
    reset(); record(0x40001234u, 0x00002000u, "overlap"); record(0x40002000u, 0x00003001u, "overlap");
    record(0x40005001u, 0x00000FFFu, "overlap"); record(0x40000800u, 0x00000900u, "overlap");
    record(0x45001234u, 0x00000010u, "overlap"); record(0x40000000u, 0x00001000u, "overlap");

    /* Window edges and wrapping 32-bit ends (which match nothing). */
    reset(); record(0x44FFF800u, 0x00001000u, "edges"); record(0x4AFFF000u, 0x00002000u, "edges");
    record(0x4FFFF001u, 0x00002000u, "edges"); record(0xFFFFF000u, 0x00002000u, "edges");
    record(0x40000000u, 0u, "edges"); record(0x40000000u, 0xC0000000u, "edges");

    /* More records than the table holds: the 33rd onwards are ignored. */
    reset();
    for (uint32_t i = 0; i < SPU_ALIAS_MAX + 6; i++) record(0x40000000u + i * 0x00080000u + (i & 3) * 0x123u, 0x00040000u, "cap");

    /* Pseudo-random layouts. */
    for (int layout = 0; layout < 60; layout++) {
        reset();
        int n = 1 + (int)(rnd() % 40);
        for (int i = 0; i < n; i++) {
            static const uint32_t align[] = { 1u, 16u, 4096u, 0x00100000u };
            uint32_t a = align[rnd() % 4];
            uint32_t addr = (0x3F000000u + rnd() % 0x13000000u) & ~(a - 1u);
            uint32_t size = 1u + rnd() % ((rnd() & 1) ? 0x00010000u : 0x03000000u);
            if (rnd() % 16 == 0) size = 0;
            if (rnd() % 32 == 0) addr = 0xFFFF0000u + rnd() % 0x10000u;
            record(addr, size, "random");
        }
        int mixed = 0;
        for (uint32_t p = 0; p < SPU_ALIAS_PAGES; p++) mixed += s_alias_page[p] == SPU_ALIAS_PAGE_MIXED;
        s_mixed_pages += (unsigned)mixed;
    }
    printf("PASS %llu fixup answers equal the scan reference (%llu aliased; %llu mixed pages over the random layouts)\n",
           s_checks, s_alias_answers, s_mixed_pages);
    return 0;
}
