/* Opt-in compatibility bridge: policy stays interpreted; only existing
 * resident-code owners may execute compiled jobs. The staging loader remains
 * responsible for code identity and invalidating overwritten owners. Configure
 * SPU_NATIVE_OWNED_JOBS=0|1 before workers start; unset preserves the runtime.
 * SPU_NATIVE_OWNED_IMAGE optionally restricts admission to one resident image. */
#ifndef SPU_NATIVE_OWNED_H
#define SPU_NATIVE_OWNED_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The option readers run one time on each host thread. They fill the three
 * thread-local words below and return the option value. */
int spu_native_owned_enabled(void);
int spu_native_owned_image(void); /* zero means unrestricted */
int spu_native_owned_observed(void);
/* The option values of one host thread as plain thread-local words: the hot
 * paths read them with one load and no call. A word is 0 until the thread
 * reads its option the first time; then it holds the option value plus one,
 * so that a value of 0 is not mistaken for "not read". One merged definition
 * per program, like the gate words in spu_context.h: a target links the
 * readers or it does not, and the words exist in both cases. */
SPU_GATE SPU_THREAD_LOCAL int g_spu_native_owned_mode;     /* SPU_NATIVE_OWNED_JOBS */
SPU_GATE SPU_THREAD_LOCAL int g_spu_native_owned_observed; /* an observation option is on */
SPU_GATE SPU_THREAD_LOCAL int g_spu_native_owned_image;    /* SPU_NATIVE_OWNED_IMAGE */
static inline int spu_owned_jobs(void) {
    int word = g_spu_native_owned_mode;
    return word ? word - 1 : spu_native_owned_enabled();
}
static inline int spu_owned_observed(void) {
    int word = g_spu_native_owned_observed;
    return word ? word - 1 : spu_native_owned_observed();
}
static inline int spu_owned_image(void) {
    int word = g_spu_native_owned_image;
    return word ? word - 1 : spu_native_owned_image();
}
void spu_native_owned_interpret(spu_context* ctx);
/* Bounded native dispatcher call; ordinary stop/budget APIs stay reference. */
uint32_t spu_interp_run_native_call(spu_context* ctx,uint32_t start,uint32_t stop);
extern int g_spu_ls_probe;
/* Native policy-module services (SPU_PM_NATIVE_SVC=1, default off; see
 * spu_channels.c). A title registers the policy image's services once, before
 * SPU workers start: entries compiled job code calls with bisl, and the LS
 * code ranges they reach. Each call runs compiled only while those ranges in
 * LS equal the pristine text, which must hash (FNV-1a, whole registered text:
 * tmpl_len bytes from guest EA tmpl_ea, then zeros) to text_fnv. */
#define SPU_PM_SERVICES_MAX_ENTRIES 8
#define SPU_PM_SERVICES_MAX_RANGES 32
#define SPU_PM_SERVICES_COUNTERS 7  /* native, verify ok/bad/incomplete/overflow, text differs, not a job call */
typedef struct {
    int image_id;
    uint32_t ls_base, text_len;     /* policy text in LS */
    uint32_t tmpl_ea, tmpl_len;     /* pristine text in guest memory */
    uint64_t text_fnv;
    const uint32_t* entries; int nentries;
    const uint32_t (*code)[2]; int ncode;   /* [lo, hi) LS ranges */
} spu_pm_services;
void spu_pm_services_register(const spu_pm_services* services);
void spu_pm_services_counters(uint64_t out[SPU_PM_SERVICES_COUNTERS]);
extern int g_spu_pm_svc_on;
int spu_pm_svc_can_run(const spu_context* ctx, uint32_t pc);
void spu_pm_svc_drain_done(spu_context* ctx, uint32_t return_pc);
/* Policy test. The drain loop makes this test one time per iteration and
 * hands the result to spu_native_owned_run_ok. */
