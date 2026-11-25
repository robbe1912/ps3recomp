/* SPU_LANE_TRACE and the two LS accessors, on the real runtime files. No game
 * data and no runtime mock. The lifted bodies come from the lifter
 * (spu_lane_trace_call.h) and use the call, drain and return ABI of generated
 * C. The runner starts one process per environment, because the gate reads the
 * environment once.
 *
 * This file is self-contained. The SPU unit tests that include
 * spu_interpreter_handoff.c get an abort boundary named spurs_pm_build_context
 * with four arguments from it. The taskset path here calls the five-argument
 * form and needs it to return.
 *
 * Modes:
 *   edge  lqd and stqd at the mask edges: 0, 0x3FFF0, a value above the mask
 *         and an unaligned value.
 *   poll  one policy module poll.
 *   lane  the lane hot paths: one policy poll, three policy DMAs, one sync
 *         dispatch, two async dispatches (with and without a taskset) and one
 *         semaphore wait. The runner sets LBP_SPU_SERIAL=1: the async thread
 *         holds the serial lock from before its entry until after its RETURNED
 *         line. That lock is the barrier of the test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_helpers.h"
#include "spu_workload.h"
#include "spu_task_lifecycle.h"
#include "sys_semaphore.h"
#ifndef _WIN32
#include <time.h>
#endif

#define LANE_CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(2); } } while (0)

extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);
extern void spu_serial_acquire(void);
extern void spu_serial_release(void);
extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_polls;
extern int g_spu_ls_gate, g_spu_lane_trace;
extern void spu_wrch(spu_context*, uint32_t, u128);   /* channel ABI of lifted code: no header declares it */
#include "spu_lane_trace_call.h"
#ifdef SPU_LANE_TRACE_GAME   /* the game runtime alone has the [modspan-w] span and the wait profile */
extern uint32_t g_ms_last_lsa, g_ms_last_size;
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;

#define GUEST_SIZE 0x40000u
#define DMA_EA     0x20000u   /* three GETs start here: 0x80 bytes, 0x80 bytes, 0 bytes */
#define TASKSET_EA 0x30000u   /* an authored taskset: task 0 only */
#define LANE_IMAGE 77
#define LANE_STORE 0xABCDEF01u
static const uint32_t elf_marker[4] = {0xDEADBEEFu, 0x0BADF00Du, 0x13572468u, 0x2468ACE0u};
static const uint32_t taskset_arg[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};

/* ---- Reference values: big-endian bytes at the address after the mask. ---- */
static unsigned char pattern(uint32_t i) { return (unsigned char)(i * 7u + 3u); }
static uint32_t pattern_word(uint32_t addr)
{
    uint32_t w = 0;
    for (int k = 0; k < 4; k++) w = (w << 8) | pattern(addr + (uint32_t)k);
    return w;
}
static u128 expect16(uint32_t addr)
{
    u128 v;
    addr &= 0x3FFF0u;
    for (int i = 0; i < 4; i++) v._u32[i] = pattern_word(addr + 4u * (uint32_t)i);
    return v;
}
static u128 splat(uint32_t w) { u128 v; v._u32[0] = v._u32[1] = v._u32[2] = v._u32[3] = w; return v; }
static u128 words(const uint32_t w[4]) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = w[i]; return v; }
static int same(u128 a, u128 b)
{
    return a._u32[0] == b._u32[0] && a._u32[1] == b._u32[1] &&
           a._u32[2] == b._u32[2] && a._u32[3] == b._u32[3];
}
static int quad_is(const unsigned char* p, uint32_t w)
{
    return p[0] == (unsigned char)(w >> 24) && p[1] == (unsigned char)(w >> 16) &&
           p[2] == (unsigned char)(w >> 8) && p[3] == (unsigned char)w;
}
static int word_is(const unsigned char* p, uint32_t w) { return quad_is(p, w); }
static unsigned char guest_byte(uint32_t i) { return (unsigned char)(i * 13u + 11u); }
static void put_be32(unsigned char* p, uint32_t w) { p[0] = (unsigned char)(w >> 24); p[1] = (unsigned char)(w >> 16); p[2] = (unsigned char)(w >> 8); p[3] = (unsigned char)w; }
static void put_be16(unsigned char* p, uint32_t w) { p[0] = (unsigned char)(w >> 8); p[1] = (unsigned char)w; }
static void sleep_1ms(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec t = {0, 1000000L};
    nanosleep(&t, NULL);
#endif
}

