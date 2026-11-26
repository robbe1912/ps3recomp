/* Bytes that the prepared vertex fetch of rsx_vertex_fetch.c produces, against
 * the scalar path before the vector decode (a verbatim copy of the old file,
 * compiled as ref_*) and against rsx_fetch_attrib.
 *
 * Draws: the captured deformation draws of MotorStorm (rsx_vertex_diet_capture.h),
 * every half and s16 value, every u8 value, random declarations over scattered
 * guest pages, and elements that end at a host guard page. The exit code is 2
 * when one byte differs. Usage: test_vertex_diet [benchmark] */
#include "rsx_vertex_fetch.h"
#include "rsx_vertex_diet_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHECK(condition, ...) \
    do { if (!(condition)) { fprintf(stderr, "FAIL " __VA_ARGS__); fputc('\n', stderr); exit(2); } } while (0)

u8* vm_base;
u32 cellGcmResolveOffset(u32 off) { (void)off; abort(); }

/* The scalar path before the vector decode. */
void ref_vertex_fetch_prepare(rsx_vertex_fetch_batch* batch, const rsx_state* st);
void ref_fetch_vertex_prepared(const rsx_vertex_fetch_batch* batch, u32 vi, rsx_vertex_value out[16]);
void ref_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4]);

#define PAGE 4096u
#define GUEST_PAGES 64u
#define HOST_PAGES 96u          /* 64 pages for MAIN, 32 for LOCAL */

/* MAIN pages that follow each other are adjacent in host memory, as in one
 * IO table block (a captured row of 20 bytes can cross a 4 KiB boundary).
 * LOCAL pages go through a fixed permutation: two guest pages that follow
 * each other are not adjacent here. */
static u8* g_memory;              /* HOST_PAGES pages; one page and the guard page follow in the same block */
static u32 g_guard_page = 0xFFFFFFFFu;   /* guest page whose host page ends at a guard page */
static u8* g_guard_base;                 /* first byte of that host page */
static u32 g_resolves;
static u64 g_trace;

/* Cold rows: each vertex reads a row in a page of its own. The pages lie at
 * pseudo-random places of a 512 MiB region, so a row is not in a cache and
 * its page is not in a TLB. That is the pattern of a race frame: other
 * threads wrote the rows after the last frame. */
#define COLD_PAGES 131072u
#define COLD_FIRST 1024u            /* first guest page of a cold vertex set */
static u8* g_cold;
static u32 g_cold_map[COLD_PAGES];
static int g_cold_mode;

u32 cellGcmResolveLocated(int location, u32 off)
{
    g_resolves++;
    g_trace = g_trace * 0x100000001b3ULL ^ ((u64)(unsigned)location << 32) ^ off;
    if (g_cold_mode) return g_cold_map[off >> 12] * PAGE + (off & (PAGE - 1));
    if ((off >> 12) == g_guard_page)
        return (u32)(g_guard_base + (off & (PAGE - 1)) - vm_base);
    u32 page = off >> 12;
    if (location == 0) return (page % GUEST_PAGES) * PAGE + (off & (PAGE - 1));
    return (GUEST_PAGES + (page * 7u + 3u) % (HOST_PAGES - GUEST_PAGES)) * PAGE + (off & (PAGE - 1));
}

static u64 g_digest = 0xcbf29ce484222325ULL;
static u64 g_vertices;
static u64 g_draws;
static u32 g_capture_vertices;

static void absorb(const void* data, u32 bytes)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < bytes; i++) g_digest = (g_digest ^ p[i]) * 0x100000001b3ULL;
}

static u32 bits(float value) { u32 b; memcpy(&b, &value, 4); return b; }

/* One vertex through the new file and through both scalar paths. The output
 * buffers start with three different fill patterns, so a byte that a path does
 * not write is a difference. */
