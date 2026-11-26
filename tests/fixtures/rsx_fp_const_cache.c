/* Inline constants of a fragment program: the draw record of before the cache (the old
 * path) against the production path. Built by tests/check_rsx_fp_const_cache.py. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_fp_decompiler.h"
#include "rsx_fp_snapshot.h"

/* What the production record and the snapshot block need from the backend. */
#define VP_FPCB_STRIDE 2304
#define VP_FPCB_MAX_CONSTS ((VP_FPCB_STRIDE / 16) - 1)
#define MAX_DRAWS 64
typedef struct {
    u32 fp_addr, fp_ctrl, alpha_ctl, alpha_ref_raw, fp_const_n, fp_snap;
    u32 rt_fmt;
} D3D12DrawRecord;
static struct { void* vp_fpcb_mapped; int vp_parity; } s_d3d;
uint8_t* vm_base;
static u8 g_guest[1u << 20];
u32 cellGcmResolveLocated(int local, u32 offset)
{
    (void)local;
    return offset < sizeof(g_guest) - 16384u ? offset : 0xFFFFFFFFu;
}
static const char* rsx_env_once(const char* name) { return getenv(name); }
extern int g_collects;

/* PRODUCTION_SNAP */

static const u8* vp_fp_snap_bytes(u32 snap, u32 off)
{
    const VPFP_snap* s = vp_fp_snap_entry(snap, off);
    return s ? s->bytes : NULL;
}

/* OLD_PATH */

/* PRODUCTION_RECORD */

static u8 g_slots[2][MAX_DRAWS * VP_FPCB_STRIDE];
static unsigned checks, failures;

static void check(int ok, const char* what)
{
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", what); }
}

static void word(u8* p, u32 w)
{
    w = (w << 16) | (w >> 16);        /* guest order: big endian, half-words swapped */
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
}

/* A program of `count` instructions; each instruction has one inline constant.
 * `end` 0 leaves the END word out: the program is malformed. */
static void program(u32 address, unsigned count, u32 base, int end)
{
    u8* p = g_guest + address;
    memset(p, 0, count * 32u + 16u);
    for (unsigned i = 0; i < count; i++) {
        word(p, 0);
        word(p + 4, 2);               /* source register type CONST */
        word(p + 8, 0);
        word(p + 12, 0);
        for (unsigned c = 0; c < 4; c++) word(p + 16 + c * 4, base + i * 4u + c);
        p += 32;
    }
    if (end) word(p, 1);              /* FP_END */
}

static void edit_constant(u32 address, unsigned index, u32 value)
{
    word(g_guest + address + index * 32u + 16, value);
}

/* One draw through both paths. The old path writes slot 0, the production path slot 1.
 * `expect_collect` is the number of collections the production path may make. */
static void draw(u32 address, u32 snap, u32 alpha_raw, u32 rt_fmt,
                 unsigned expect_collect, u32 expect_n, const char* what)
{
    D3D12DrawRecord a, b;
    memset(&a, 0, sizeof a);
    a.fp_addr = address; a.fp_snap = snap; a.alpha_ref_raw = alpha_raw; a.rt_fmt = rt_fmt;
    b = a;
    memset(g_slots[0], 0xA5, VP_FPCB_STRIDE);
    memset(g_slots[1], 0xA5, VP_FPCB_STRIDE);
    s_d3d.vp_fpcb_mapped = g_slots[0];
    old_record_fp_b1(0, &a);
    s_d3d.vp_fpcb_mapped = g_slots[1];
    const unsigned before = (unsigned)g_collects;
    vp_record_fp_b1(0, &b);
    const unsigned used = (unsigned)g_collects - before;
    const u32 n = b.fp_const_n == 0xFFFFu ? 0u : b.fp_const_n;
    float ref = rsx_fp_alpha_ref(alpha_raw, rt_fmt);
    char text[160];
    snprintf(text, sizeof text, "%s: count old=%u new=%u expected=%u", what, a.fp_const_n, b.fp_const_n, expect_n);
    check(a.fp_const_n == b.fp_const_n && b.fp_const_n == expect_n, text);
    if (memcmp(g_slots[0], g_slots[1], VP_FPCB_STRIDE)) {
        const u32* x = (const u32*)g_slots[0];
        const u32* y = (const u32*)g_slots[1];
        unsigned i = 0;
        while (x[i] == y[i]) i++;
        snprintf(text, sizeof text, "%s: slot word %u old=0x%08X new=0x%08X", what, i, x[i], y[i]);
        check(0, text);
    }
    snprintf(text, sizeof text, "%s: collector calls new=%u expected=%u", what, used, expect_collect);
    check(used == expect_collect, text);
    snprintf(text, sizeof text, "%s: alpha reference of this draw", what);
    check(memcmp(g_slots[1] + (n ? n : 1u) * 16u, &ref, 4) == 0, text);
}