/* ---- The context of the edge job, the poll job and the DMA job. ---- */
static spu_context ctx;
static void finish(spu_context* c)
{
    c->gpr[11]._u32[0]++;
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}

/* ---- The workload job: image LANE_IMAGE, dispatched three ways. ---- */
static unsigned char lane_elf[0x64];
static volatile int lane_entered;      /* the entry ran on its own thread */
static int lane_runs;
static uint32_t lane_r3[3][4];
static int context_builds;
static void lane_exit(spu_context* c)
{
    int n = lane_runs++;
    LANE_CHECK(n < 3);
    for (int i = 0; i < 4; i++) lane_r3[n][i] = c->gpr[3]._u32[i];
    /* The loader put the ELF marker at LS 0x30. The job read it, stored over
     * it and read the store back. */
    LANE_CHECK(same(c->gpr[12], words(elf_marker)) && c->gpr[10]._u32[0] == 0x1234u);
    LANE_CHECK(same(c->gpr[13], splat(LANE_STORE)));
    LANE_CHECK(quad_is(c->ls + 0x30, LANE_STORE) && quad_is(c->ls + 0x34, LANE_STORE) &&
               quad_is(c->ls + 0x38, LANE_STORE) && quad_is(c->ls + 0x3C, LANE_STORE));
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    lane_entered = n + 1;              /* before the halt: the halt does not return */
    spu_halt(c);
}
static void make_elf(void)
{
    memset(lane_elf, 0, sizeof lane_elf);
    memcpy(lane_elf, "\x7F" "ELF", 4);
    lane_elf[4] = 1; lane_elf[5] = 2;              /* ELFCLASS32, ELFDATA2MSB */
    put_be32(lane_elf + 0x18, 0x240);              /* e_entry */
    put_be32(lane_elf + 0x1C, 0x34);               /* e_phoff: one program header */
    put_be16(lane_elf + 0x2A, 0x20);               /* e_phentsize */
    put_be16(lane_elf + 0x2C, 1);                  /* e_phnum */
    put_be32(lane_elf + 0x34, 1);                  /* p_type: PT_LOAD */
    put_be32(lane_elf + 0x38, 0x54);               /* p_offset */
    put_be32(lane_elf + 0x3C, 0x30);               /* p_vaddr: LS 0x30 */
    put_be32(lane_elf + 0x44, 0x10);               /* p_filesz */
    put_be32(lane_elf + 0x48, 0x10);               /* p_memsz */
    for (int i = 0; i < 4; i++) put_be32(lane_elf + 0x54 + 4 * i, elf_marker[i]);
}

/* ---- Authored stand-in for libs/spurs/spurs_pm.c. The sentinel 0xA70 at LS
 * 0x27C4 makes the task entry read its argument from the quad at LS 0x2780. */
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t taskid, uint32_t a, uint32_t b)
{
    (void)a; (void)b;
    LANE_CHECK(taskset_ea == TASKSET_EA && taskid == 0);
    for (int i = 0; i < 4; i++) put_be32(ls + 0x2780 + 4 * i, taskset_arg[i]);
    put_be32(ls + 0x27C4, 0xA70u);
    context_builds++;
    return 0;
}

/* ---- Host guest memory of the task lifecycle: one byte store. ---- */
void vm_write8(uint64_t addr, uint8_t v)
{
    LANE_CHECK(addr < GUEST_SIZE);
    vm_base[addr] = v;
}

/* ---- Boundaries of runtime/syscalls/sys_semaphore.c. The two hooks are
 * no-ops here. A wait that takes a token at once does not call the others. */