static inline int spu_native_owned_policy(const spu_context* ctx) {
    return ctx && ctx->policy_mode && spu_owned_jobs();
}
/* Admission of lifted code at `pc` in a policy context, without the policy
 * test. This is the complete stop list; each line makes the interpreter run
 * the code instead of the lifted function:
 *   image, local store, address range, alignment: no valid lifted code;
 *   irq_frame, irq_saved, a pending interrupt: interrupt paths stay exact;
 *   SPU_LS_WATCH, SPU_LS_LOWREAD, SPU_SMC_WATCH: the local store probes and
 *     the watchpoints, with the invalidation of overwritten owners;
 *   an observation option: tracing keeps the interpreter granularity. */
static inline int spu_native_owned_run_ok(const spu_context* ctx, uint32_t pc) {
    if (ctx->image_id < 0 || !ctx->ls ||
        pc >= SPU_LS_SIZE || (pc & 3) || ctx->irq_frame || ctx->irq_saved ||
        (ctx->int_enable && (ctx->event_status & ctx->event_mask)) ||
        g_spu_ls_watch_n > 0 || g_spu_ls_probe > 0 || g_spu_smc_watch > 0 ||
        spu_owned_observed()) return 0;
    const int image = spu_owned_image();
    for (unsigned i=0;i<sizeof(ctx->resident_code)/sizeof(ctx->resident_code[0]);++i) {
        if (!ctx->resident_code[i].image_id) continue;
        uint32_t base=ctx->resident_code[i].lsa, size=ctx->resident_code[i].size;
        if (pc < base || pc-base >= size) continue;
        /* First-containing-owner precedence matches the production resolver. */
        return ctx->resident_code[i].image_id > 0 &&
               (!image || image == ctx->resident_code[i].image_id) &&
               base < SPU_LS_SIZE &&
               size <= SPU_LS_SIZE-base;
    }
    if (image) return 0;
    return g_spu_pm_svc_on && spu_pm_svc_can_run(ctx,pc);
}
static inline int spu_native_owned_can_run(const spu_context* ctx, uint32_t pc) {
    return spu_native_owned_policy(ctx) && spu_native_owned_run_ok(ctx, pc);
}

/* The stop list of spu_native_owned_run_ok, as one number. 0: the lifted code
 * may run. The two functions hold the same test in the same order. */
enum {
    SPU_STOP_NO_IMAGE = 1, SPU_STOP_NO_LS, SPU_STOP_PC_RANGE, SPU_STOP_PC_ALIGN,
    SPU_STOP_IRQ_FRAME, SPU_STOP_IRQ_SAVED, SPU_STOP_INT_PENDING,
    SPU_STOP_LS_WATCH, SPU_STOP_LS_LOWREAD, SPU_STOP_SMC_WATCH, SPU_STOP_OBSERVED,
    SPU_STOP_NO_OWNER, SPU_STOP_OWNER_IMAGE, SPU_STOP_OWNER_RANGE,
    SPU_STOP_SVC_REFUSED, SPU_STOP_COUNT
};
static inline int spu_native_owned_stop_reason(const spu_context* ctx, uint32_t pc) {
    if (ctx->image_id < 0) return SPU_STOP_NO_IMAGE;
    if (!ctx->ls) return SPU_STOP_NO_LS;
    if (pc >= SPU_LS_SIZE) return SPU_STOP_PC_RANGE;
    if (pc & 3) return SPU_STOP_PC_ALIGN;
    if (ctx->irq_frame) return SPU_STOP_IRQ_FRAME;
    if (ctx->irq_saved) return SPU_STOP_IRQ_SAVED;
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) return SPU_STOP_INT_PENDING;
    if (g_spu_ls_watch_n > 0) return SPU_STOP_LS_WATCH;
    if (g_spu_ls_probe > 0) return SPU_STOP_LS_LOWREAD;
    if (g_spu_smc_watch > 0) return SPU_STOP_SMC_WATCH;
    if (spu_owned_observed()) return SPU_STOP_OBSERVED;
    const int image = spu_owned_image();
    for (unsigned i=0;i<sizeof(ctx->resident_code)/sizeof(ctx->resident_code[0]);++i) {
        if (!ctx->resident_code[i].image_id) continue;
        uint32_t base=ctx->resident_code[i].lsa, size=ctx->resident_code[i].size;
        if (pc < base || pc-base >= size) continue;
        const unsigned owner = ctx->resident_code[i].image_id;
        if (owner <= 0) return SPU_STOP_NO_OWNER;
        if (image && image != owner) return SPU_STOP_OWNER_IMAGE;
        if (base >= SPU_LS_SIZE || size > SPU_LS_SIZE-base) return SPU_STOP_OWNER_RANGE;
        return 0;
    }
    if (image) return SPU_STOP_OWNER_IMAGE;
    return g_spu_pm_svc_on && spu_pm_svc_can_run(ctx,pc) ? 0 : SPU_STOP_NO_OWNER;
}

