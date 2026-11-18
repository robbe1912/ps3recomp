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
int spu_native_owned_enabled(void);
int spu_native_owned_image(void); /* zero means unrestricted */
int spu_native_owned_observed(void);
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
static inline int spu_native_owned_policy(const spu_context* ctx) {
    return ctx && ctx->policy_mode && spu_native_owned_enabled();
}
static inline int spu_native_owned_can_run(const spu_context* ctx, uint32_t pc) {
    if (!spu_native_owned_policy(ctx) || ctx->image_id < 0 || !ctx->ls ||
        pc >= SPU_LS_SIZE || (pc & 3) || ctx->irq_frame || ctx->irq_saved ||
        (ctx->int_enable && (ctx->event_status & ctx->event_mask)) ||
        g_spu_ls_watch_n > 0 || g_spu_ls_probe > 0 || g_spu_smc_watch > 0 ||
        spu_native_owned_observed()) return 0;
    const int image = spu_native_owned_image();
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
#ifdef __cplusplus
}
#endif
#endif
