/* Content identity for the source bytes actually uploaded by the legacy backend.
 * Batch-local hits retain an immutable snapshot. Cross-batch reuse validates all
 * top-level bytes of every face, excluding unused mip/padding gaps. */
#ifndef RSX_TEXTURE_FINGERPRINT_H
#define RSX_TEXTURE_FINGERPRINT_H
#include <stddef.h>
#include <stdint.h>
#include "rsx_texture_layout.h"
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

typedef struct rsx_texture_source_view {
    uint64_t face_bytes, face_stride, span_bytes, hashed_bytes;
    unsigned faces;
} rsx_texture_source_view;

static int rsx_texture_source_view_init(uint32_t w, uint32_t h,
        uint32_t format, int cube, uint32_t mips, uint64_t available,
        rsx_texture_source_view* out)
{
    if (!out || !w || !h || w > 4096 || h > 4096 || mips > 32 ||
            (cube != 0 && cube != 1)) return 0;
    rsx_tex_layout layout;
    rsx_texture_layout(format, w, h, &layout);
    uint64_t face = layout.face_bytes, stride = face;
    if (!face) return 0;
    if (cube) {
        uint64_t chain = 0;
        uint32_t lw = w, lh = h;
        for (uint32_t level = 0; level < (mips ? mips : 1); ++level) {
            rsx_tex_layout mip;
            rsx_texture_layout(format, lw, lh, &mip);
            chain += mip.face_bytes;
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        stride = (chain + 127u) & ~UINT64_C(127);
    }
    unsigned faces = cube ? 6 : 1;
    uint64_t span = stride * (faces - 1) + face;
    if (span > available || span > SIZE_MAX) return 0;
    *out = (rsx_texture_source_view){face,stride,span,face*faces,faces};
    return 1;
}

/* Widen a view's per-face identity to `chain_bytes`, the uploaded mip chain
 * starting at each face (RSX_TEX_MIPS). A cube's chain must fit inside its
 * face stride; the whole span must stay inside `available`. */
static int rsx_texture_source_view_extend(rsx_texture_source_view* view,
        uint64_t chain_bytes, uint64_t available)
{
    if (!view || chain_bytes < view->face_bytes ||
            (view->faces > 1 && chain_bytes > view->face_stride)) return 0;
    uint64_t span = view->face_stride * (view->faces - 1) + chain_bytes;
    if (span > available || span > SIZE_MAX) return 0;
    view->face_bytes = chain_bytes;
    view->span_bytes = span;
    view->hashed_bytes = chain_bytes * view->faces;
    return 1;
}

static uint64_t rsx_texture_source_fingerprint(const uint8_t* source,
        const rsx_texture_source_view* view)
{
    if (view->faces == 1) return XXH3_64bits(source,(size_t)view->face_bytes);
    XXH3_state_t state;
    XXH3_INITSTATE(&state);
    XXH3_64bits_reset(&state);
    for (unsigned face=0; face<view->faces; ++face)
        XXH3_64bits_update(&state,source+face*view->face_stride,(size_t)view->face_bytes);
    return XXH3_64bits_digest(&state);
}
#endif
