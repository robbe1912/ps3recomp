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

/* Kind of a staging burst. A staging accumulator holds the kind in the place
 * of the span while the burst lasts.
 * STAGE_SPAN_LEGACY: hash every byte and offer the legacy key only. This is
 *   header mode off. A header that is not valid gives the span 0, with the
 *   same effect.
 * STAGE_SPAN_PENDING: header mode and fewer than 0x30 bytes in the burst, so
 *   the header cannot be read yet.
 * STAGE_SPAN_DATA: header mode and no C0DEC0DE magic at +0x20. Each identity
 *   that a header declares is at least 0x800 bytes and holds the magic at
 *   +0x20, so no such burst can be that identity: it needs no hash.
 * Any other value: the identity span that the header declares. The burst
 *   keeps each key that it had: the legacy key over all its bytes, and the
 *   header key over the span, offered one time. */
#define STAGE_SPAN_LEGACY  0u
#define STAGE_SPAN_PENDING 0xFFFFFFFEu
#define STAGE_SPAN_DATA    0xFFFFFFFFu

/* Kind of the burst that starts at `base` and holds at least 0x30 bytes. */
static inline uint32_t stage_burst_kind(const uint8_t* base, uint32_t lsa, int header_identity)
{
    if (!header_identity) return STAGE_SPAN_LEGACY;
    if (stage_be32(base + 0x20) != 0xC0DEC0DEu) return STAGE_SPAN_DATA;
    return stage_header_span(base, lsa, 0x30u);
}

static inline int spu_jobmod_header_log_enabled(void)
{
    if (s_spu_jobmod_header_logs >= 16u) return 0;
    ++s_spu_jobmod_header_logs;
    return 1;
}

#undef SPU_JOBMOD_THREAD_LOCAL
#endif
