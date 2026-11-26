/* The read sync memo (libs/video/rsx_read_sync_memo.h) against the direct
   call of vm_read_sync_mark_range: the same state after the same binds, with
   fewer calls. A simple model of the memo tells for each bind whether the
   real mark must run.
   The runtime file runtime/memory/guest_read_sync.c is compiled in, so the
   real mark function and the real mark table run. The page bytes of the PPU
   fast memory path are a model with the three bits that the hook writes and
   the rule of ppu_fm_window_end.
   Build 1: the production sizes. Build 2: RSX_READ_SYNC_MEMO_ENTRIES 8u and
   RSX_READ_SYNC_MEMO_PAGES 131072u, to reach eviction and slot collisions.
   Usage: check old|new <dump file> */

#include "rsx_read_sync_memo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIND_READ 0x01u
#define KIND_WRITE 0x02u
#define KIND_SYNC 0x20u
#define PAGE 4096u
#define MAX_CODES 4096

static unsigned char g_kind[RSX_READ_SYNC_MEMO_PAGES];
static unsigned g_hook_calls;
static int g_failures, g_checks, g_new_path;
static unsigned char g_codes[MAX_CODES];
static unsigned g_code_count;
static rsx_read_sync_memo g_memo;
static struct { uint32_t address; uint64_t bytes; } g_model[RSX_READ_SYNC_MEMO_ENTRIES];
static unsigned char g_model_inside[RSX_READ_SYNC_MEMO_PAGES];

