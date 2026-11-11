/* Per-draw guest sampler tables for the VP root signature (root parameter 3).
 * Included after s_d3d/D3D12DrawRecord, vp_upload_retire and the FP snapshot
 * ring. Tables are immutable while any recorded command can reference them: a
 * persistent base heap caches tables across frames, overflow heaps are added
 * only when a frame needs more than 128 distinct tables, and descriptors are
 * reused or heaps released only after a checked GPU retirement. A failed
 * retirement keeps every table. Pattern follows ps3recomp's
 * rsx_address_tables.h; filtering is decoded as well. */
#include "rsx_d3d12_guest_sampler.h"

#define GUEST_SAMPLER_TABLES 128   /* 128 tables x 16 = the 2048-sampler heap limit */
typedef struct GuestSamplerHeap {
    struct GuestSamplerHeap* next;
    ID3D12DescriptorHeap* heap;
    unsigned count;
    unsigned long long hash[GUEST_SAMPLER_TABLES];
    D3D12_SAMPLER_DESC tables[GUEST_SAMPLER_TABLES][16];
} GuestSamplerHeap;
/* The base heap persists across frames so repeated sampler states keep their
 * table and the bound heap rarely changes. Overflow heaps exist only for the
 * frame that filled the base heap. */
static GuestSamplerHeap* s_sampler_base;
static GuestSamplerHeap* s_sampler_overflow;
static ID3D12DescriptorHeap* s_sampler_bound;
static unsigned s_sampler_guest_units, s_sampler_tables_made;
/* Sampled-unit masks by FP structural hash (filled by vp_get_fp_pso). */
static rsx_guest_sampler_mask_slot s_sampler_masks[RSX_GUEST_SAMPLER_MASKS];

