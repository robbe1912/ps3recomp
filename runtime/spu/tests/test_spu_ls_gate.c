/* One gate for the probes of spu_ls_read128 and spu_ls_write128, and the
 * SPU_LANE_TRACE switch of the [spurs-pm] poll line. Real runtime files, no
 * runtime mocks. The lifted bodies come from the lifter (spu_ls_gate_call.h)
 * and use the call, drain and return ABI of generated C. The runner starts one
 * process per environment, because the gate reads the environment once. */
#define main existing_handoff_main
#include "test_spu_interpreter_handoff.c"
#undef main
#include "spu_ls_gate_call.h"

extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_polls;
extern int g_spu_ls_gate, g_spu_lane_trace;

#define GATE_CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while (0)

/* Independent reference: big-endian bytes at the address after the mask. */
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
static int same(u128 a, u128 b)
{
    return a._u32[0] == b._u32[0] && a._u32[1] == b._u32[1] &&
           a._u32[2] == b._u32[2] && a._u32[3] == b._u32[3];
}
static int quad_is(const unsigned char* p, uint32_t w)
{
    return p[0] == (w >> 24) && p[1] == (w >> 16) && p[2] == (w >> 8) && p[3] == (unsigned char)w;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    GATE_CHECK(argc == 2);
    const char* mode = argv[1];
    const int policy = !strcmp(mode, "policy");
    const char* trace = getenv("SPU_LANE_TRACE");          /* set, non-empty and not "0" is on */
    const int armed = getenv("SPU_LS_LOWREAD") || getenv("SPU_SMC_WATCH") ||
                      getenv("SPU_LS_WATCH") || (trace && *trace && strcmp(trace, "0"));
    spu_context_init(&ctx, 0);
    ctx.image_id = policy ? 2 : 77;
    ctx.policy_mode = policy;
    ctx.status = SPU_STATUS_RUNNING;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) ctx.ls[i] = pattern(i);
    spu_begin_image(ctx.image_id);
    spu_register_function(0x240, finish);
    GATE_CHECK(g_spu_ls_gate == -1 && g_spu_lane_trace == -1);
    int halted = spu_run_with_halt(policy ? lifted_poll_job : lifted_edge_job, &ctx);
    GATE_CHECK(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    GATE_CHECK(ctx.gpr[11]._u32[0] == 1);            /* the job returned to 0x240 */
    if (policy) {
        /* cellSpursModulePoll ran once; [wws-code] and [wws-res] are its probes. */
        GATE_CHECK(g_spurs_pm_polls == 1);
        GATE_CHECK(same(ctx.gpr[9], expect16(0xDF0u)));
        GATE_CHECK(same(ctx.gpr[10], splat(0x00005A5Au)));   /* il sign-extends 0x5A5A */
        unsigned changed = 0;
        for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
            int in_store = i >= 0x1320u && i < 0x1330u;
            unsigned char want = (unsigned char)(in_store ? (((i & 3) < 2) ? 0x00 : 0x5A) : pattern(i));
            if (ctx.ls[i] != want) { fprintf(stderr,"diff at 0x%05X: got %02X want %02X\n", i, ctx.ls[i], want); GATE_CHECK(0); }
            changed += in_store;
        }
        GATE_CHECK(changed == 16);
    } else {
        GATE_CHECK(same(ctx.gpr[3], expect16(0x0u)));        /* 0 */
        GATE_CHECK(same(ctx.gpr[4], expect16(0x40000u)));    /* above the mask: wraps to 0 */
        GATE_CHECK(same(ctx.gpr[5], expect16(0x3FFF0u)));    /* the last quadword */
        GATE_CHECK(same(ctx.gpr[6], expect16(0x1230u)));     /* 0x1235: unaligned, rounds down */
        GATE_CHECK(same(ctx.gpr[10], splat(0x11223344u)) && same(ctx.gpr[12], splat(0x55667788u)));
        /* 0x40035 is stored at 0x30; 0x123A is stored at 0x1230. No other byte changes. */
        unsigned changed = 0;
        for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
            uint32_t w = i < 0x40u ? 0x11223344u : 0x55667788u;
            int in_store = (i >= 0x30u && i < 0x40u) || (i >= 0x1230u && i < 0x1240u);
            GATE_CHECK(ctx.ls[i] == (in_store ? (unsigned char)(w >> (8 * (3 - (i & 3)))) : pattern(i)));
            changed += in_store;
        }
        GATE_CHECK(changed == 32);
    }
    /* The gate read the environment once, on the first accessor call. */
    GATE_CHECK((g_spu_ls_gate != 0) == (armed != 0));
    if (!armed)
        GATE_CHECK(g_spu_ls_gate == 0 && g_spu_lane_trace == 0 && g_spu_ls_watch_n == 0 &&
                   g_spu_ls_probe == 0 && g_spu_smc_watch == 0);
    printf("%s: gate=%d trace=%d watch_n=%d polls=%u\n", mode, g_spu_ls_gate,
           g_spu_lane_trace, g_spu_ls_watch_n, (unsigned)g_spurs_pm_polls);
    puts("PASS");
    return 0;
}
