#include "ps3emu/guest_read_sync.h"
#include <stddef.h>
#include <string.h>
volatile unsigned char vm_read_sync_pages[1u << 20];
volatile uint32_t vm_read_sync_any;
static vm_read_sync_handler s_handler;
static void *s_opaque;
static vm_read_sync_mark_hook s_mark_hook;
void vm_read_sync_set_mark_hook(vm_read_sync_mark_hook hook) {
    s_mark_hook=hook;
}
void vm_read_sync_set_handler(vm_read_sync_handler handler, void *opaque) {
    if(handler) s_opaque=opaque;
#if defined(_MSC_VER) && !defined(__clang__)
    _InterlockedExchangePointer((void *volatile *)&s_handler,(void *)handler);
#else
    __atomic_store_n(&s_handler,handler,__ATOMIC_RELEASE);
#endif
}
int vm_read_sync_mark_range(uint32_t address, uint64_t bytes) {
    if(!bytes || bytes>UINT64_C(0x100000000)-(uint64_t)address) return -1;
    unsigned first=address>>12,last=(unsigned)(((uint64_t)address+bytes-1)>>12);
    for(unsigned page=first;page<=last;page++) {
#if defined(_MSC_VER) && !defined(__clang__)
        _InterlockedExchange8((volatile char *)&vm_read_sync_pages[page],1);
#else
        __atomic_store_n(&vm_read_sync_pages[page],1,__ATOMIC_RELEASE);
#endif
    }
    /* Set after the pages, so a reader that sees the flag sees the marks. */
#if defined(_MSC_VER) && !defined(__clang__)
    _InterlockedExchange((volatile long *)&vm_read_sync_any,1);
#else
    __atomic_store_n(&vm_read_sync_any,1,__ATOMIC_RELEASE);
#endif
    /* The fast memory path of the PPU (PPU_FM4) tests its page byte, not
     * the marks. The hook sets that byte. It runs after the flag: a reader
     * that the byte sends to vm_cpu_read_sync then sees the flag. */
    if(s_mark_hook)s_mark_hook(first,last);
    return 0;
}
void vm_read_sync_dispatch(uint32_t address, uint32_t bytes) {
#if defined(_MSC_VER) && !defined(__clang__)
    vm_read_sync_handler handler=(vm_read_sync_handler)_InterlockedCompareExchangePointer((void *volatile *)&s_handler,NULL,NULL);
#else
    vm_read_sync_handler handler=__atomic_load_n(&s_handler,__ATOMIC_ACQUIRE);
#endif
    if(handler)handler(address,bytes,s_opaque);
}

static vm_read_snapshot_handler s_snapshot_handler;
static void *s_snapshot_opaque;
void vm_read_snapshot_set_handler(vm_read_snapshot_handler handler, void *opaque) {
    if(handler)s_snapshot_opaque=opaque;
#if defined(_MSC_VER) && !defined(__clang__)
    _InterlockedExchangePointer((void *volatile *)&s_snapshot_handler,(void *)handler);
#else
    __atomic_store_n(&s_snapshot_handler,handler,__ATOMIC_RELEASE);
#endif
}
int vm_read_snapshot(const VmReadSnapshotRange *ranges, uint32_t count,
    void *destination, uint32_t output_bytes) {
    if(!ranges || !destination || !count || count>VM_READ_SNAPSHOT_MAX_RANGES ||
        !output_bytes || output_bytes>VM_READ_SNAPSHOT_MAX_BYTES)
        return VM_READ_SNAPSHOT_INVALID;
    VmReadSnapshotRange checked[VM_READ_SNAPSHOT_MAX_RANGES];
    for(uint32_t i=0;i<count;i++) {
        checked[i]=ranges[i];
        const VmReadSnapshotRange *r=&checked[i];
        if(!r->bytes || (uint64_t)r->address+r->bytes>UINT64_C(0x100000000) ||
            (uint64_t)r->output_offset+r->bytes>output_bytes)
            return VM_READ_SNAPSHOT_INVALID;
        for(uint32_t j=0;j<i;j++)
            if((uint64_t)r->output_offset< (uint64_t)checked[j].output_offset+checked[j].bytes &&
                (uint64_t)checked[j].output_offset<(uint64_t)r->output_offset+r->bytes)
                return VM_READ_SNAPSHOT_INVALID;
    }
#if defined(_MSC_VER) && !defined(__clang__)
    vm_read_snapshot_handler handler=(vm_read_snapshot_handler)_InterlockedCompareExchangePointer((void *volatile *)&s_snapshot_handler,NULL,NULL);
#else
    vm_read_snapshot_handler handler=__atomic_load_n(&s_snapshot_handler,__ATOMIC_ACQUIRE);
#endif
    if(!handler)return VM_READ_SNAPSHOT_UNAVAILABLE;
    unsigned char staging[VM_READ_SNAPSHOT_MAX_BYTES];
    memcpy(staging,destination,output_bytes);
    int result=handler(checked,count,staging,output_bytes,s_snapshot_opaque);
    if(result==VM_READ_SNAPSHOT_OK)memcpy(destination,staging,output_bytes);
    else if(result!=VM_READ_SNAPSHOT_UNAVAILABLE && result!=VM_READ_SNAPSHOT_INVALID)
        result=VM_READ_SNAPSHOT_FAILED;
    return result;
}