#define CHECK(condition, ...) \
    do { \
        g_checks++; \
        if (!(condition)) { \
            g_failures++; \
            printf("FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

/* ppu_fm_mark_sync (runtime/ppu/ppu_loader.cpp) on the model table. */
static void test_hook(unsigned first, unsigned last)
{
    g_hook_calls++;
    if (first > 0) g_kind[first - 1u] |= (unsigned char)KIND_READ;
    for (unsigned page = first; page <= last; page++)
        g_kind[page] |= (unsigned char)(KIND_READ | KIND_SYNC);
}

/* ppu_fm_window_end(page, PPU_FM_ALIAS_IDENTITY) with an arm rule of 0: the
   page keeps only its bits when it has PPU_FM_K_SYNC. */
static void window_end(unsigned page)
{
    const unsigned char keep = (unsigned char)(KIND_READ | KIND_WRITE | KIND_SYNC);
    g_kind[page] = (g_kind[page] & KIND_SYNC) ? (unsigned char)(g_kind[page] & keep) : 0;
}

/* The model of the memo. Returns the result of the mark, and sets *called
   when the real mark must run. */
static int model_mark(uint32_t address, uint64_t bytes, int *called)
{
    const uint32_t first = address >> 12;
    const unsigned slot = (unsigned)((first * 2654435761u) >> 12u) & (RSX_READ_SYNC_MEMO_ENTRIES - 1u);
    *called = 0;
    if (!bytes || bytes > UINT64_C(0x100000000) - (uint64_t)address) return -1;
    if (g_model[slot].bytes == bytes && g_model[slot].address == address
        && (!first || (g_model_inside[first - 1u] & 1u)))
        return 0;                     /* a hit: no call */
    *called = 1;
    g_model[slot].address = address;
    g_model[slot].bytes = bytes;
    const uint32_t last = (uint32_t)(((uint64_t)address + bytes - 1u) >> 12);
    for (uint32_t page = first; page <= last; page++) g_model_inside[page] = 1;
    return 0;
}

static void bind(uint32_t address, uint64_t bytes)
{
    int called_model = 0;
    const int expected = model_mark(address, bytes, &called_model);
    const unsigned before = g_hook_calls;
    const int result = g_new_path ? rsx_read_sync_memo_mark(&g_memo, address, bytes)
                                  : vm_read_sync_mark_range(address, bytes);
    const int called = g_hook_calls != before;
    CHECK(g_code_count < MAX_CODES, "the table of results is full");
    if (g_code_count < MAX_CODES) g_codes[g_code_count++] = (unsigned char)result;
    if (g_new_path) {
        CHECK(result == expected, "bind 0x%08X+%llu: result %d, model %d",
              address, (unsigned long long)bytes, result, expected);
        if (result == expected && result == 0)
            CHECK(called == called_model, "bind 0x%08X+%llu: mark call %d, model %d",
                  address, (unsigned long long)bytes, called, called_model);
    }
}

static uint32_t page_address(unsigned page) { return page * PAGE; }

static void part1(void)
{
    /* One surface, 52 binds. */
    bind(page_address(0), 900u * PAGE);
    for (int i = 0; i < 51; i++) bind(page_address(0), 900u * PAGE);
    /* Two surfaces in turn. */
    bind(page_address(1000), 100u * PAGE);
    for (int i = 0; i < 10; i++) {
        bind(page_address(0), 900u * PAGE);
        bind(page_address(1000), 100u * PAGE);
    }
    /* A surface that starts directly after a marked one. */
    bind(page_address(1100), 100u * PAGE);
    bind(page_address(1100), 100u * PAGE);
    /* The alias answer of the page before it is computed again. */
    window_end(1099);
    bind(page_address(1100), 100u * PAGE);
    /* The page before the range is in no marked range. */
    bind(page_address(1300), 100u * PAGE);
    bind(page_address(1300), 100u * PAGE);
    window_end(1299);
    bind(page_address(1300), 100u * PAGE);
    bind(page_address(1299), 52u * PAGE);   /* now page 1299 is in a marked range */
    window_end(1299);
    bind(page_address(1300), 100u * PAGE);
    /* The same address with other sizes. */
    bind(page_address(2000), 100u * PAGE);
    bind(page_address(2000), 200u * PAGE);
    bind(page_address(2000), 51u * PAGE);
    bind(page_address(2000), 200u * PAGE);
    /* Refused ranges. */
    bind(page_address(3000), 0);
    bind(0xFFFFF000u, 0x2000u);
    bind(page_address(3000), 10u * PAGE);
    bind(page_address(3000), 10u * PAGE);
    /* Forty surfaces, two rounds. */
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 40; i++) bind(page_address(4000 + i * 64), 64u * PAGE);
    /* The same first page with another address: the end is in another page. */
    bind(page_address(7000), 10000u);
    bind(page_address(7000) + 3000u, 10000u);
    bind(page_address(7000) + 3000u, 10000u);
    bind(page_address(7000), 10000u);
    /* A range that starts and ends inside pages. */
    bind(page_address(7500) + 123u, 3u * PAGE - 45u);
    bind(page_address(7500) + 123u, 3u * PAGE - 45u);
}

static void part2(void)
{
    /* A pair with one slot, when the table has one below its page limit. */
    unsigned first = 0, second = 0;
    for (unsigned a = 100; a < 400 && !second; a++)
        for (unsigned b = a + 1; b + 300u < RSX_READ_SYNC_MEMO_PAGES; b++)
            if (((a * 2654435761u) >> 12u) == ((b * 2654435761u) >> 12u)
                || ((((a * 2654435761u) >> 12u) ^ ((b * 2654435761u) >> 12u))
                    & (RSX_READ_SYNC_MEMO_ENTRIES - 1u)) == 0) {
                first = a;
                second = b;
                break;
            }
    if (second) {
        for (int i = 0; i < 6; i++) {
            bind(page_address(first), 100u * PAGE);
            bind(page_address(second), 100u * PAGE);
        }
    }
#if RSX_READ_SYNC_MEMO_PAGES == (1u << 20)
    /* The full guest space: no page before the range. */
    bind(0, UINT64_C(0x100000000));
    bind(0, UINT64_C(0x100000000));
#endif
}

static void capture(unsigned char *marks, unsigned char *kind, unsigned limit)
{
    for (unsigned page = 0; page < limit; page++) {
        marks[page] = vm_read_sync_pages[page];
        kind[page] = g_kind[page];
    }
}

typedef struct {
    unsigned limit, codes, hooks;
    unsigned char *codes_bytes, *marks, *kind;
} state;

static void save_state(FILE *file, const state *s)
{
    fwrite(&s->limit, sizeof s->limit, 1, file);
    fwrite(&s->codes, sizeof s->codes, 1, file);
    fwrite(&s->hooks, sizeof s->hooks, 1, file);
    fwrite(s->codes_bytes, 1, s->codes, file);
    fwrite(s->marks, 1, s->limit, file);
    fwrite(s->kind, 1, s->limit, file);
}

static int load_state(FILE *file, state *s, unsigned limit)
{
    unsigned char *codes_bytes = malloc(MAX_CODES), *marks = malloc(limit), *kind = malloc(limit);
    s->codes_bytes = codes_bytes;
    s->marks = marks;
    s->kind = kind;
    if (!codes_bytes || !marks || !kind) return 0;
    return fread(&s->limit, sizeof s->limit, 1, file) == 1
        && fread(&s->codes, sizeof s->codes, 1, file) == 1
        && fread(&s->hooks, sizeof s->hooks, 1, file) == 1
        && s->limit == limit && s->codes <= MAX_CODES
        && fread(codes_bytes, 1, s->codes, file) == s->codes
        && fread(marks, 1, limit, file) == limit
        && fread(kind, 1, limit, file) == limit;
}

static void compare_state(const state *s, const char *what)
{
    unsigned different = 0, first = 0;
    for (unsigned page = 0; page < s->limit; page++) {
        if (vm_read_sync_pages[page] != s->marks[page] || g_kind[page] != s->kind[page]) {
            if (!different) first = page;
            different++;
        }
    }
    CHECK(different == 0, "%s: %u pages differ from the direct mark, first page %u"
          " (mark %u against %u, page byte 0x%02X against 0x%02X)", what, different, first,
          different ? vm_read_sync_pages[first] : 0, different ? s->marks[first] : 0,
          different ? g_kind[first] : 0, different ? s->kind[first] : 0);
    CHECK(g_code_count == s->codes && !memcmp(g_codes, s->codes_bytes, s->codes),
          "%s: %u results against %u, or another result", what, g_code_count, s->codes);
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "old") && strcmp(argv[1], "new"))) {
        fprintf(stderr, "usage: check old|new <dump file>\n");
        return 2;
    }
    const unsigned limit = RSX_READ_SYNC_MEMO_PAGES;
    vm_read_sync_set_mark_hook(test_hook);
    g_new_path = !strcmp(argv[1], "new");
    int ok = 1;
    if (!g_new_path) {
        FILE *file = fopen(argv[2], "wb");
        state one = {limit, 0, 0, g_codes, malloc(limit), malloc(limit)}, two = one;
        two.marks = malloc(limit);
        two.kind = malloc(limit);
        if (!file || !one.marks || !one.kind || !two.marks || !two.kind) return 2;
        part1();
        one.codes = g_code_count;
        one.hooks = g_hook_calls;
        capture(one.marks, one.kind, limit);
        part2();
        two.codes = g_code_count;
        two.hooks = g_hook_calls;
        capture(two.marks, two.kind, limit);
        save_state(file, &one);
        save_state(file, &two);
        fclose(file);
        printf("old: hook calls %u and %u, binds %u and %u\n", one.hooks, two.hooks - one.hooks,
               one.codes, two.codes - one.codes);
        return 0;
    }
    FILE *file = fopen(argv[2], "rb");
    state one = {0, 0, 0, NULL, NULL, NULL}, two = one;
    if (!file || !load_state(file, &one, limit) || !load_state(file, &two, limit)) {
        printf("FAIL the dump file of the old path is missing or damaged\n");
        return 1;
    }
    part1();
    compare_state(&one, "part 1");
    const unsigned hooks_one = g_hook_calls;
    part2();
    compare_state(&two, "part 2");
    CHECK(vm_read_sync_any != 0, "the flag of the read sync is not set");
    printf("new: hook calls %u and %u (old %u and %u)\n", hooks_one, g_hook_calls - hooks_one,
           one.hooks, two.hooks - one.hooks);
    printf("checks=%d failures=%d\n", g_checks, g_failures);
    ok = g_failures == 0;
    return ok ? 0 : 1;
}