static void compare(const rsx_state* st, u32 vi, const char* what)
{
    rsx_vertex_fetch_batch current, old;
    rsx_vertex_value now[16], before[16], scalar[16];
    memset(&current, 0xA5, sizeof current);
    memset(&old, 0x5A, sizeof old);
    rsx_vertex_fetch_prepare(&current, st);
    ref_vertex_fetch_prepare(&old, st);
    memset(now, 0x11, sizeof now);
    memset(before, 0x22, sizeof before);
    memset(scalar, 0x33, sizeof scalar);
    g_resolves = 0; g_trace = 0;
    rsx_fetch_vertex_prepared(&current, vi, now);
    u32 count = g_resolves;
    u64 trace = g_trace;
    g_resolves = 0; g_trace = 0;
    ref_fetch_vertex_prepared(&old, vi, before);
    CHECK(count == g_resolves && trace == g_trace, "%s vertex %u: the address resolution changed", what, vi);
    for (int i = 0; i < 16; i++) ref_fetch_attrib(st, i, vi, scalar[i].v);
    for (int i = 0; i < 16; i++) {
        CHECK(memcmp(now[i].v, before[i].v, 16) == 0,
              "%s vertex %u slot %d: new %08x %08x %08x %08x, old %08x %08x %08x %08x", what, vi, i,
              bits(now[i].v[0]), bits(now[i].v[1]), bits(now[i].v[2]), bits(now[i].v[3]),
              bits(before[i].v[0]), bits(before[i].v[1]), bits(before[i].v[2]), bits(before[i].v[3]));
        CHECK(memcmp(now[i].v, scalar[i].v, 16) == 0,
              "%s vertex %u slot %d: the new path differs from rsx_fetch_attrib", what, vi, i);
    }
    absorb(now, sizeof now);
    g_vertices++;
}

/* The guest bytes of each captured row, from the text of the capture. */
static u8 g_capture_rows[RSX_DIET_CAPTURE_ROWS][RSX_DIET_CAPTURE_ROW_BYTES];

static void decode_capture(void)
{
    for (u32 r = 0; r < RSX_DIET_CAPTURE_ROWS; r++)
        for (u32 b = 0; b < RSX_DIET_CAPTURE_ROW_BYTES; b++) {
            u32 byte = 0;
            for (u32 k = 0; k < 2; k++) {
                char c = rsx_diet_capture_rows[r][2 * b + k];
                u32 digit = (u32)(c <= '9' ? c - '0' : c - 'A' + 10);
                CHECK(digit < 16, "capture row %u byte %u: bad hexadecimal digit", r, b);
                byte = byte * 16 + digit;
            }
            g_capture_rows[r][b] = (u8)byte;
        }
}

static void captured(void)
{
    decode_capture();
    for (u32 d = 0; d < RSX_DIET_CAPTURE_DRAWS; d++) {
        const rsx_diet_capture* c = &rsx_diet_capture_draws[d];
        rsx_state st;
        char what[48];
        memset(&st, 0, sizeof st);
        memcpy(st.vertex_data4f, c->constants, sizeof st.vertex_data4f);
        st.frequency_divider_op = c->frequency_divider_op;
        for (u32 i = 0; i < 16; i++) {
            rsx_vertex_attrib* a = &st.vertex_attribs[i];
            a->enabled = c->attribs[i].enabled;
            a->type = c->attribs[i].type;
            a->size = c->attribs[i].size;
            a->stride = c->attribs[i].stride;
            a->offset = c->attribs[i].offset;
            a->frequency = c->attribs[i].frequency;
        }
        sprintf(what, "capture %u", (unsigned)d);
        for (u32 v = 0; v < c->vertices; v++) {
            /* Records share their arrays and hold different bytes: each vertex is written before it is read. */
            u32 at = c->base + (u32)c->indices[v] * RSX_DIET_CAPTURE_ROW_BYTES;
            memcpy(vm_base + cellGcmResolveLocated(0, at), g_capture_rows[c->rows_of[v]],
                   RSX_DIET_CAPTURE_ROW_BYTES);
            compare(&st, c->indices[v], what);
            g_capture_vertices++;
        }
        g_draws++;
    }
}

static void place(u32 at, const void* bytes, u32 count)
{
    memcpy(vm_base + cellGcmResolveLocated(0, at), bytes, count);
}

static rsx_state single(u32 type, u32 size, u32 at)
{
    rsx_state st;
    memset(&st, 0, sizeof st);
    st.vertex_attribs[0].enabled = 1;
    st.vertex_attribs[0].type = type;
    st.vertex_attribs[0].size = size;
    st.vertex_attribs[0].stride = 64;
    st.vertex_attribs[0].offset = at;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) {
            float value = (float)(i * 4 + k) - 7.5f;
            memcpy(&st.vertex_data4f[i][k], &value, 4);
        }
    return st;
}

