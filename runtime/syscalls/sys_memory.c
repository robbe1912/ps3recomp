/*
 * ps3recomp - Memory management syscalls (implementation)
 */

#include "sys_memory.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

/* Bump allocator for main memory pool.
 * Starts after a reasonable offset to avoid the low addresses used by ELF. */
uint32_t g_sys_mem_bump_ptr = 0;

/* Total user memory size (default 213 MB, after kernel reservation) */
#define SYS_MEM_USER_TOTAL  (213 * 1024 * 1024)

/* Guest window handed out by sys_memory_allocate / sys_mmapper.
 * 0x40000000+ matches where the real lv2 places these allocations (verified
 * against an RPCS3 boot log of Yakuza: Dead Souls); the window is outside
 * the pre-committed main region, so pages are committed on demand. */
#define SYS_MEM_ALLOC_BASE  0x40000000u
/* Alias collision gaps consume virtual addresses, not user memory. Keep
 * sufficient VA space for the advertised physical pool and its alias gaps. */
#define SYS_MEM_ALLOC_END   0x60000000u

static uint32_t s_total_allocated = 0;

/* Guest threads are real host threads; serialize the bump allocator. */
#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate
 *
 * r3 = size
 * r4 = flags (page size)
 * r5 = pointer to receive allocated address (u32*)
 * -----------------------------------------------------------------------*/
/* Both allocation syscalls share the same VA and physical-memory accounting. */
static int64_t memory_allocate(uint32_t size, uint32_t flags,
                               uint32_t addr_out, uint32_t container_id)
{
    uint32_t alignment = (flags & SYS_MEMORY_PAGE_SIZE_1M) ? 0x100000 : 0x10000;
    size = VM_ALIGN_UP(size, alignment);
    if (!size) return (int64_t)(int32_t)CELL_EINVAL;

    bump_lock();
    sys_mem_container_info* container = NULL;
    if (container_id) {
        if (container_id > SYS_MEMORY_CONTAINER_MAX ||
            !g_sys_mem_containers[container_id - 1].active) {
            bump_unlock();
            return (int64_t)(int32_t)CELL_ESRCH;
        }
        container = &g_sys_mem_containers[container_id - 1];
        if (container->used_size > container->total_size ||
            size > container->total_size - container->used_size) {
            bump_unlock();
            return (int64_t)(int32_t)CELL_ENOMEM;
        }
    }
    if (s_total_allocated > SYS_MEM_USER_TOTAL ||
        size > SYS_MEM_USER_TOTAL - s_total_allocated) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    uint32_t address = VM_ALIGN_UP(g_sys_mem_bump_ptr ? g_sys_mem_bump_ptr : SYS_MEM_ALLOC_BASE, alignment);
    extern int sysmem_alias_va_conflicts(uint32_t, uint32_t);
    while (address <= SYS_MEM_ALLOC_END - size &&
           sysmem_alias_va_conflicts(address, size)) address += alignment;
    if (address > SYS_MEM_ALLOC_END - size) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++)
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    if (slot < 0) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    int32_t rc = vm_commit(address, size);
    if (rc != CELL_OK) {
        bump_unlock();
        return (int64_t)rc;
    }
    extern void sysmem_alias_record(uint32_t, uint32_t);
    sysmem_alias_record(address, size);
    sys_mem_alloc_info* allocation = &g_sys_mem_allocs[slot];
    allocation->addr = address;
    allocation->size = size;
    allocation->container_id = (int32_t)container_id;
    allocation->page_size = alignment;
    allocation->active = 1;
    s_total_allocated += size;
    if (container) container->used_size += size;
    g_sys_mem_bump_ptr = address + size;
    if (addr_out) write_be32(addr_out, address);
    bump_unlock();
    fprintf(stderr, "[sys_memory] allocate -> 0x%08X\n", address);
    return CELL_OK;
}

int64_t sys_memory_allocate(ppu_context* ctx)
{
    return memory_allocate(LV2_ARG_U32(ctx, 0), LV2_ARG_U32(ctx, 1),
                           LV2_ARG_PTR(ctx, 2), 0);
}

/* LV2 350: size, container ID, page flags, output pointer (r3..r6). */
int64_t sys_memory_allocate_from_container(ppu_context* ctx)
{
    uint64_t size = ctx->gpr[3];
    uint32_t cid = LV2_ARG_U32(ctx, 1);
    uint64_t flags = ctx->gpr[5];
    uint32_t output = LV2_ARG_PTR(ctx, 3);
    uint32_t alignment = flags == 0 || flags == SYS_MEMORY_PAGE_SIZE_1M ? 0x100000 :
                         flags == SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : 0;
    if (!size) return (int64_t)(int32_t)CELL_EALIGN;
    if (!alignment) return (int64_t)(int32_t)CELL_EINVAL;
    if (size % alignment) return (int64_t)(int32_t)CELL_EALIGN;
    if (size > UINT32_MAX) return (int64_t)(int32_t)CELL_ENOMEM;
    if (!cid) return (int64_t)(int32_t)CELL_ESRCH;
    if (!output) return (int64_t)(int32_t)CELL_EFAULT;
    return memory_allocate((uint32_t)size, flags ? (uint32_t)flags : SYS_MEMORY_PAGE_SIZE_1M,
                           output, cid);
}

