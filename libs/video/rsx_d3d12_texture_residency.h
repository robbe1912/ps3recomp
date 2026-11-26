/* Included in the D3D12 backend after its state and retirement helpers.
 * Account owned default+upload allocation footprints, not driver residency.
 * Indispensable batch pins may exceed the target; never drop their draws. */
#ifndef RSX_D3D12_TEXTURE_RESIDENCY_H
#define RSX_D3D12_TEXTURE_RESIDENCY_H
static u64 texture_retention_budget(void)
{
    static u64 budget = ~(u64)0;
    if (budget == ~(u64)0) {
        const char* e = getenv("RSX_TEXTURE_CACHE_MIB");
        unsigned long mib = 512;
        if (e) {
            char* end = NULL; mib = strtoul(e, &end, 10);
            if (*e < '0' || *e > '9' || *end || mib > 4096) {
                fprintf(stderr, "[RSXTEXTURE] invalid RSX_TEXTURE_CACHE_MIB; use 0..4096\n"); abort();
            }
        }
        budget = (u64)mib * 1024 * 1024;
    }
    return budget;
}
static u64 texture_pinned_bytes(void)
{
    u64 total = 0;
    for (unsigned i = 0; i < s_d3d.vp_tex_capacity; ++i)
        if (s_d3d.vp_tex[i].used) total += s_d3d.vp_tex[i].allocation_bytes;
    return total;
}
static void texture_slot_release(VPTexSlot* t)
{
    if (t->used || t->allocation_bytes > s_d3d.vp_tex_bytes) {
        fprintf(stderr, "[RSXTEXTURE] attempted pinned release or invalid byte accounting\n"); abort();
    }
    if (t->res) t->res->lpVtbl->Release(t->res);
    if (t->up) t->up->lpVtbl->Release(t->up);
    s_d3d.vp_tex_bytes -= t->allocation_bytes;
    memset(t, 0, sizeof(*t));
}
static void texture_retention_trim(void)
{
    const u64 budget = texture_retention_budget();
    while (s_d3d.vp_tex_bytes > budget) {
        int victim = -1; u64 oldest_age = 0;
        for (unsigned i = 0; i < s_d3d.vp_tex_capacity; ++i) {
            VPTexSlot* t = &s_d3d.vp_tex[i];
            if (!t->used && t->allocation_bytes) {
                const u64 age = s_d3d.vp_tex_use_serial - t->last_use;
                if (victim < 0 || age > oldest_age) { victim = (int)i; oldest_age = age; }
            }
        }
        if (victim < 0) {
            const u64 over = s_d3d.vp_tex_bytes - budget;
            if (over > s_d3d.vp_tex_peak_over_budget) s_d3d.vp_tex_peak_over_budget = over;
            return;
        }
        texture_slot_release(&s_d3d.vp_tex[victim]); ++s_d3d.vp_tex_evictions;
    }
}
/* Sole unpin boundary: callers first complete checked GPU retirement. */
static void texture_retention_begin_batch(void)
{
    for (unsigned i = 0; i < s_d3d.vp_tex_capacity; ++i) s_d3d.vp_tex[i].used = 0;
    if (texture_retention_on()) texture_retention_trim();
}
static void texture_resource_fail(VPTexSlot* t, const char* operation, HRESULT hr)
{
    ++s_d3d.vp_tex_resource_failures;
    fprintf(stderr, "[RSXTEXTURE] %s failed: 0x%08lX\n", operation, (unsigned long)hr);
    texture_slot_release(t); abort();
}
#endif
