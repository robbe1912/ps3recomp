/* SPU_NATIVE_OWNED_MISS_LOG: the counter of the dispatches that send owned
 * code to the interpreter, on the real runtime files. No lifted body comes
 * from the lifter: every scenario runs interpreted code.
 *
 * One process per mode, because the switch reads the environment once.
 *   unit   the counter vectors and the parity of the stop reasons.
 *   smc    a stop state (SPU_SMC_WATCH): one line "gate stop smc watch".
 *   owner  no code owner: one line "gate stop no code owner".
 *   entry  a target without a lifted entry: "no lifted entry 0x04000".
 *   lanes  the smc scenario on two threads: lane=0, then lane=1. */
#include "spu_helpers.h"
#include "spu_workload.h"
#include "spu_native_owned.h"
/* The fork runner defines SPU_INTERP_COUNT_STUBS: that build links a part of
 * the runtime, and spu_interp_count_stubs.h holds the other boundaries. */
#ifdef SPU_INTERP_COUNT_STUBS
#include "spu_interp_count_stubs.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win32_compat.h"
#else
#include <pthread.h>
#endif

#define MISS_CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(2); } } while (0)

extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_indirect_branch(spu_context*);
extern int g_spu_smc_watch;
extern int g_spu_ls_probe;

static void forbidden(spu_context* c) { (void)c; fputs("FORBIDDEN lifted code\n", stderr); exit(2); }

static spu_miss_state s;
static char line[SPU_MISS_LINE];
static int count(int lane, int image, int cause, uint32_t pc, unsigned long long now_ms)
{
    line[0] = 0;
    return spu_miss_count(&s, lane, image, cause, pc, now_ms, line);
}
/* One call of the counter. expected is the line it must write; "" for none. */
static void vector(int lane, int image, int cause, uint32_t pc, unsigned long long now_ms,
                   const char* expected)
{
    line[0] = 0;
    const int result = count(lane, image, cause, pc, now_ms);
    MISS_CHECK(result == !!expected[0]);
    if (strcmp(line, expected)) fprintf(stderr, "line: %s\nwant: %s\n", line, expected);
    MISS_CHECK(!strcmp(line, expected));
}
#define NOE SPU_MISS_NO_ENTRY
static void unit(void)
{
    /* one miss prints at once; the counters start at zero after the line */
    s = (spu_miss_state){0};
    vector(3, 77, SPU_STOP_SMC_WATCH, 0x4000, 5000, "[spu-miss] lane=3 img=77 gate stop smc watch=1");
    vector(3, 77, SPU_STOP_SMC_WATCH, 0x4000, 5999, "");
    vector(3, 77, SPU_STOP_SMC_WATCH, 0x4000, 6000, "[spu-miss] lane=3 img=77 gate stop smc watch=2");
    /* the image id, the cause and the pc of a missing entry select the counter */
    s = (spu_miss_state){0};
    vector(0, 5, NOE, 0x100, 5000, "[spu-miss] lane=0 img=5 no lifted entry 0x00100=1");
    vector(0, 5, NOE, 0x200, 5010, "");
    vector(0, 5, NOE, 0x100, 5020, "");
    vector(0, 9, SPU_STOP_IRQ_FRAME, 0x100, 5030, "");
    vector(0, 9, SPU_STOP_INT_PENDING, 0x100, 5040, "");
    vector(0, 5, NOE, 0x100, 6000,
           "[spu-miss] lane=0 img=5 no lifted entry 0x00100=2 no lifted entry 0x00200=1 | img=9 gate stop irq frame=1 gate stop interrupt pending=1");
    /* a full pc table keeps counting in its last row */
    for (unsigned i = 0; i < SPU_MISS_PCS + 4; ++i) vector(0, 5, NOE, 0x1000 + 4 * i, 6010 + i, "");
    vector(0, 5, NOE, 0x100, 7000, "[spu-miss] lane=0 img=5 no lifted entry 0x00100=1"
           " no lifted entry 0x01000=1 no lifted entry 0x01004=1 no lifted entry 0x01008=1 no lifted entry 0x0100C=1"
           " no lifted entry 0x01010=1 no lifted entry 0x01014=1 no lifted entry 0x01018=1 no lifted entry 0x0101C=1"
           " no lifted entry 0x01020=1 no lifted entry 0x01024=1 no lifted entry 0x01028=1 no lifted entry 0x0102C=1"
           " no lifted entry 0x01030=1 no lifted entry 0xFFFFFFFF=7");
    /* a second lane state is independent */
    s = (spu_miss_state){0};
    vector(7, -1, SPU_STOP_NO_IMAGE, 0, 5000, "[spu-miss] lane=7 img=-1 gate stop no image=1");
    vector(7, -1, SPU_STOP_PC_RANGE, 0x40000, 5100, "");
    vector(7, 4, SPU_STOP_OBSERVED, 0, 5200, "");
    vector(7, -1, SPU_STOP_NO_IMAGE, 0, 6000,
           "[spu-miss] lane=7 img=-1 gate stop no image=1 gate stop pc range=1 | img=4 gate stop observed=1");
    spu_miss_reset();
    MISS_CHECK(g_spu_miss.started == 0 && g_spu_miss.nimage == 0);
    /* the stop reasons are the stop list of spu_native_owned_run_ok */
    spu_context c;
    spu_context_init(&c, 0);
    c.image_id = 77; c.policy_mode = 1; c.pc = 0x4000;
    c.resident_code[0].lsa = 0x4000; c.resident_code[0].size = 0x14; c.resident_code[0].image_id = 25;
    const int probe = g_spu_ls_probe, smc = g_spu_smc_watch;
    static const int expected[12] = { 0, SPU_STOP_SMC_WATCH, SPU_STOP_LS_LOWREAD, SPU_STOP_LS_WATCH,
        SPU_STOP_IRQ_FRAME, SPU_STOP_IRQ_SAVED, SPU_STOP_INT_PENDING, SPU_STOP_NO_IMAGE, SPU_STOP_NO_LS,
        SPU_STOP_PC_ALIGN, SPU_STOP_NO_OWNER, SPU_STOP_NO_OWNER };
    for (int k = 0; k < 12; ++k) {
        uint8_t* ls = c.ls; int image = c.image_id;
        g_spu_smc_watch = 0; g_spu_ls_probe = 0; g_spu_ls_watch_n = 0;
        c.irq_frame = 0; c.irq_saved = 0; c.int_enable = 0; c.image_id = 77; c.ls = ls;
        c.resident_code[0].image_id = 25; c.resident_code[0].lsa = 0x4000; c.resident_code[0].size = 0x14;
        uint32_t pc = 0x4000;
        switch (k) {
        case 1: g_spu_smc_watch = 1; break;
        case 2: g_spu_ls_probe = 1; break;
        case 3: g_spu_ls_watch_n = 1; break;
        case 4: c.irq_frame = (void*)1; break;
        case 5: c.irq_saved = 1; break;
        case 6: c.int_enable = 1; c.event_status = 1; c.event_mask = 1; break;
        case 7: c.image_id = -1; break;
        case 8: c.ls = (uint8_t*)0; break;
        case 9: pc = 0x4001; break;
        case 10: c.resident_code[0].size = 4; pc = 0x4010; break;
        case 11: c.resident_code[0].image_id = 0; break;
        default: break;
        }
        const int reason = spu_native_owned_stop_reason(&c, pc);
        MISS_CHECK((reason != 0) == !spu_native_owned_run_ok(&c, pc));
        MISS_CHECK(reason == expected[k]);
        c.ls = ls; c.image_id = image;
    }
    g_spu_smc_watch = smc; g_spu_ls_probe = probe;
    /* the switch word: the miss path calls the counter only when it is on */
    spu_miss_reset();
    MISS_CHECK(g_spu_miss.started == 0);
    puts("unit PASS");
}