/* ---------------------------------------------------------------------------
 * sys_memory_free
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    bump_lock();
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        sys_mem_alloc_info* allocation = &g_sys_mem_allocs[i];
        if (allocation->active && allocation->addr == addr) {
            s_total_allocated -= allocation->size;
            if (allocation->container_id > 0 && allocation->container_id <= SYS_MEMORY_CONTAINER_MAX)
                g_sys_mem_containers[allocation->container_id - 1].used_size -= allocation->size;
            allocation->active = 0;
            bump_unlock();
            return CELL_OK;
        }
    }
    bump_unlock();
    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_user_memory_size
 *
 * r3 = pointer to output struct: { u32 total, u32 available }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");
    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        uint32_t avail = total - s_total_allocated;
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_page_attribute (syscall 351 / 0x15F)
 *
 * r3 = address to query
 * r4 = pointer to sys_page_attribute_t:
 *        +0x00 attribute   (u64)
 *        +0x08 access_right(u64)
 *        +0x10 page_size   (u32)   <- the field titles actually check
 *        +0x14 pad         (u32)
 *
 * Was unimplemented (fell through to the return-0 stub), so the output struct
 * was never filled -> callers reading page_size got stack garbage. Dantelion2's
 * graphics init (func_009F6D40) queries a 1MB-page buffer and only proceeds to
 * construct a sub-object when page_size == 0x100000; the stub made it take the
 * failure path, leaving that sub-object NULL -> later null-deref crash.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    /* Report the page size of the allocation containing addr. */
    uint32_t page_size = 0x10000;  /* default 64K */
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active &&
            addr >= g_sys_mem_allocs[i].addr &&
            addr <  g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size) {
            page_size = g_sys_mem_allocs[i].page_size ? g_sys_mem_allocs[i].page_size : 0x10000;
            break;
        }
    }

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);          /* attribute    (hi) */
        write_be32(attr_out + 0x04, 0x00040000); /* attribute    (lo) PROT_READ_WRITE-ish */
        write_be32(attr_out + 0x08, 0);          /* access_right (hi) */
        write_be32(attr_out + 0x0C, 0x00000008); /* access_right (lo) PPU_THREAD-ish */
        write_be32(attr_out + 0x10, page_size);  /* page_size */
        write_be32(attr_out + 0x14, 0);          /* pad */
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_create
 *
 * r3 = pointer to receive container ID (u32*)
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_destroy
 *
 * r3 = container_id
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    bump_lock();
    if (!c->active) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (c->used_size) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    c->active = 0;
    bump_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_get_size
 *
 * REAL ABI (matches the game's register setup and rpcs3's
 * sys_memory_container_get_size(vm::ptr<sys_memory_info_t> mem_info, u32 cid)):
 *   r3 = pointer to output struct { u32 total_user_memory, u32 available_user_memory }
 *   r4 = container_id
 * The second word is AVAILABLE, not used -- MotorStorm: Pacific Rift's
 * thread_auto_load_save reads info[1] and fatals ("Memory container failure")
 * unless it exceeds 4 MB.
 *
 * MS1 never calls container_create through any lv2-visible path; it passes its
 * own WWS pool handle (an mmapper address, e.g. 0x40300000) as the cid.
 * Like rpcs3's lv2_memory_container::search(SYS_MEMORY_CONTAINER_ID_INVALID)
 * falling back to the default container, an unknown non-zero cid resolves to
 * the default (system-wide) view instead of ESRCH-ing the whole boot.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 0);
    uint32_t container_id = LV2_ARG_U32(ctx, 1);

    uint32_t total = 0x0EC00000u, used = 0;   /* default container: ~236 MB user memory */
    if (container_id >= 1 && container_id <= SYS_MEMORY_CONTAINER_MAX &&
        g_sys_mem_containers[container_id - 1].active) {
        total = g_sys_mem_containers[container_id - 1].total_size;
        used  = g_sys_mem_containers[container_id - 1].used_size;
    } else if (container_id != 0 && container_id != 0xFFFFFFFFu) {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            fprintf(stderr, "[sys_memory] container_get_size: unknown cid 0x%08X -> "
                            "default container\n", container_id);
        }
    } else {
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (out_addr != 0) {
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, total - used);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_address
 *
 * r3 = size
 * r4 = flags
 * r5 = alignment
 * r6 = pointer to receive address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint32_t size      = LV2_ARG_U32(ctx, 0);
    /* uint32_t flags  = LV2_ARG_U32(ctx, 1); */
    uint32_t alignment = LV2_ARG_U32(ctx, 2);
    uint32_t addr_out  = LV2_ARG_PTR(ctx, 3);

    if (alignment == 0) alignment = 0x10000;
    size = VM_ALIGN_UP(size, alignment);

    bump_lock();

    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;

    g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, alignment);

    if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    uint32_t alloc_addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;

    bump_unlock();

    /* Commit the reserved region */
    vm_commit(alloc_addr, size);

    if (addr_out != 0) {
        write_be32(addr_out, alloc_addr);
    }
    /* Register for the LV1 SPU alias window (spu_ea_alias.c): SPURS reaches
     * this memory from SPU side as base|0x05000000. */
    { extern void sysmem_alias_record(uint32_t, uint32_t);
      sysmem_alias_record(alloc_addr, size); }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_free_address
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    /* uint32_t addr = LV2_ARG_U32(ctx, 0); */
    (void)ctx;
    /* In our bump allocator we don't actually free, just acknowledge */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_shared_memory
 *
 * r3 = key
 * r4 = size
 * r5 = flags
 * r6 = pointer to receive shared mem ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_map_shared_memory
 *
 * r3 = address (where to map)
 * r4 = shared mem ID
 * r5 = flags
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Commit the memory at the specified address */
    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK)
        return (int64_t)(int32_t)rc;

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_memory_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE_FROM_CONTAINER, sys_memory_allocate_from_container);
}
