/* Inline fragment constants of one FP snapshot, collected once.
 *
 * rsx_fp_collect_constants() is a pure function of the program bytes, and the
 * bytes of an FP snapshot slot do not change while the slot holds its program
 * (vp_fp_snap_take() is the only writer). So each draw of a snapshot can reuse
 * the result of the first draw. The cache has one entry per snapshot slot.
 * vp_fp_snap_take() resets the entry of the ring slot it takes, so a reused slot
 * never returns the values of its last program.
 *
 * The includer defines RSX_FP_CONST_CACHE_MAX_CONSTS (the constants that one draw
 * slot holds) before the include. The alpha-test reference is not cached: it can
 * change from draw to draw without a change of the program.
 */
#ifndef PS3RECOMP_RSX_FP_CONST_CACHE_H
#define PS3RECOMP_RSX_FP_CONST_CACHE_H

#include <string.h>
#include "rsx_fp_decompiler.h"

#ifndef RSX_FP_CONST_CACHE_MAX_CONSTS
#error define RSX_FP_CONST_CACHE_MAX_CONSTS before rsx_fp_const_cache.h
#endif

typedef struct rsx_fp_const_cache {
    u32 valid;      /* 0: the snapshot slot holds no result */
    u32 count;      /* constants the program declares; 0 for a malformed program */
    u32 values[RSX_FP_CONST_CACHE_MAX_CONSTS][4];   /* set when count <= the bound */
} rsx_fp_const_cache;

typedef struct rsx_fp_const_result {
    u32 count;      /* what rsx_fp_collect_constants() returns; 0 when it returns <= 0 */
    const u32* values;
} rsx_fp_const_result;

typedef int (*rsx_fp_const_collect_fn)(const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out);

static inline void rsx_fp_const_cache_reset(rsx_fp_const_cache* entry)
{
    entry->valid = 0;
}

/* Constants of the program at `bytes` for one draw. `entry` is the cache slot of
 * the verified snapshot at `bytes`; pass NULL with live guest memory, which is
 * collected for each draw. `scratch` takes the block of one collection. */
static inline void rsx_fp_const_cache_resolve(
    rsx_fp_const_cache* entry, const u8* bytes, u32 max_bytes, u32 max_constants,
    rsx_fp_constant_block* scratch, rsx_fp_const_collect_fn collect, rsx_fp_const_result* out)
{
    if (entry && entry->valid) {
        out->count = entry->count;
        out->values = entry->values;
        return;
    }
    const int got = collect(bytes, max_bytes, scratch);
    out->count = got > 0 ? (u32)got : 0u;
    if (!entry) {
        out->values = scratch->values;
        return;
    }
    entry->count = out->count;
    if (out->count && out->count <= max_constants)
        memcpy(entry->values, scratch->values, (size_t)out->count * 16u);
    entry->valid = 1;
    out->values = entry->values;
}

#endif
