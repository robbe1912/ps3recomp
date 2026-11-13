/* Authored job-module header identity helpers. Configure the selector before
 * starting workers. Each worker owns its cached selector and diagnostic cap. */
#ifndef PS3RECOMP_SPU_JOBMOD_IDENTITY_H
#define PS3RECOMP_SPU_JOBMOD_IDENTITY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
#define SPU_JOBMOD_THREAD_LOCAL thread_local
#else
#define SPU_JOBMOD_THREAD_LOCAL _Thread_local
#endif

static SPU_JOBMOD_THREAD_LOCAL int s_spu_jobmod_header_mode = -1;
static SPU_JOBMOD_THREAD_LOCAL unsigned s_spu_jobmod_header_logs;

static inline int spu_jobmod_header_fnv_enabled(void)
{
    if (s_spu_jobmod_header_mode < 0) {
        const char* value = getenv("SPU_JOBMOD_HEADER_FNV");
        if (!value || !strcmp(value, "0")) s_spu_jobmod_header_mode = 0;
        else if (!strcmp(value, "1")) s_spu_jobmod_header_mode = 1;
        else { fprintf(stderr, "invalid SPU_JOBMOD_HEADER_FNV: expected 0 or 1\n"); abort(); }
    }
    return s_spu_jobmod_header_mode;
}

static inline uint32_t stage_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Identity span declared by a burst's first transfer, or 0: magic at +0x20,
 * link base at +0x28 equal to the staging address, image size at +0x14. */
static inline uint32_t stage_header_span(const uint8_t* ls, uint32_t lsa, uint32_t size)
{
    if (size < 0x30 || stage_be32(ls + 0x20) != 0xC0DEC0DEu || stage_be32(ls + 0x28) != lsa)
        return 0;
    uint32_t image = stage_be32(ls + 0x14);
    if (image < 0x820u || image - 0x20u > 0x3E000u - lsa) return 0;
    return image - 0x20u;
}

static inline int spu_jobmod_header_log_enabled(void)
{
    if (s_spu_jobmod_header_logs >= 16u) return 0;
    ++s_spu_jobmod_header_logs;
    return 1;
}

#undef SPU_JOBMOD_THREAD_LOCAL
#endif