static void unexpected(const char* what)
{
    fprintf(stderr, "FAIL unexpected call: %s\n", what);
    abort();
}
static const char* wait_begin_kind;
void ps3_spu_job_complete_pending(void) {}
void ppu_thread_wait_begin(ppu_context* c, const char* kind, uint64_t a, uint64_t b) { (void)c; (void)a; (void)b; wait_begin_kind = kind; }
void lbp_breadcrumb_dump(const char* tag) { (void)tag; unexpected("lbp_breadcrumb_dump"); }
void ppu_log_host_chain(const char* tag) { (void)tag; unexpected("ppu_log_host_chain"); }
int64_t lv2_usec_deadline(uint64_t usec) { (void)usec; unexpected("lv2_usec_deadline"); abort(); }
int lv2_deadline_passed(int64_t deadline) { (void)deadline; unexpected("lv2_deadline_passed"); abort(); }

/* ---- Link-only boundaries of the SPU runtime outside this fixture. Fail if
 * any is reached. The fixture runs its own contexts: no LV2 SPU thread, no
 * DMA watch query. The alias fixup is the identity rule of the vendor
 * runtime/syscalls/spu_ea_alias.c: an EA below 0x45000000 maps to itself. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; unexpected("spu_thread_publish_ctx"); abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; unexpected("ppu_resv_break_store"); abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; unexpected("ps3_ww_report_inline"); abort(); }
unsigned long long ps3_ms_now(void) { unexpected("ps3_ms_now"); abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; unexpected("spurs_ef_set_from_spu"); abort(); }
void ydkj_wake_all_event_flags(void) { unexpected("ydkj_wake_all_event_flags"); abort(); }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t spu_thread_register_phantom(void) { unexpected("spu_thread_register_phantom"); abort(); }
unsigned int mainspin_watch_n(void) { unexpected("mainspin_watch_n"); abort(); }
unsigned int mainspin_watch_at(int i) { (void)i; unexpected("mainspin_watch_at"); abort(); }
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { LANE_CHECK(ea < 0x45000000u); return ea; }

static int armed_probes(void)
{
    const char* trace = getenv("SPU_LANE_TRACE");          /* set, non-empty and not "0" is on */
    return getenv("SPU_LS_LOWREAD") || getenv("SPU_SMC_WATCH") ||
           getenv("SPU_LS_WATCH") || (trace && *trace && strcmp(trace, "0"));
}

static int run_edge(void)
{
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.status = SPU_STATUS_RUNNING;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) ctx.ls[i] = pattern(i);
    spu_begin_image(77);
    spu_register_function(0x240, finish);
    LANE_CHECK(g_spu_ls_gate == -1 && g_spu_lane_trace == -1);
    int halted = spu_run_with_halt(lifted_edge_job, &ctx);
    LANE_CHECK(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    LANE_CHECK(ctx.gpr[11]._u32[0] == 1);                  /* the job returned to 0x240 */
    LANE_CHECK(same(ctx.gpr[3], expect16(0x0u)));          /* 0 */
    LANE_CHECK(same(ctx.gpr[4], expect16(0x40000u)));      /* above the mask: wraps to 0 */
    LANE_CHECK(same(ctx.gpr[5], expect16(0x3FFF0u)));      /* the last quadword */
    LANE_CHECK(same(ctx.gpr[6], expect16(0x1230u)));       /* 0x1235: unaligned, rounds down */
    LANE_CHECK(same(ctx.gpr[10], splat(0x11223344u)) && same(ctx.gpr[12], splat(0x55667788u)));
    /* 0x40035 is stored at 0x30; 0x123A is stored at 0x1230. No other byte changes. */
    unsigned changed = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
        uint32_t w = i < 0x40u ? 0x11223344u : 0x55667788u;
        int in_store = (i >= 0x30u && i < 0x40u) || (i >= 0x1230u && i < 0x1240u);
        if (ctx.ls[i] != (unsigned char)(in_store ? (w >> (8 * (3 - (i & 3)))) : pattern(i))) {
            fprintf(stderr, "diff at 0x%05X: got %02X\n", i, ctx.ls[i]);
            LANE_CHECK(0);
        }
        changed += in_store;
    }
    LANE_CHECK(changed == 32);
    return 1;
}