static void four_big_endian(u32 first, u32 bytes_each, u8* out)
{
    for (u32 k = 0; k < 4; k++)
        for (u32 b = 0; b < bytes_each; b++)
            out[k * bytes_each + b] = (u8)((first + k) >> (8 * (bytes_each - 1 - b)));
}

static void exhaustive(void)
{
    const u32 inside = 32 * PAGE + 2048;
    u8 bytes[8];
    /* Every half value, four lanes at a time. Size < 4 keeps the register in the other lanes. */
    for (u32 size = 1; size <= 4; size++) {
        rsx_state st = single(3, size, inside);
        for (u32 h = 0; h < 65536u; h += 4) {
            four_big_endian(h, 2, bytes);
            place(inside, bytes, 8);
            compare(&st, h / 4, "half");
        }
    }
    /* Every s16 value, normalized (S1) and integer (S32K). */
    for (u32 type = 1; type <= 5; type += 4) {
        rsx_state st = single(type, 4, inside);
        for (u32 h = 0; h < 65536u; h += 4) {
            four_big_endian(h, 2, bytes);
            place(inside, bytes, 8);
            compare(&st, h / 4, "s16");
        }
    }
    /* Every u8 value, normalized (UB). */
    {
        rsx_state st = single(4, 4, inside);
        for (u32 b = 0; b < 256u; b += 4) {
            four_big_endian(b, 1, bytes);
            place(inside, bytes, 4);
            compare(&st, b / 4, "u8");
        }
    }
    g_draws += 11;
}

static u32 g_seed = 0x9E3779B9u;
static u32 rnd(void)
{
    u32 x = g_seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_seed = x;
}

static const u32 special_bits[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u,
    0x3F800000u, 0xBF800000u, 0x3EAAAAABu, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
    0xFFC00001u, 0x7F800001u,
};

static u32 random_bits(void)
{
    return (rnd() & 7u) == 0 ? special_bits[rnd() % 14] : rnd();
}

/* Random declarations: any type (also types the prepared loop does not decode),
 * any size, dense layouts, instancing, elements at the end of a guest page, and
 * constants with NaN, infinity and denormal patterns. */
static void sweep(void)
{
    for (u32 iteration = 0; iteration < 6000; iteration++) {
        rsx_state st;
        u32 active = rnd() % 13;
        u32 shape = rnd() % 4;
        memset(&st, 0, sizeof st);
        for (u32 i = 0; i < 16; i++) {
            rsx_vertex_attrib* a = &st.vertex_attribs[i];
            u32 constant = random_bits();
            for (u32 k = 0; k < 4; k++) memcpy(&st.vertex_data4f[i][k], &constant, 4);
            a->enabled = i < active;
            a->type = shape == 1 ? 1 + rnd() % 7 : rnd() % 10;
            a->size = shape == 1 ? 1 + rnd() % 4 : rnd() % 6;
            a->stride = shape == 1 ? (1 + rnd() % 4) * 4 : rnd() % 64;
            a->frequency = shape == 3 ? rnd() % 5 : 0;
            u32 in_page = shape == 2 ? (PAGE - 1 - rnd() % 48) : rnd() % PAGE;
            a->offset = ((rnd() & 1) ? 0x80000000u : 0u) + (rnd() % GUEST_PAGES) * PAGE + in_page;
        }
        st.frequency_divider_op = rnd();
        /* Changed guest bytes between two reads of the same declaration. */
        u32 patches = rnd() % 8;
        for (u32 p = 0; p < patches; p++) g_memory[rnd() % (HOST_PAGES * PAGE)] = (u8)rnd();
        for (u32 v = 0; v < 6; v++) compare(&st, rnd() % 200, "sweep");
        g_draws++;
    }
}

static rsx_state g_guard_state;
static void guard_call(void) { compare(&g_guard_state, 0, "guard page"); }

