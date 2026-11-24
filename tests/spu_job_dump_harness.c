/* spu_job_dump_harness.c - drives the opt-in SPU job dump.
 *
 * Build with tests/check_spu_job_dump.py. The harness includes the production
 * definitions as spu_channels.c does, and reaches the probe hook through
 * spu_native_owned_can_run. With JD_REAL_DMA each transfer goes through the
 * production mfc_do_transfer, so the DMA hook runs at its real site.
 *
 * Usage: spu_job_dump_harness <directory> <a|b|off|bad>
 * The harness prints "spu_job_dump: PASS <mode>". The check script reads the
 * files and compares them with the scenario below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The record buffer of the dump starts dirty, as in a debug CRT build (new
 * heap memory is 0xCD there). The reader then sees whether the writer zeroes
 * every reserved byte of a record. The buffer of a slot is never freed. */
static void* jd_test_realloc(void* p, size_t n)
{
    unsigned char* base = p ? (unsigned char*)p - 16 : NULL;
    size_t old = base ? *(size_t*)base : 0;
    unsigned char* grown = (unsigned char*)malloc(n + 16);
    if (!grown) return NULL;
    *(size_t*)grown = n;
    memset(grown + 16, 0xCD, n);
    if (base) {
        memcpy(grown + 16, p, old < n ? old : n);
        free(base);
    }
    return grown + 16;
}
#define realloc jd_test_realloc
#include "spu_context.h"
#include "spu_native_owned.h"
#ifdef JD_REAL_DMA
#include "spu_dma.h"
#endif
#include "spu_job_dump.inc"
#undef realloc

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
    return 0; } } while (0)

static int s_failed;
/* REQ is CHECK for a void function: it sets the flag that main() reads. */
#define REQ(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
    s_failed = 1; } } while (0)

#define LS_SIZE 262144u
#define ENTRY 0x16270u
#define SPAN_BASE 0x4000u
#define SPAN_SIZE 0x126C0u
int g_spu_ls_probe, g_spu_pm_svc_on, g_spu_ls_watch_n;

static int s_enabled = 1, s_image = 0;
#define SELECT_WORKLOAD SPURS_PM_SELECT_WORKLOAD_LS

/* Stubs of the owned-job path. The probe hook runs before them. */
int spu_native_owned_enabled(void) { return s_enabled; }
int spu_native_owned_image(void) { return s_image; }
int spu_native_owned_observed(void) { return 0; }
int spu_pm_svc_can_run(const spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; return 0; }

#ifdef JD_REAL_DMA
/* Stubs of the externs that mfc_do_transfer names. */
uint8_t* vm_base;
uint8_t g_vm_page_bitmap[65536 / 8];
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_cri_video_dma;
void (*g_spu_put_hook)(uint32_t, uint32_t);
unsigned long long ps3_ms_now(void) { return 0; }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* bytes, uint32_t size,
                         const char* kind) { (void)ctx; (void)ea; (void)bytes; (void)size; (void)kind; }
void spu_overlay_note_get(spu_context* spu, uint32_t ea, const uint8_t* bytes, uint32_t size)
{ (void)spu; (void)ea; (void)bytes; (void)size; }
unsigned* spu_ls_watch_list(int* out_n) { *out_n = 0; return NULL; }
unsigned int mainspin_watch_n(void) { return 0; }
unsigned int mainspin_watch_at(int i) { (void)i; return 0; }
void spu_gread_note(uint32_t ea) { (void)ea; }
void spu_pm_fill_note(spu_context* ctx, uint32_t lsa, const void* bytes16, const char* src)
{ (void)ctx; (void)lsa; (void)bytes16; (void)src; }
void spu_wp_note(const spu_context* ctx, uint32_t ea, uint32_t size,
                 const void* bytes, const char* by)
{ (void)ctx; (void)ea; (void)size; (void)bytes; (void)by; }
void spu_pm_store_note(spu_context* ctx, uint32_t ea, uint32_t lsa, const char* kind)
{ (void)ctx; (void)ea; (void)lsa; (void)kind; }
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { return ea; }
#endif

/* The contexts stay alive until the exit of the process: the exit flush
 * reads the local store of the open jobs. */