/* SPU_NATIVE_OWNED_MISS_LOG=1: count the dispatches that send owned code to
 * the interpreter, by image id and by cause. Default off: one predicted
 * branch on the miss path and nothing on the lifted path. While the switch
 * is on, each lane writes one line per second in which it had a miss:
 *   [spu-miss] lane=<n> img=<id> gate stop <stop reason>=<n> ...
 *            no lifted entry <pc>=<n> | img=<id> ...
 * The numbers are the misses of the last second. The test vector of the
 * counter is spu_miss_count_test.c. */
#include "spu_interp.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "../platform/win32_compat.h"
#endif
#define SPU_MISS_NO_ENTRY SPU_STOP_COUNT   /* target without a lifted entry */
#define SPU_MISS_CAUSES (SPU_STOP_COUNT + 1)
#define SPU_MISS_IMAGES 8
#define SPU_MISS_PCS 16
#define SPU_MISS_PERIOD_MS 1000
typedef struct {
    int image;
    unsigned long long cause[SPU_MISS_CAUSES];   /* indexed by stop reason */
    struct { uint32_t pc; unsigned long long n; } entry[SPU_MISS_PCS];
    int nentry;
} spu_miss_image;
typedef struct {
    spu_miss_image image[SPU_MISS_IMAGES];
    int nimage;
    unsigned long long last_ms;   /* time of the last line; the first miss prints at once */
    int started;
} spu_miss_state;
SPU_GATE SPU_THREAD_LOCAL spu_miss_state g_spu_miss;
SPU_GATE SPU_THREAD_LOCAL int g_spu_miss_log = -1;  /* SPU_NATIVE_OWNED_MISS_LOG, then 0/1 */
SPU_GATE long g_spu_miss_lanes;

static __attribute__((noinline, cold)) int spu_miss_log_init(void)
{
    const char* e = getenv("SPU_NATIVE_OWNED_MISS_LOG");
    g_spu_miss_log = (e && *e && strcmp(e, "0")) ? 1 : 0;
    return g_spu_miss_log;
}
static inline int spu_miss_log_on(void) {
    return __builtin_expect(g_spu_miss_log >= 0, 1) ? g_spu_miss_log : spu_miss_log_init();
}

/* The lane index: 0, 1, 2 ... in the order in which the lanes count a miss. */
static __attribute__((noinline, cold)) int spu_miss_lane(void)
{
#ifdef _WIN32
    return InterlockedIncrement(&g_spu_miss_lanes) - 1;
#else
    return __atomic_add_fetch(&g_spu_miss_lanes, 1, __ATOMIC_RELAXED) - 1;
#endif
}
static inline unsigned long long spu_miss_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}
static const char* const spu_miss_stop_names[SPU_STOP_COUNT] = {
    "", "no image", "no ls", "pc range", "pc unaligned", "irq frame", "irq saved",
    "interrupt pending", "ls watch", "ls lowread", "smc watch", "observed",
    "no code owner", "owner image differs", "owner range", "policy svc refused"
};
/* The counters start at zero. The image ids and the pc table stay. */
static inline void spu_miss_image_reset(spu_miss_image* im)
{
    memset(im->cause, 0, sizeof im->cause);
    for (int e = 0; e < im->nentry; ++e) im->entry[e].n = 0;
}
static inline int spu_miss_slot(spu_miss_state* s, int image)
{
    for (int i = 0; i < s->nimage; ++i) if (s->image[i].image == image) return i;
    if (s->nimage >= SPU_MISS_IMAGES) return SPU_MISS_IMAGES - 1;  /* the last slot overflows */
    s->image[s->nimage].image = image;
    return s->nimage++;
}