#if defined(_WIN32)
/* 1 when the call completed, 0 when it read the guard page. */
static int guarded(void)
{
    __try { guard_call(); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) { return 0; }
    return 1;
}
#else
static sigjmp_buf g_guard_jump;
static volatile sig_atomic_t g_guard_armed;
static void guard_signal(int number)
{
    (void)number;
    if (g_guard_armed) siglongjmp(g_guard_jump, 1);
}
static int guarded(void)
{
    struct sigaction action, old;
    memset(&action, 0, sizeof action);
    action.sa_handler = guard_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &old);
    g_guard_armed = 1;
    int completed = sigsetjmp(g_guard_jump, 0) == 0;
    if (completed) guard_call();
    g_guard_armed = 0;
    sigaction(SIGSEGV, &old, NULL);
    return completed;
}
#endif

/* Guest memory, then one accessible page that ends at the guard page. One
 * block: the host offset of the guard page from vm_base fits a u32. */
static void allocate(void)
{
    u8* block;
    u32 page_bytes;
#ifdef _WIN32
    SYSTEM_INFO info;
    DWORD old;
    GetSystemInfo(&info);
    page_bytes = info.dwPageSize;
    block = (u8*)VirtualAlloc(NULL, (HOST_PAGES + 3) * page_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(block != NULL, "VirtualAlloc of the guest memory and the guard pages");
    CHECK(VirtualProtect(block + (HOST_PAGES + 2) * page_bytes, page_bytes, PAGE_NOACCESS, &old),
          "VirtualProtect of the guard page");
#else
    page_bytes = (u32)sysconf(_SC_PAGESIZE);
    block = (u8*)mmap(NULL, (HOST_PAGES + 3) * page_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(block != MAP_FAILED, "mmap of the guest memory and the guard pages");
    CHECK(mprotect(block + (HOST_PAGES + 2) * page_bytes, page_bytes, PROT_NONE) == 0, "mprotect of the guard page");
#endif
    CHECK(page_bytes == PAGE, "the host page size is not 4096");
    g_memory = block;
    vm_base = block;
    g_guard_base = block + (HOST_PAGES + 1) * PAGE;
}

/* Elements that end at the last byte before a host guard page. A vector load
 * that reads past the element reads the guard page and fails the case. */
static void guard_page(void)
{
    static const u32 cases[][3] = {
        { 2, 4, 16 }, { 2, 3, 12 }, { 2, 2, 8 }, { 2, 1, 4 },
        { 3, 4, 8 }, { 3, 3, 6 }, { 3, 2, 4 }, { 3, 1, 2 },
        { 4, 4, 4 }, { 4, 2, 2 }, { 7, 4, 4 }, { 7, 2, 2 },
        { 1, 4, 8 }, { 1, 2, 4 }, { 5, 4, 8 }, { 5, 2, 4 }, { 6, 4, 4 },
    };
    memset(g_guard_base, 0x5C, PAGE);
    g_guard_page = 61;
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        g_guard_state = single(cases[i][0], cases[i][1], g_guard_page * PAGE + (PAGE - cases[i][2]));
        CHECK(guarded(), "type %u size %u: the read crossed the page end (case %u)",
              cases[i][0], cases[i][1], (unsigned)i);
    }
    g_draws += sizeof cases / sizeof cases[0];
}

static double now(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

typedef struct { u32 slot, type, size, at; } shape_attrib;
typedef struct { const char* name; u32 stride; u32 count; shape_attrib attribs[9]; } shape;

/* The captured paint layout, and scene-like layouts with the types the race
 * fetches (float32, half, CMP, s16, u8). Eight arrays is the densest prepared layout. */
static const shape shapes[] = {
    { "paint (captured)", 20, 3, { { 0, 2, 2, 0 }, { 8, 2, 2, 8 }, { 3, 4, 4, 16 } } },
    { "float x3", 32, 3, { { 0, 2, 4, 0 }, { 1, 2, 3, 16 }, { 2, 2, 2, 28 } } },
    { "half x5", 32, 5, { { 0, 3, 4, 0 }, { 1, 3, 4, 8 }, { 2, 3, 2, 16 }, { 3, 4, 4, 20 }, { 8, 3, 4, 24 } } },
    { "float+cmp", 32, 4, { { 0, 2, 4, 0 }, { 1, 6, 4, 16 }, { 2, 3, 2, 20 }, { 3, 4, 4, 24 } } },
    { "s16", 24, 3, { { 0, 1, 4, 0 }, { 2, 5, 2, 8 }, { 3, 4, 4, 16 } } },
    { "float x8", 128, 8, { { 0, 2, 4, 0 }, { 1, 2, 4, 16 }, { 2, 2, 4, 32 }, { 3, 2, 4, 48 },
                            { 4, 2, 4, 64 }, { 5, 2, 4, 80 }, { 6, 2, 4, 96 }, { 7, 2, 2, 112 } } },
};

typedef void (*fetch_fn)(const rsx_vertex_fetch_batch*, u32, rsx_vertex_value*);

static u64 g_sink;

/* Nanoseconds per vertex: the best of five runs of at least 0.1 s each. */
static double timed(fetch_fn fetch, const rsx_vertex_fetch_batch* batch, rsx_vertex_value* out)
{
    double best = 1e9;
    double seconds, ns;
    u32 passes = 64;
    for (u32 run = 0; run < 6; run++) {
        double started = now();
        for (u32 pass = 0; pass < passes; pass++)
            for (u32 v = 0; v < 64; v++) {
                fetch(batch, v, &out[v]);
                g_sink += ((const u32*)&out[v])[v & 15];
            }
        seconds = now() - started;
        if (run == 0) {              /* sizes the other runs */
            passes = (u32)(6.4 / seconds) + 64;      /* 0.1 s per run */
            if (passes > 100000) passes = 100000;
            continue;
        }
        ns = seconds * 1e9 / ((double)passes * 64.0);
        if (ns < best) best = ns;
    }
    return best;
}

/* Nanoseconds per vertex of the old and the new path, for a cached output
 * buffer (the indexed strip cache) and for the output buffer of the game: a
 * D3D12 upload heap, write-combined for the CPU. */
#ifdef _WIN32
#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")
/* IID_ID3D12Device and IID_ID3D12Resource: the check does not link uuid.lib. */
static const IID rsx_diet_device_iid = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const IID rsx_diet_resource_iid = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
#endif

/* The hit copy of upload_strip_vp_indexed (256 bytes): the CRT call that the game has, and
 * sixteen 16-byte moves (SSE2, the base of x86-64). */
typedef void (*copy_fn)(rsx_vertex_value* out, const rsx_vertex_value* in);

static void copy_crt(rsx_vertex_value* out, const rsx_vertex_value* in) { memcpy(out, in, sizeof *out); }

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
static void copy_sse(rsx_vertex_value* out, const rsx_vertex_value* in)
{
    for (u32 i = 0; i < 16; i++)
        _mm_storeu_si128((__m128i*)out[i].v, _mm_loadu_si128((const __m128i*)in[i].v));
}
#else
static void copy_sse(rsx_vertex_value* out, const rsx_vertex_value* in) { copy_crt(out, in); }
#endif

/* Nanoseconds per call: the best of five runs of at least 0.1 s each. */
static double timed_copy(copy_fn copy, rsx_vertex_value* out, const rsx_vertex_value* in)
{
    double best = 1e9;
    double seconds, ns;
    u64 calls = 4096, c;
    for (u32 run = 0; run < 6; run++) {
        double started = now();
        for (c = 0; c < calls; c++) copy(&out[c & 63], in);
        seconds = now() - started;
        if (run == 0) {              /* sizes the other runs */
            calls = (u64)(409.6 / seconds) + 4096;
            if (calls > 100000000ULL) calls = 100000000ULL;
            continue;
        }
        ns = seconds * 1e9 / (double)c;
        if (ns < best) best = ns;
    }
    g_sink += ((const u32*)&out[c & 63])[3];    /* the stores are not dead */
    return best;
}

#if defined(_M_X64) || defined(__SSE2__)
#include <mmintrin.h>
#define COLD_PREFETCH 1
#else
#define COLD_PREFETCH 0
#endif

/* Brings the row of vertex `vi` of every active array into the cache. */
static void prefetch_vertex(const rsx_vertex_fetch_batch* batch, u32 vi)
{
#if COLD_PREFETCH
    for (u32 s = 0; s < batch->count; s++) {
        const rsx_vertex_fetch_slot* a = &batch->slots[s];
        u32 ei = a->modulo ? (vi % a->frequency) : (vi / a->frequency);
        _mm_prefetch((const char*)(vm_base + cellGcmResolveLocated(a->location, a->offset + ei * a->stride)),
                     _MM_HINT_T0);
    }
#else
    (void)batch;
    (void)vi;
#endif
}

static void cold_map(u32 pass)
{
    for (u32 v = 0; v < 64; v++)
        g_cold_map[COLD_FIRST + v] = ((pass * 64u + v) * 2654435761u) & (COLD_PAGES - 1u);
}

/* Nanoseconds per vertex for cold rows, with and without a prefetch of the
 * rows of the vertex `ahead` places later. */
static double timed_cold(fetch_fn fetch, const rsx_vertex_fetch_batch* batch, rsx_vertex_value* out, u32 ahead)
{
    double best = 1e9;
    double seconds, ns;
    u32 passes = 64;
    for (u32 run = 0; run < 7; run++) {
        double started = now();
        for (u32 pass = 0; pass < passes; pass++) {
            cold_map(pass);
            for (u32 v = 0; v < 64; v++) {
                if (ahead && v + ahead < 64) prefetch_vertex(batch, v + ahead);
                fetch(batch, v, &out[v]);
                g_sink += ((const u32*)&out[v])[v & 15];
            }
        }
        seconds = now() - started;
        if (run == 0) {              /* sizes the other runs */
            passes = (u32)(6.4 / seconds) + 64;
            if (passes > 100000) passes = 100000;
            continue;
        }
        ns = seconds * 1e9 / ((double)passes * 64.0);
        if (ns < best) best = ns;
    }
    return best;
}

static void benchmark_cold(void)
{
#ifdef _WIN32
    const shape* shape = &shapes[0];
    rsx_state st;
    rsx_vertex_fetch_batch current, old;
    u8* was_base = vm_base;
    rsx_vertex_value* cached;
    g_cold = (u8*)VirtualAlloc(NULL, (SIZE_T)COLD_PAGES * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    cached = (rsx_vertex_value*)malloc(64 * sizeof(rsx_vertex_value));
    if (!g_cold || !cached) {
        printf("NOTE no memory for the cold benchmark\n");
        free(cached);
        if (g_cold) VirtualFree(g_cold, 0, MEM_RELEASE);
        g_cold = NULL;
        return;
    }
    memset(&st, 0, sizeof st);
    for (u32 i = 0; i < 16; i++)
        for (u32 k = 0; k < 4; k++) {
            float value = (float)(i + k) * 0.25f;
            memcpy(&st.vertex_data4f[i][k], &value, 4);
        }
    for (u32 i = 0; i < shape->count; i++) {
        rsx_vertex_attrib* a = &st.vertex_attribs[shape->attribs[i].slot];
        a->enabled = 1;
        a->type = shape->attribs[i].type;
        a->size = shape->attribs[i].size;
        a->stride = PAGE;            /* one guest page per vertex */
        a->offset = COLD_FIRST * PAGE + (u32)shape->attribs[i].at;
    }
    rsx_vertex_fetch_prepare(&current, &st);
    ref_vertex_fetch_prepare(&old, &st);
    for (u32 page = 0; page < COLD_PAGES; page++)      /* commits every page and writes its row */
        memcpy(g_cold + (size_t)page * PAGE, g_capture_rows[page % RSX_DIET_CAPTURE_ROWS],
               RSX_DIET_CAPTURE_ROW_BYTES);
    vm_base = g_cold;
    g_cold_mode = 1;
    printf("BENCH cold shape=%s out=cached old=%.1f new=%.1f ns/vertex | prefetch old: ahead=2 %.1f, 4 %.1f, 8 %.1f, 16 %.1f\n",
           shape->name, timed_cold(ref_fetch_vertex_prepared, &old, cached, 0),
           timed_cold(rsx_fetch_vertex_prepared, &current, cached, 0),
           timed_cold(ref_fetch_vertex_prepared, &old, cached, 2), timed_cold(ref_fetch_vertex_prepared, &old, cached, 4),
           timed_cold(ref_fetch_vertex_prepared, &old, cached, 8), timed_cold(ref_fetch_vertex_prepared, &old, cached, 16));
    g_cold_mode = 0;
    vm_base = was_base;
    free(cached);
    VirtualFree(g_cold, 0, MEM_RELEASE);
    g_cold = NULL;
#endif
}

static void benchmark(void)
{
    rsx_vertex_value* cached = (rsx_vertex_value*)malloc(64 * sizeof(rsx_vertex_value));
    rsx_vertex_value* combined = cached;
#ifdef _WIN32
    {
        static ID3D12Device* device;
        static ID3D12Resource* resource;
        D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                      D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64 * sizeof(rsx_vertex_value), 1, 1, 1,
                                     DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                     D3D12_RESOURCE_FLAG_NONE };
        D3D12_RANGE range = { 0, 0 };
        HRESULT result = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &rsx_diet_device_iid, (void**)&device);
        if (SUCCEEDED(result))
            result = device->lpVtbl->CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &rsx_diet_resource_iid, (void**)&resource);
        if (SUCCEEDED(result))
            result = resource->lpVtbl->Map(resource, 0, &range, (void**)&combined);
        if (FAILED(result)) {
            printf("NOTE no D3D12 upload heap on this system (HRESULT 0x%08lx); the second line uses cached memory\n",
                   (unsigned long)result);
            combined = cached;
        }
    }
#endif
    CHECK(cached != NULL, "malloc of the cached buffer");
    for (u32 s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        const shape* shape = &shapes[s];
        rsx_state st;
        rsx_vertex_fetch_batch current, old;
        memset(&st, 0, sizeof st);
        for (u32 i = 0; i < 16; i++)
            for (u32 k = 0; k < 4; k++) {
                float value = (float)(i + k) * 0.25f;
                memcpy(&st.vertex_data4f[i][k], &value, 4);
            }
        for (u32 i = 0; i < shape->count; i++) {
            rsx_vertex_attrib* a = &st.vertex_attribs[shape->attribs[i].slot];
            a->enabled = 1;
            a->type = shape->attribs[i].type;
            a->size = shape->attribs[i].size;
            a->stride = shape->stride;
            a->offset = 32 * PAGE + (u32)shape->attribs[i].at;
        }
        for (u32 v = 0; v < 64; v++) {
            u8 row[128];
            for (u32 b = 0; b < shape->stride; b++) row[b] = (u8)rnd();
            place(32 * PAGE + v * shape->stride, row, shape->stride);
        }
        rsx_vertex_fetch_prepare(&current, &st);
        ref_vertex_fetch_prepare(&old, &st);
        printf("BENCH shape=%s out=cached old=%.1f new=%.1f ns/vertex", shape->name,
               timed(ref_fetch_vertex_prepared, &old, cached), timed(rsx_fetch_vertex_prepared, &current, cached));
        if (combined != cached)
            printf(" | out=upload-heap old=%.1f new=%.1f",
                   timed(ref_fetch_vertex_prepared, &old, combined), timed(rsx_fetch_vertex_prepared, &current, combined));
        printf("\n");
    }
    {
        rsx_vertex_value* source = (rsx_vertex_value*)malloc(sizeof(rsx_vertex_value));
        CHECK(source != NULL, "malloc of the copy source");
        for (u32 i = 0; i < 16; i++)
            for (u32 k = 0; k < 4; k++) source[i].v[k] = (float)(i * 4 + k) * 0.5f;
        for (u32 b = 0; b < 2; b++) {
            rsx_vertex_value* out = b ? combined : cached;
            if (b && out == cached) break;
            printf("BENCH copy out=%s crt=%.1f sse=%.1f ns/copy\n", b ? "upload-heap" : "cached",
                   timed_copy(copy_crt, out, source), timed_copy(copy_sse, out, source));
        }
        free(source);
    }
    benchmark_cold();
    fflush(stdout);
}

int main(int argc, char** argv)
{
    allocate();
    for (u32 i = 0; i < HOST_PAGES * PAGE; i++) g_memory[i] = (u8)(i * 181u + (i >> 8) * 29u);
    captured();
    exhaustive();
    sweep();
    guard_page();
    printf("PASS captured draws=%u vertices=%u other draws=%llu vertices=%llu digest=%016llx\n",
           (unsigned)RSX_DIET_CAPTURE_DRAWS, (unsigned)g_capture_vertices,
           (unsigned long long)(g_draws - RSX_DIET_CAPTURE_DRAWS), (unsigned long long)g_vertices,
           (unsigned long long)g_digest);
    fflush(stdout);
    if (argc > 1 && strcmp(argv[1], "benchmark") == 0) benchmark();
    return 0;
}