static spu_context s_a, s_b, s_c, s_d;
#ifdef JD_REAL_DMA
static uint8_t s_guest[0x10000];
#define GUEST_BASE 0x01000000u
#endif

/* Guest addresses of the scenario. They lie in one 64 KiB block. */
#define EA_GET_A1   0x01000150u   /* 64 bytes */
#define EA_GET_A2   0x01000960u   /* 8 bytes */
#define EA_GET_B1   0x01004000u   /* 496 bytes */
#define EA_PUT_A1   0x01008000u   /* 16 bytes */
#define EA_PUT_A2   0x01008100u   /* 88 bytes */
#define EA_GET_B2   0x01008200u   /* 16 bytes */
#define EA_GET_D1   0x01008300u   /* 32 bytes */

#define CMD_GET 0x40u
#define CMD_PUT 0x20u

static void pattern(uint8_t* dst, size_t n, unsigned step, unsigned add)
{
    size_t i;
    for (i = 0; i < n; i++) dst[i] = (uint8_t)(i * step + add);
}

/* One transfer. `bytes` is the source of the copy: guest memory for a get,
 * local store for a put. `pc` is the address of the issuing code. */
static void dma(spu_context* ctx, uint32_t pc, uint32_t lsa, uint32_t ea, uint32_t size,
                uint32_t cmd, const uint8_t* bytes)
{
    ctx->pc = pc;
#ifdef JD_REAL_DMA
    {
        uint8_t* g = s_guest + (ea - GUEST_BASE);
        vm_base = s_guest - GUEST_BASE;
        if (cmd == CMD_GET) memcpy(g, bytes, size); else memcpy(ctx->ls + lsa, bytes, size);
#ifdef JD_DMA_HAS_TAG
        REQ(mfc_do_transfer(ctx, lsa, ea, size, cmd, 0) == 0);
#else
        REQ(mfc_do_transfer(ctx, lsa, ea, size, cmd) == 0);
#endif
        if (cmd == CMD_GET) REQ(memcmp(ctx->ls + lsa, bytes, size) == 0);
        else REQ(memcmp(g, ctx->ls + lsa, size) == 0);
    }
#else
    if (cmd == CMD_GET) memcpy(ctx->ls + lsa, bytes, size);
    spu_job_dump_dma(ctx, lsa, ea, size, cmd, cmd == CMD_GET, bytes);
#endif
}

static void probe(spu_context* ctx, uint32_t pc)
{
    ctx->pc = pc;
    (void)spu_native_owned_can_run(ctx, pc);
}

static int setup(spu_context* ctx, int spu_id, int image, uint32_t size, unsigned step,
                 unsigned add)
{
    spu_context_init(ctx, spu_id);
    ctx->policy_mode = 1;
    ctx->image_id = image;
    ctx->resident_code[0].lsa = SPAN_BASE;
    ctx->resident_code[0].size = size;
    ctx->resident_code[0].image_id = image;
    pattern(ctx->ls, LS_SIZE, step, add);
    return ctx->ls != NULL;
}

static void regs(spu_context* ctx, uint32_t r0, uint32_t r3, uint32_t r4)
{
    ctx->gpr[1]._u32[0] = 0x3F000u;
    ctx->gpr[3]._u32[0] = r3;
    ctx->gpr[4]._u32[0] = r4;
    ctx->gpr[0]._u32[0] = r0;
}

/* Mode a. Maximum of 4 jobs, every job.
 *   job 1 on context A: get, exit to a policy service, put by the policy,
 *     get, final exit, put by the policy module; closed by the next entry.
 *   job 2 on context B: one get, closed by the select-workload address.
 *   job 3 on context A: no DMA.
 *   job 4 on context B: one get. The next entry is over the maximum: the
 *     dump turns off and closes jobs 3 and 4.
 *   Context C holds image 219 at the entry address: no job. */