int main(void)
{
    vm_base = g_guest;
    const u32 A = 0x1000, B = 0x8000, C = 0x10000, D = 0x18000, E = 0x20000;
    const u32 F = 0x28000, Z = 0x30000, G = 0x38000, H = 0x40000;
    program(A, 3, 0x3F800000, 1);     /* three constants */
    program(B, 20, 0x40000000, 1);    /* the size of the bloom blur weights */
    program(C, 143, 0x3E000000, 1);   /* exactly the slot bound */
    program(D, 144, 0x3E000001, 1);   /* one constant above the slot bound */
    program(E, 200, 0x3E000002, 1);
    program(F, 0, 0, 1);              /* no constant: zero is a result */
    program(Z, 5, 0x3E000003, 0);     /* no END word */
    program(G, 2, 0x7FC00000, 1);     /* NaN words: both paths must sanitize them */
    program(H, 3, 0x3F000000, 1);

    const u32 sa = vp_fp_snap_for(A);
    draw(A, sa, 0x80, 8, 1, 3, "first draw of a snapshot collects");
    draw(A, sa, 0x40, 8, 0, 3, "second draw reuses the result");
    draw(A, sa, 0x40, 14, 0, 3, "alpha reference follows the draw (fp16 format)");
    draw(A, sa, 0x40, 15, 0, 3, "alpha reference follows the draw (float format)");

    draw(A, 0, 0x80, 8, 1, 3, "draw without a snapshot reads live memory");
    edit_constant(A, 1, 0x42C80000);
    draw(A, 0, 0x80, 8, 1, 3, "draw without a snapshot sees an in-place edit");

    /* The edit keeps the hash: the snapshot is refreshed into a new ring slot. */
    const u32 sa2 = vp_fp_snap_for(A);
    check(sa2 != sa, "an in-place constant edit takes a new snapshot slot");
    draw(A, sa2, 0x80, 8, 1, 3, "refreshed snapshot collects again");
    draw(A, sa2, 0x80, 8, 0, 3, "refreshed snapshot is cached");

    /* The ring comes back to a cached slot: same address, same hash, new bytes. */
    s_fp_snap_head = sa2 - 1;
    edit_constant(A, 2, 0x42480000);
    const u32 sa3 = vp_fp_snap_for(A);
    check(sa3 == sa2, "the ring slot is reused");
    draw(A, sa3, 0x80, 8, 1, 3, "reused slot never returns the values of its last program");
    draw(A, sa3, 0x80, 8, 0, 3, "and caches the new program");

    /* The ring comes back to the slot of the first snapshot, for another address. */
    s_fp_snap_head = sa - 1;
    const u32 sb = vp_fp_snap_for(B);
    check(sb == sa, "the first ring slot is reused by another program");
    draw(B, sb, 0x80, 8, 1, 20, "reused slot, other program");
    /* A record that still names that slot for address A: the slot does not verify. */
    draw(A, sb, 0x80, 8, 1, 3, "record with a snapshot of another program reads live memory");
    draw(A, sa3, 0x80, 8, 0, 3, "two snapshots keep their own results");

    const u32 sc = vp_fp_snap_for(C);
    draw(C, sc, 0x80, 8, 1, 143, "slot bound");
    draw(C, sc, 0x10, 8, 0, 143, "slot bound is cached");

    const u32 sd = vp_fp_snap_for(D);
    draw(D, sd, 0x80, 8, 1, 0xFFFF, "overflow refuses the slot");
    draw(D, sd, 0x80, 8, 0, 0xFFFF, "overflow result stays with its snapshot");
    const u32 se = vp_fp_snap_for(E);
    draw(E, se, 0x80, 8, 1, 0xFFFF, "overflow far above the bound");
    draw(E, se, 0x20, 8, 0, 0xFFFF, "overflow far above the bound is cached");

    const u32 sf = vp_fp_snap_for(F);
    draw(F, sf, 0x80, 8, 1, 0, "program without constants");
    draw(F, sf, 0x20, 15, 0, 0, "program without constants is cached");

    check(vp_fp_snap_for(Z) == 0, "a program without END takes no snapshot");
    draw(Z, 0, 0x80, 8, 1, 0, "malformed program, first draw");
    draw(Z, 0, 0x40, 8, 1, 0, "malformed program, second draw");

    const u32 sg = vp_fp_snap_for(G);
    draw(G, sg, 0x80, 8, 1, 2, "non-finite constant words");
    draw(G, sg, 0x80, 8, 0, 2, "non-finite constant words are cached");
    const u32 sh = vp_fp_snap_for(H);
    draw(H, sh, 0x80, 8, 1, 3, "a later snapshot collects");

    printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
