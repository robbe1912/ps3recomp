#ifndef PS3RECOMP_RSX_CPU_SURFACE_SNAPSHOT_H
#define PS3RECOMP_RSX_CPU_SURFACE_SNAPSHOT_H

#include <stdint.h>
#include "ps3emu/guest_read_sync.h"

/* A provider may stage a range only when one live surface owns every byte.  The
 * logical render width is intentionally absent: GetDepth's original neighbour
 * loads may cross that edge, while the registered pitch-backed allocation is
 * the actual ownership boundary. */
typedef struct RsxCpuSurfaceSnapshotView {
    uint32_t address;
    uint32_t pitch;
    uint32_t height;
} RsxCpuSurfaceSnapshotView;

typedef struct RsxCpuSurfaceSnapshotBinding {
    uint32_t view_index;
    uint32_t byte_offset;
} RsxCpuSurfaceSnapshotBinding;

static inline int rsx_cpu_surface_snapshot_bind(const VmReadSnapshotRange *ranges,
    uint32_t range_count, const RsxCpuSurfaceSnapshotView *views,
    uint32_t view_count, RsxCpuSurfaceSnapshotBinding *bindings)
{
    if (!ranges || !range_count || !views || !view_count || !bindings)
        return 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        const uint64_t begin = ranges[i].address;
        const uint64_t end = begin + ranges[i].bytes;
        if (!ranges[i].bytes || end > UINT64_C(0x100000000))
            return 0;
        uint32_t matched = UINT32_MAX;
        for (uint32_t j = 0; j < view_count; ++j) {
            const uint64_t view_begin = views[j].address;
            const uint64_t view_end = view_begin + (uint64_t)views[j].pitch * views[j].height;
            if (!views[j].pitch || !views[j].height || view_end > UINT64_C(0x100000000))
                continue;
            if (begin < view_end && end > view_begin) {
                if (begin < view_begin || end > view_end || matched != UINT32_MAX)
                    return 0; /* Partial overlap also makes ownership ambiguous. */
                matched = j;
            }
        }
        if (matched == UINT32_MAX)
            return 0;
        /* A single transactional query needs one coherent allocation.  It may
         * contain pitch padding, but it may not splice neighbours from
         * independently published render targets. */
        if (i && matched != bindings[0].view_index)
            return 0;
        bindings[i].view_index = matched;
        bindings[i].byte_offset = (uint32_t)(begin - views[matched].address);
    }
    return 1;
}

#endif