/* A policy lane with one resident owner for 0x4000. The code there is
 * "ai r3,r3,3" then a halt word. */
static spu_context lane;
static void scenario_context(int owner)
{
    spu_context_init(&lane, 0); lane.image_id = 77; lane.policy_mode = 1;
    lane.pc = 0x4000; lane.status = SPU_STATUS_RUNNING;
    uint8_t* p = lane.ls + 0x4000; const uint32_t w = 0x1C00C183;
    p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
    memset(p + 4, 0, 0x10);
    if (owner) { lane.resident_code[0].lsa = 0x4000; lane.resident_code[0].size = 0x14;
                lane.resident_code[0].image_id = 25; }
}

static int no_owner;
static void scenario(void)
{
    scenario_context(!no_owner);
    spu_run_with_halt(forbidden, &lane);
    MISS_CHECK(lane.gpr[3]._u32[0] == 3);
}

#ifdef _WIN32
static DWORD __stdcall thread(void* arg) { scenario(); return 0; }
#else
static void* thread(void* arg) { scenario(); return NULL; }
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    MISS_CHECK(argc == 2); const char* mode = argv[1];
    if (!strcmp(mode, "unit")) { unit(); return 0; }
    if (!strcmp(mode, "smc") || !strcmp(mode, "lanes")) g_spu_smc_watch = 1;
    else if (!strcmp(mode, "owner")) no_owner = 1;
    else if (strcmp(mode, "entry")) return 2;
    if (!strcmp(mode, "lanes")) {
#ifdef _WIN32
        HANDLE a = CreateThread(NULL, 0, thread, NULL, 0, NULL);
        WaitForSingleObject(a, INFINITE);
        HANDLE b = CreateThread(NULL, 0, thread, NULL, 0, NULL);
        WaitForSingleObject(b, INFINITE);
#else
        pthread_t a, b;
        pthread_create(&a, NULL, thread, NULL); pthread_join(a, NULL);
        pthread_create(&b, NULL, thread, NULL); pthread_join(b, NULL);
#endif
    } else scenario();
    printf("%s PASS r3=%u\n", mode, lane.gpr[3]._u32[0]);
    return 0;
}