static void run_poll_job(void)
{
    spu_context_init(&ctx, 0);
    ctx.image_id = 2;
    ctx.policy_mode = 1;
    ctx.status = SPU_STATUS_RUNNING;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) ctx.ls[i] = pattern(i);
    spu_begin_image(2);
    spu_register_function(0x240, finish);
    int halted = spu_run_with_halt(lifted_poll_job, &ctx);
    LANE_CHECK(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.gpr[11]._u32[0] == 1);
    LANE_CHECK(g_spurs_pm_polls == 1);                     /* cellSpursModulePoll ran once */
    LANE_CHECK(same(ctx.gpr[9], expect16(0xDF0u)));
    LANE_CHECK(same(ctx.gpr[10], splat(0x5A5Au)));         /* il sign-extends 0x5A5A */
    /* The one store of the job is the 16 bytes at 0x1320. */
    unsigned changed = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
        int in_store = i >= 0x1320u && i < 0x1330u;
        unsigned char want = (unsigned char)(in_store ? (((i & 3) < 2) ? 0x00 : 0x5A) : pattern(i));
        if (ctx.ls[i] != want) {
            fprintf(stderr, "diff at 0x%05X: got %02X want %02X\n", i, ctx.ls[i], want);
            LANE_CHECK(0);
        }
        changed += in_store;
    }
    LANE_CHECK(changed == 16);
}

static void run_dma_job(void)
{
    spu_context_init(&ctx, 0);
    ctx.image_id = 2;
    ctx.policy_mode = 1;
    ctx.status = SPU_STATUS_RUNNING;
    spu_begin_image(2);
    spu_register_function(0x240, finish);
#ifdef SPU_LANE_TRACE_GAME
    g_ms_last_lsa = 0x1000;            /* the second GET lands in the last module span */
    g_ms_last_size = 0x800;
#endif
    int halted = spu_run_with_halt(lifted_dma_job, &ctx);
#ifdef SPU_LANE_TRACE_GAME
    g_ms_last_lsa = g_ms_last_size = 0;
#endif
    LANE_CHECK(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.gpr[11]._u32[0] == 1);
    for (uint32_t i = 0; i < 0x80; i++) {
        LANE_CHECK(ctx.ls[0x4000 + i] == guest_byte(DMA_EA + i));             /* GET of 0x80 bytes */
        LANE_CHECK(ctx.ls[0x1000 + i] == guest_byte(DMA_EA + 0x100 + i));     /* GET of 0x80 bytes */
    }
    LANE_CHECK(word_is(ctx.ls + 0x2000, 0) && word_is(ctx.ls + 0x200C, 0));   /* the size-0 GET moved nothing */
}

static void wait_lane(int runs)
{
    int waited = 0;
    while (lane_entered < runs && waited++ < 30000) sleep_1ms();
    LANE_CHECK(lane_entered == runs);
    spu_serial_acquire();       /* the async thread holds the lock until after its RETURNED line */
    spu_serial_release();
}

static void wait_taskset_retired(void)
{
    int waited = 0, retired = 0;
    while (waited++ < 30000 && !retired) {
        spu_task_lifecycle_lock();
        const uint8_t* running = spu_task_running_locked(TASKSET_EA);
        retired = running && !(running[0] & 0x80u);
        spu_task_lifecycle_unlock();
        if (!retired) sleep_1ms();
    }
    LANE_CHECK(retired);
    LANE_CHECK(vm_base[TASKSET_EA] == 0 && vm_base[TASKSET_EA + 0x30] == 0x80);   /* not running, still enabled */
}