static int mode_a(void)
{
    static uint8_t g1[64], g2[8], g3[496], p1[16], p2[96], g4[16], g5[16];
    int r = 1;
    pattern(g1, sizeof(g1), 13, 5);
    pattern(g2, sizeof(g2), 3, 200);
    pattern(g3, sizeof(g3), 1, 77);
    pattern(p1, sizeof(p1), 17, 9);
    pattern(p2, sizeof(p2), 5, 60);
    pattern(g4, sizeof(g4), 7, 33);
    pattern(g5, sizeof(g5), 9, 12);
    r &= setup(&s_a, 3, 210, SPAN_SIZE, 7, 1);
    r &= setup(&s_b, 5, 210, SPAN_SIZE, 11, 3);
    r &= setup(&s_c, 6, 219, SPAN_SIZE, 5, 9);
    CHECK(r);

    /* Before the first job nothing is stored. */
    probe(&s_a, SELECT_WORKLOAD);
    dma(&s_a, 0x1EE4, 0x1000, EA_GET_A1, 64, CMD_GET, g1);

    /* Job 1. */
    regs(&s_a, ENTRY, 0x1340, 7);
    s_a.gpr[3]._u32[1] = 0; s_a.gpr[3]._u32[2] = 0; s_a.gpr[3]._u32[3] = 0;
    s_a.gpr[4]._u32[1] = 1; s_a.gpr[4]._u32[2] = 2; s_a.gpr[4]._u32[3] = 3;
    probe(&s_a, ENTRY);
    probe(&s_a, ENTRY);                     /* the same entry again */
    probe(&s_a, 0x4034);                    /* inside the span */
    dma(&s_a, 0x4200, 0x13C0, EA_GET_A1, 64, CMD_GET, g1);
    s_a.ls[0x16650] = 1;                    /* status */
    s_a.ls[0x16700] = 0xAA;
    regs(&s_a, 0x40B0, 0x11, 0x1F);
    probe(&s_a, 0x1648);                    /* exit 1: policy service */
    memcpy(s_a.ls + 0x1400, p1, sizeof(p1));
    dma(&s_a, 0x1F04, 0x1400, EA_PUT_A1, 16, CMD_PUT, p1);
    probe(&s_a, 0x1ABC);                    /* policy module */
    probe(&s_a, 0x9D4);                     /* not the select-workload address */
    probe(&s_a, 0x40B0);                    /* back in the span */
    dma(&s_a, 0x4FBC, 0x4F90, EA_GET_A2, 8, CMD_GET, g2);
    s_a.ls[0x16704] = 0xBB;
    regs(&s_a, 0x3068, 0x1234, 0);
    probe(&s_a, 0x3068);                    /* exit 2: end of the job */
    memcpy(s_a.ls + 0x13C0, p2, sizeof(p2));
    dma(&s_a, 0x1EE4, 0x13C0, EA_PUT_A2, 96, CMD_PUT, p2);
    probe(&s_a, 0x1ABC);

    /* Job 2 starts while job 1 is open. */
    regs(&s_b, ENTRY, 0x1340, 0);
    probe(&s_b, ENTRY);
    dma(&s_b, 0x4200, 0x13C0, EA_GET_B1, 496, CMD_GET, g3);
    regs(&s_b, 0x3068, 0x2345, 0);
    probe(&s_b, 0x3068);                    /* exit 1 */
    probe(&s_b, SELECT_WORKLOAD);           /* closes job 2 */

    /* The next entry on A closes job 1 and starts job 3. */
    regs(&s_a, ENTRY, 0x1340, 9);
    probe(&s_a, ENTRY);
    regs(&s_a, 0x3068, 0x3456, 0);
    probe(&s_a, 0x3068);                    /* exit 1 */

    /* Image 219 at the entry address: no job. */
    regs(&s_c, ENTRY, 0x1340, 0);
    probe(&s_c, ENTRY);

    /* Job 4 on B. Job 3 stays open on A. */
    regs(&s_b, ENTRY, 0x1340, 11);
    probe(&s_b, ENTRY);
    dma(&s_b, 0x4200, 0x13C0, EA_GET_B2, 16, CMD_GET, g4);

    /* The fifth entry is over the maximum. The dump turns off. */
    regs(&s_b, ENTRY, 0x1340, 13);
    probe(&s_b, ENTRY);
    probe(&s_b, 0x3068);
    dma(&s_b, 0x4200, 0x13C0, EA_GET_B2, 16, CMD_GET, g5);
    return 1;
}

/* Mode b. Maximum of 10 jobs, every second entry of a context.
 *   entry 1: job 1. entry 2: no job, but it closes job 1.
 *   entry 3: job 2, closed by the select-workload address.
 *   entry 4: no job. entry 5: job 3, open at the exit of the process. */