static void guest_sampler_fail(const char* message)
{
    fprintf(stderr, "[RSXSAMPLER] %s\n", message); abort();
}
static GuestSamplerHeap* guest_sampler_heap_new(void)
{
    GuestSamplerHeap* h = (GuestSamplerHeap*)calloc(1, sizeof(*h));
    if (!h) guest_sampler_fail("table allocation failed");
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = GUEST_SAMPLER_TABLES * 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
        &IID_ID3D12DescriptorHeap, (void**)&h->heap);
    if (FAILED(hr) || !h->heap) {
        fprintf(stderr, "[RSXSAMPLER] CreateDescriptorHeap 0x%08lX removed=0x%08lX tables=%u\n",
                (unsigned long)hr,
                (unsigned long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device),
                s_sampler_tables_made);
        guest_sampler_fail("sampler heap allocation failed");
    }
    return h;
}
static unsigned long long guest_sampler_hash(const D3D12_SAMPLER_DESC* desc)
{
    const unsigned char* b = (const unsigned char*)desc;
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < 16 * sizeof(*desc); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* Record time: the draw keeps the sampler registers that were live when the
 * guest issued it, like blend_key/ds_key. */
static void guest_sampler_record(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    for (unsigned u = 0; u < 16; ++u) {
        rsx_guest_sampler_regs* r = &dr->sampler[u];
        if (!st) { memset(r, 0, sizeof(*r)); continue; }
        r->address  = st->textures[u].address;
        r->filter   = st->textures[u].filter;
        r->control0 = st->textures[u].control0;
        r->border   = st->textures[u].border_color;
    }
}
static void guest_sampler_note_mask(u64 ucode_hash, u32 mask)
{
    rsx_guest_sampler_mask_note(s_sampler_masks, ucode_hash, mask);
}
/* Units the record's program samples: its snapshot's structural hash, the same
 * identity vp_get_fp_pso compiles from. Unknown programs compare every unit. */
static unsigned guest_sampler_record_mask(const D3D12DrawRecord* dr)
{
    if (!dr->fp_snap || dr->fp_snap > VP_FPSNAP_MAX) return 0xFFFFu;
    return rsx_guest_sampler_mask_for(s_sampler_masks, s_fp_snap[dr->fp_snap - 1].hash);
}
/* Draw concatenation must not merge across a change on a unit the program
 * samples. Unsampled units, and units disabled on both sides, are ignored. */
static int guest_sampler_matches(const D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return 0;
    unsigned mask = guest_sampler_record_mask(dr);
    for (unsigned u = 0; u < 16; ++u) {
        if (!(mask & (1u << u))) continue;
        rsx_guest_sampler_regs now;
        now.address = st->textures[u].address;
        now.filter = st->textures[u].filter;
        now.control0 = st->textures[u].control0;
        now.border = st->textures[u].border_color;
        if (!rsx_guest_sampler_same(&dr->sampler[u], &now)) return 0;
    }
    return 1;
}
/* Reclaim last frame's tables. Only after vp_upload_retire proves the queue
 * completed (no device removal, no sentinel fence, fence reached): a failed
 * wait keeps every heap and slot, and the next bind appends instead. */
static int guest_sampler_retire(void)
{
    if (!vp_upload_retire(NULL)) {
        static unsigned logged;
        if (logged++ < 8)
            fprintf(stderr, "[RSXSAMPLER] GPU retirement failed; sampler tables preserved\n");
        return 0;
    }
    int overflowed = s_sampler_overflow != NULL;
    while (s_sampler_overflow) {
        GuestSamplerHeap* h = s_sampler_overflow;
        s_sampler_overflow = h->next;
        if (h->heap) h->heap->lpVtbl->Release(h->heap);
        free(h);
    }
    if (s_sampler_base && (overflowed || s_sampler_base->count == GUEST_SAMPLER_TABLES))
        s_sampler_base->count = 0;   /* rewrite from slot 0; nothing in flight */
    s_sampler_bound = NULL;
    return 1;
}
/* Device teardown: release every sampler heap after a checked retirement and
 * clear all owners, so a later initialization never binds descriptors from
 * the previous device. If retirement fails the heaps are leaked, never
 * released while possibly referenced; the owners are cleared either way. */
static void guest_sampler_shutdown(void)
{
    int retired = s_sampler_base || s_sampler_overflow ? vp_upload_retire(NULL) : 1;
    if (!retired)
        fprintf(stderr, "[RSXSAMPLER] shutdown retirement failed; sampler heaps leaked\n");
    GuestSamplerHeap* lists[2] = {s_sampler_base, s_sampler_overflow};
    for (unsigned i = 0; i < 2; ++i)
        while (lists[i]) {
            GuestSamplerHeap* h = lists[i];
            lists[i] = h->next;
            if (retired && h->heap) h->heap->lpVtbl->Release(h->heap);
            free(h);
        }
    s_sampler_base = s_sampler_overflow = NULL;
    s_sampler_bound = NULL;
    s_sampler_guest_units = s_sampler_tables_made = 0;
    memset(s_sampler_masks, 0, sizeof(s_sampler_masks));
}
static GuestSamplerHeap* guest_sampler_find(GuestSamplerHeap* h, unsigned long long hash,
                                            const D3D12_SAMPLER_DESC* desc, unsigned* slot)
{
    for (; h; h = h->next)
        for (unsigned i = 0; i < h->count; ++i)
            if (h->hash[i] == hash && !memcmp(desc, h->tables[i], 16 * sizeof(*desc))) {
                *slot = i; return h;
            }
    return NULL;
}
/* Bind this draw's table to root parameter 3. mask = units the bound guest
 * pixel shader samples (vp_get_fp_pso's s_pso_sampler_mask); 0 for the
 * fallback atlas/colour pipelines. Returns 1 when the shader-visible heaps
 * changed, which invalidates the caller's bound SRV table (root parameter 1). */
static int guest_sampler_bind(const D3D12DrawRecord* dr, unsigned mask)
{
    D3D12_SAMPLER_DESC desc[16];
    int set[16];
    for (unsigned u = 0; u < 16; ++u) set[u] = dr->tex[u].set;
    s_sampler_guest_units += rsx_guest_sampler_table(dr->sampler, set, mask,
        mask ? dr->depth_view : NULL, desc);

    if (!s_sampler_base) s_sampler_base = guest_sampler_heap_new();
    unsigned long long hash = guest_sampler_hash(desc);
    unsigned slot = 0;
    GuestSamplerHeap* h = guest_sampler_find(s_sampler_base, hash, desc, &slot);
    if (!h) h = guest_sampler_find(s_sampler_overflow, hash, desc, &slot);
    UINT inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (!h) {
        h = s_sampler_base;
        if (h->count == GUEST_SAMPLER_TABLES) {
            h = s_sampler_overflow;
            if (!h || h->count == GUEST_SAMPLER_TABLES) {
                static unsigned logged;
                if (logged++ < 8)
                    fprintf(stderr, "[RSXSAMPLER] overflow heap: frame needs more than %u tables\n",
                            GUEST_SAMPLER_TABLES);
                h = guest_sampler_heap_new();
                h->next = s_sampler_overflow; s_sampler_overflow = h;
            }
        }
        slot = h->count++;
        h->hash[slot] = hash;
        memcpy(h->tables[slot], desc, sizeof(desc));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        h->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(h->heap, &cpu);
        cpu.ptr += (SIZE_T)slot * 16 * inc;
        for (unsigned u = 0; u < 16; ++u) {
            s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, &desc[u], cpu);
            cpu.ptr += inc;
        }
        s_sampler_tables_made++;
    }
    int changed = s_sampler_bound != h->heap;
    if (changed) {
        ID3D12DescriptorHeap* heaps[] = {s_d3d.srv_heap, h->heap};
        s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 2, heaps);
        s_sampler_bound = h->heap;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    h->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(h->heap, &gpu);
    gpu.ptr += (UINT64)slot * 16 * inc;
    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 3, gpu);
    return changed;
}