static void run_semaphore(void)
{
    static ppu_context ppu;
    memset(&ppu, 0, sizeof ppu);
    ppu.gpr[3] = 0; ppu.gpr[4] = 0; ppu.gpr[5] = 1; ppu.gpr[6] = 1;   /* no id out, no attr, 1 token, max 1 */
    LANE_CHECK(sys_semaphore_create(&ppu) == 0);
    LANE_CHECK(g_sys_semaphores[0].active && g_sys_semaphores[0].value == 1);
    ppu.gpr[3] = 1; ppu.gpr[4] = 0;                                    /* semaphore 1, no timeout */
    LANE_CHECK(sys_semaphore_wait(&ppu) == 0);
    LANE_CHECK(g_sys_semaphores[0].value == 0);
}

static int run_lane(void)
{
    LANE_CHECK(getenv("LBP_SPU_SERIAL"));            /* the barrier needs the serial lock */
    /* 1. The policy module polls once. No guest memory yet: the poll needs none. */
    run_poll_job();
    /* 2. Guest memory: three policy DMAs. */
    vm_base = calloc(1, GUEST_SIZE);
    LANE_CHECK(vm_base);
    ppu_vm_size = GUEST_SIZE;
    for (uint32_t i = DMA_EA; i < DMA_EA + 0x180; i++) vm_base[i] = guest_byte(i);
    vm_base[TASKSET_EA + 0x30] = 0x80;                /* task 0 of the taskset is enabled */
    run_dma_job();
    /* 3. The workload job, three ways. */
    make_elf();
    spu_workload_register_img(spu_workload_fingerprint(lane_elf, sizeof lane_elf), lifted_lane_job, LANE_IMAGE, "lane");
    spu_begin_image(LANE_IMAGE);
    spu_register_function(0x240, lane_exit);
    LANE_CHECK(spu_workload_dispatch(lane_elf, sizeof lane_elf, 0) == 1);
    LANE_CHECK(lane_runs == 1 && lane_r3[0][0] == 0 && lane_r3[0][1] == 0 && lane_r3[0][2] == 0 && lane_r3[0][3] == 0);
    LANE_CHECK(spu_workload_dispatch_owned(lane_elf, sizeof lane_elf, 0, NULL, NULL) == 1);   /* async, no owner */
    wait_lane(2);
    LANE_CHECK(lane_runs == 2 && lane_r3[1][0] == 0x00400000u && lane_r3[1][1] == 0 && lane_r3[1][2] == 0 && lane_r3[1][3] == 0);
    LANE_CHECK(context_builds == 0);
    LANE_CHECK(spu_workload_dispatch_task(lane_elf, sizeof lane_elf, 0, NULL, TASKSET_EA, 0) == 1);
    wait_lane(3);
    wait_taskset_retired();
    LANE_CHECK(lane_runs == 3 && same(words(lane_r3[2]), words(taskset_arg)) && context_builds == 1);
    /* 4. One semaphore wait on the PPU side. */
    run_semaphore();
#ifdef SPU_LANE_TRACE_GAME
    LANE_CHECK(wait_begin_kind && !strcmp(wait_begin_kind, "semaphore_wait"));
#endif
    free(vm_base);
    vm_base = NULL;
    ppu_vm_size = 0;
    printf("lane: gate=%d trace=%d runs=%d polls=%u\n", g_spu_ls_gate, g_spu_lane_trace, lane_runs, (unsigned)g_spurs_pm_polls);
    return 1;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    LANE_CHECK(argc == 2);
    const char* mode = argv[1];
    LANE_CHECK(!strcmp(mode, "edge") || !strcmp(mode, "poll") || !strcmp(mode, "lane"));
    if (!strcmp(mode, "edge")) run_edge();
    else if (!strcmp(mode, "poll")) run_poll_job();
    else run_lane();
    /* The gate read the environment once, on the first accessor call. */
    LANE_CHECK((g_spu_ls_gate != 0) == (armed_probes() != 0));
    if (!armed_probes())
        LANE_CHECK(g_spu_ls_gate == 0 && g_spu_lane_trace == 0 && g_spu_ls_watch_n == 0 &&
                   g_spu_ls_probe == 0 && g_spu_smc_watch == 0);
    puts("PASS");
    return 0;
}