static int mode_b(void)
{
    static uint8_t g1[32], g2[32];
    int r = 1;
    pattern(g1, sizeof(g1), 21, 4);
    pattern(g2, sizeof(g2), 23, 6);
    r &= setup(&s_d, 7, 210, SPAN_SIZE, 3, 15);
    CHECK(r);
    regs(&s_d, ENTRY, 0x1340, 1);
    probe(&s_d, ENTRY);
    dma(&s_d, 0x4200, 0x13C0, EA_GET_D1, 32, CMD_GET, g1);
    regs(&s_d, 0x3068, 0x1111, 0);
    probe(&s_d, 0x3068);
    regs(&s_d, ENTRY, 0x1340, 2);
    probe(&s_d, ENTRY);                     /* not dumped; closes job 1 */
    dma(&s_d, 0x4200, 0x13C0, EA_GET_D1, 32, CMD_GET, g2);   /* no job is open */
    regs(&s_d, 0x3068, 0x2222, 0);
    probe(&s_d, 0x3068);
    regs(&s_d, ENTRY, 0x1340, 3);
    probe(&s_d, ENTRY);                     /* job 2 */
    probe(&s_d, ENTRY);                     /* the same entry again */
    regs(&s_d, 0x3068, 0x3333, 0);
    probe(&s_d, 0x3068);
    probe(&s_d, SELECT_WORKLOAD);
    regs(&s_d, ENTRY, 0x1340, 4);
    probe(&s_d, ENTRY);                     /* no job */
    regs(&s_d, 0x3068, 0x5555, 0);
    probe(&s_d, 0x3068);
    regs(&s_d, ENTRY, 0x1340, 5);
    probe(&s_d, ENTRY);                     /* job 3 */
    regs(&s_d, 0x3068, 0x4444, 0);
    probe(&s_d, 0x3068);
    return 1;
}

/* Mode off: no switch. Nothing is stored. */
static int mode_off(void)
{
    static uint8_t g1[64];
    pattern(g1, sizeof(g1), 13, 5);
    CHECK(setup(&s_a, 3, 210, SPAN_SIZE, 7, 1));
    regs(&s_a, ENTRY, 0x1340, 7);
    probe(&s_a, ENTRY);
    dma(&s_a, 0x4200, 0x13C0, EA_GET_A1, 64, CMD_GET, g1);
    regs(&s_a, 0x3068, 0x1234, 0);
    probe(&s_a, 0x3068);
    probe(&s_a, SELECT_WORKLOAD);
    return 1;
}

int main(int argc, char** argv)
{
    const char* dir, *mode;
    char value[2048];
    int ok;
    if (argc != 3) { fprintf(stderr, "usage: %s <directory> <a|b|off|bad>\n", argv[0]); return 2; }
    dir = argv[1];
    mode = argv[2];
    if (!strcmp(mode, "off")) {
#ifdef _WIN32
        _putenv_s("SPU_JOB_DUMP", "");
#else
        unsetenv("SPU_JOB_DUMP");
#endif
    } else {
        if (strlen(dir) + 64 >= sizeof(value)) return 2;
        if (!strcmp(mode, "bad"))
            snprintf(value, sizeof(value), "SPU_JOB_DUMP=210");
        else if (!strcmp(mode, "b"))
            snprintf(value, sizeof(value), "SPU_JOB_DUMP=210:0x16270:10:2:%s", dir);
        else
            snprintf(value, sizeof(value), "SPU_JOB_DUMP=210:0x16270:4:1:%s", dir);
#ifdef _WIN32
        _putenv(value);
#else
        {
            char* eq = strchr(value, '=');
            *eq = 0;
            setenv(value, eq + 1, 1);
        }
#endif
    }
    /* Mode bad runs the scenario of mode a: the first hook reads the switch. */
    ok = (!strcmp(mode, "a") || !strcmp(mode, "bad")) ? mode_a() : !strcmp(mode, "b") ? mode_b() : !strcmp(mode, "off") ? mode_off() : 0;
    if (!ok || s_failed) return 1;
    printf("spu_job_dump: PASS %s\n", mode);
    return 0;
}