/* One miss: cause is a stop reason of the gate, or SPU_MISS_NO_ENTRY for a
 * target that has no lifted entry. now_ms is a monotonic time in ms. When
 * SPU_MISS_PERIOD_MS passed since the last line, the line is written and the
 * counters start at zero. The line goes to line (SPU_MISS_LINE bytes).
 * Returns 1 when a line was written. */
#define SPU_MISS_LINE 12288
static __attribute__((noinline)) int spu_miss_count(spu_miss_state* s, int lane, int image,
                                                    int cause, uint32_t pc, unsigned long long now_ms,
                                                    char* line)
{
    spu_miss_image* im = &s->image[spu_miss_slot(s, image)];
    if (cause == SPU_MISS_NO_ENTRY) {
        int e;
        for (e = 0; e < im->nentry && im->entry[e].pc != pc; ++e) {}
        if (e == im->nentry && e < SPU_MISS_PCS) { im->entry[e].pc = pc; im->nentry = e + 1; }
        if (e >= SPU_MISS_PCS) { e = SPU_MISS_PCS - 1; im->entry[e].pc = 0xFFFFFFFFu; }
        ++im->entry[e].n;
    } else {
        ++im->cause[cause < SPU_STOP_COUNT ? cause : 0];
    }
    if (!s->started) { s->started = 1; s->last_ms = now_ms - SPU_MISS_PERIOD_MS; }
    if (now_ms - s->last_ms < SPU_MISS_PERIOD_MS) return 0;
    s->last_ms = now_ms;
    int n = 0, printed = 0;
    n += snprintf(line + n, (size_t)SPU_MISS_LINE - n, "[spu-miss] lane=%d", lane);
    for (int i = 0; i < s->nimage; ++i) {
        const spu_miss_image* m = &s->image[i];
        unsigned long long sum = 0;
        for (int c = 0; c < SPU_MISS_CAUSES; ++c) sum += m->cause[c];
        for (int e = 0; e < m->nentry; ++e) sum += m->entry[e].n;
        if (!sum) continue;   /* an image without a miss in this second prints nothing */
        const int first = !printed; printed = 1;
        n += snprintf(line + n, (size_t)SPU_MISS_LINE - n, "%s img=%d", first ? "" : " |", m->image);
        for (int c = 1; c < SPU_STOP_COUNT; ++c)
            if (m->cause[c])
                n += snprintf(line + n, (size_t)SPU_MISS_LINE - n, " gate stop %s=%llu",
                              spu_miss_stop_names[c], m->cause[c]);
        for (int e = 0; e < m->nentry; ++e)
            if (m->entry[e].n)
                n += snprintf(line + n, (size_t)SPU_MISS_LINE - n, " no lifted entry 0x%05X=%llu",
                              m->entry[e].pc, m->entry[e].n);
    }
    if (n > 0 && (size_t)n >= SPU_MISS_LINE) n = sizeof line - 1;
    line[n] = 0;
    for (int i = 0; i < SPU_MISS_IMAGES; ++i) spu_miss_image_reset(&s->image[i]);
    return 1;
}

static inline void spu_miss_reset(void) { g_spu_miss = (spu_miss_state){0}; }

/* 0: the lifted code of an owned context may run. Else the dispatch goes to
 * the interpreter, and the value says why. */
static inline int spu_native_owned_miss(const spu_context* ctx)
{
    if (!spu_native_owned_can_run(ctx,ctx->pc)) return spu_native_owned_stop_reason(ctx,ctx->pc);
    return spu_lifted_lookup(ctx,ctx->pc) ? 0 : SPU_MISS_NO_ENTRY;
}
/* The call of the dispatch site, on the miss path only. */
static inline void spu_miss_hit(spu_context* ctx, int cause)
{
    if (__builtin_expect(spu_miss_log_on(), 0))
    {
        char line[SPU_MISS_LINE];
        if (spu_miss_count(&g_spu_miss, spu_miss_lane(), ctx->image_id, cause, ctx->pc, spu_miss_ms(), line))
            fprintf(stderr, "%s\n", line);
    }
}
#ifdef __cplusplus
}
#endif
#endif
