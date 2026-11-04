/* WWS policy code identity. Image numbers belong to the registering port;
 * the policy role and current staged header determine overlay admission. */
#ifndef PS3RECOMP_SPU_POLICY_OVERLAY_H
#define PS3RECOMP_SPU_POLICY_OVERLAY_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
int spu_overlay_match_sig(const uint8_t hdr[16]);
static inline int spu_policy_job_address(const spu_context* ctx, uint32_t pc)
{
    return ctx && ctx->policy_mode && pc >= 0x4000u && pc < 0x3E000u;
}
static inline int spu_policy_overlay_image(const spu_context* ctx, uint32_t pc)
{
    if (!spu_policy_job_address(ctx, pc) || !ctx->ls) return 0;
    const uint8_t* p = ctx->ls + 0x1320u; /* WWS policy code-buffer field. */
    const uint32_t base = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 8) | p[3];
    /* Subtraction keeps an invalid base+16 from wrapping. */
    if (base > SPU_LS_SIZE - 16u) return 0;
    return spu_overlay_match_sig(ctx->ls + base);
}
static inline void spu_policy_overlay_refresh(spu_context* ctx, uint32_t pc)
{
    if (spu_policy_job_address(ctx, pc))
        ctx->resident_ovl = spu_policy_overlay_image(ctx, pc);
}
#ifdef __cplusplus
}
#endif
#endif
