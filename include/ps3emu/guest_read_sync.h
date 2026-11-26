/* CPU reads of device-owned guest pages must observe completed device writes. */
#ifndef PS3RECOMP_GUEST_READ_SYNC_H
#define PS3RECOMP_GUEST_READ_SYNC_H
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*vm_read_sync_handler)(uint32_t address, uint32_t bytes, void *opaque);
extern volatile unsigned char vm_read_sync_pages[1u << 20];
/* Nonzero after `vm_read_sync_mark_range` marks its first page. No function
 * clears a mark, so the flag stays nonzero. While it is zero,
 * `vm_cpu_read_sync` reads no table byte. */
extern volatile uint32_t vm_read_sync_any;
void vm_read_sync_set_handler(vm_read_sync_handler handler, void *opaque);
int vm_read_sync_mark_range(uint32_t address, uint64_t bytes);
void vm_read_sync_dispatch(uint32_t address, uint32_t bytes);
/* Hook of the fast memory path of the PPU (PPU_FM4). After
 * `vm_read_sync_mark_range` marked its pages and set the flag, it calls
 * the hook with the first and the last page of the range. The hook sets
 * the page byte of the fast memory path for these pages, and for the page
 * before the range. NULL (the start): only the marks are set. The paths
 * of the fast memory before PPU_FM4 test the marks inline, so they need
 * no hook. Install it before the first mark, as the handler. */
typedef void (*vm_read_sync_mark_hook)(unsigned first_page, unsigned last_page);
void vm_read_sync_set_mark_hook(vm_read_sync_mark_hook hook);

/* Small immutable native views. The provider must establish device visibility
 * before filling the staging buffer. No installed provider means unavailable;
 * this API never substitutes an unsynchronized guest-memory read.
 *
 * Installation/removal requires quiescent callers, as does handler lifetime.
 * Callers keep guest source allocations alive and own the range descriptors
 * and destination throughout the call.
 * Destination writes are transactional, including provider rejection/failure.
 */
#define VM_READ_SNAPSHOT_MAX_RANGES 16u
#define VM_READ_SNAPSHOT_MAX_BYTES 4096u
typedef struct VmReadSnapshotRange {
    uint32_t address, bytes, output_offset;
} VmReadSnapshotRange;
enum {
    VM_READ_SNAPSHOT_FAILED = -2,
    VM_READ_SNAPSHOT_INVALID = -1,
    VM_READ_SNAPSHOT_UNAVAILABLE = 0,
    VM_READ_SNAPSHOT_OK = 1
};
typedef int (*vm_read_snapshot_handler)(const VmReadSnapshotRange *ranges,
    uint32_t count, void *staging, uint32_t output_bytes, void *opaque);
void vm_read_snapshot_set_handler(vm_read_snapshot_handler handler, void *opaque);
int vm_read_snapshot(const VmReadSnapshotRange *ranges, uint32_t count,
    void *destination, uint32_t output_bytes);
static inline unsigned char vm_read_sync_page(unsigned index) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (unsigned char)_InterlockedCompareExchange8((volatile char *)&vm_read_sync_pages[index],0,0);
#else
    return __atomic_load_n(&vm_read_sync_pages[index], __ATOMIC_ACQUIRE);
#endif
}
static inline void vm_cpu_read_sync(uint32_t address, uint32_t bytes) {
    if (!bytes || (uint64_t)address+bytes > UINT64_C(0x100000000)) return;
    if (!vm_read_sync_any) return;
    unsigned first=address>>12, last=(unsigned)(((uint64_t)address+bytes-1)>>12);
    for (unsigned page=first;page<=last;page++)
        if(vm_read_sync_page(page)) { vm_read_sync_dispatch(address,bytes); return; }
}
#ifdef __cplusplus
}
#endif
#endif
