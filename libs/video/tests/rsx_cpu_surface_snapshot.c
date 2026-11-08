#include "rsx_cpu_surface_snapshot.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const RsxCpuSurfaceSnapshotView views[] = {{0x1000, 8, 3}, {0x2000, 16, 2}};
    const VmReadSnapshotRange neighbours[] = {{0x1007, 1, 0}, {0x1008, 1, 1}, {0x1010, 1, 2}, {0x1017, 1, 3}};
    RsxCpuSurfaceSnapshotBinding bindings[4] = {{0}};
    assert(rsx_cpu_surface_snapshot_bind(neighbours, 4, views, 2, bindings));
    const uint32_t expected_offsets[] = {7, 8, 16, 23};
    for (unsigned i = 0; i < 4; ++i) assert(bindings[i].view_index == 0 && bindings[i].byte_offset == expected_offsets[i]);
    const VmReadSnapshotRange second[] = {{0x200f, 2, 0}};
    assert(rsx_cpu_surface_snapshot_bind(second, 1, views, 2, bindings));
    assert(bindings[0].view_index == 1 && bindings[0].byte_offset == 15);
    const VmReadSnapshotRange mixed_views[] = {{0x1000, 1, 0}, {0x2000, 1, 1}};
    assert(!rsx_cpu_surface_snapshot_bind(mixed_views, 2, views, 2, bindings));
    const VmReadSnapshotRange beyond[] = {{0x1017, 2, 0}};
    assert(!rsx_cpu_surface_snapshot_bind(beyond, 1, views, 2, bindings));
    const VmReadSnapshotRange overflow[] = {{0xffffffffu, 2, 0}};
    assert(!rsx_cpu_surface_snapshot_bind(overflow, 1, views, 2, bindings));
    const RsxCpuSurfaceSnapshotView overflowing_view[] = {{0xfffffffeu, 4, 1}};
    const VmReadSnapshotRange malformed_view_range[] = {{0xffffffffu, 1, 0}};
    assert(!rsx_cpu_surface_snapshot_bind(malformed_view_range, 1, overflowing_view, 1, bindings));
    const RsxCpuSurfaceSnapshotView aliases[] = {{0x1000, 8, 3}, {0x1000, 8, 3}};
    assert(!rsx_cpu_surface_snapshot_bind(neighbours, 1, aliases, 2, bindings));
    const VmReadSnapshotRange gap[] = {{0x1800, 1, 0}};
    assert(!rsx_cpu_surface_snapshot_bind(gap, 1, views, 2, bindings));
    puts("PASS CPU-surface snapshot binding: pitch padding, ownership, aliases, bounds");
}
