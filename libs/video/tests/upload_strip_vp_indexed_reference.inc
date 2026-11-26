static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    static rsx_index_strip cursor;
    static const rsx_state* owner;
    static u32 epoch;
    static int previous_fan;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    rsx_vertex_fetch_batch fetch_batch;
    rsx_vertex_fetch_prepare(&fetch_batch, state);
    if (s_vertex_profile_enabled) {
        ++s_vertex_profile.draws;
        if (s_vertex_profile.draws > 1) ++s_vertex_profile.repeats;
    }
    if (owner != state || epoch != state->begin_epoch || previous_fan != fan) {
        memset(&cursor, 0, sizeof cursor);
        owner = state; epoch = state->begin_epoch; previous_fan = fan;
    }
    vp_upload_reserve((u64)count * 3);
    u32 maxv = (s_d3d.vp_capacity * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * s_d3d.vp_capacity * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    /* Strip/fan triangles share vertices. Reuse their exact decoded bytes
     * only within this callback: all attribute/base/frequency state is fixed,
     * and nothing survives a later draw or guest-memory update. 64 entries,
     * two-way set-associative on the index with LRU per set: both shared strip
     * vertices and the fan center are retained across common strip/fan patterns.
     * Two ways also retain reuse between neighbouring strip rows. */
    const int use_cache = count > 3 || count + cursor.held > 3;
    enum { VP_STRIP_SETS = 32 };
    VPSlot cached_vertices[VP_STRIP_SETS][2][16];
    u32 cached_indices[VP_STRIP_SETS][2];
    u8 cached_state[VP_STRIP_SETS];   /* bit w: way w valid; bit 2: way to evict */
    if (use_cache) memset(cached_state, 0, sizeof cached_state);
    for (u32 k = 0; k < count; k++) {
        u32 triangle[3];
        if (!rsx_index_strip_push(&cursor, read_guest_index(state, first + k), fan,
                state->restart_index_enable, state->restart_index,
                ((state->index_array_dma >> 4) & 15u) != 1, triangle)) continue;
        if (maxv - o < 3) {
            fprintf(stderr, "[D3D12] indexed strip/fan vertex buffer exhausted\n");
            abort();
        }
        /* A fresh single triangle has no shared work to amortize a cache. */
        if (!use_cache) {
            if (s_vertex_profile_enabled)
                s_vertex_profile.direct_bytes += 3u * VP_VERT_STRIDE;
            for (u32 v = 0; v < 3; ++v)
                read_vp_vertex(state, &fetch_batch, triangle[v], &out[(o++)*16]);
            continue;
        }
        for (u32 v = 0; v < 3; v++) {
            const u32 set = triangle[v] & (VP_STRIP_SETS - 1);
            u32 bits = cached_state[set], way;
            int hit = 0;
            if ((bits & 1u) && cached_indices[set][0] == triangle[v]) { way = 0; hit = 1; }
            else if ((bits & 2u) && cached_indices[set][1] == triangle[v]) { way = 1; hit = 1; }
            else {
                way = !(bits & 1u) ? 0u : !(bits & 2u) ? 1u : (bits >> 2) & 1u;
                read_vp_vertex(state, &fetch_batch, triangle[v], cached_vertices[set][way]);
                cached_indices[set][way] = triangle[v];
                bits |= 1u << way;
            }
            if (s_vertex_profile_enabled) {
                if (hit) ++s_vertex_profile.cache_hits;
                else ++s_vertex_profile.cache_misses;
            }
            cached_state[set] = (u8)((bits & 3u) | ((way ^ 1u) << 2));
            const double started = s_vertex_profile_enabled ? perf_now() : 0.0;
            memcpy(&out[o*16], cached_vertices[set][way], sizeof cached_vertices[set][way]);
            if (s_vertex_profile_enabled) {
                s_vertex_profile.copy_bytes += sizeof cached_vertices[set][way];
                s_vertex_profile.copy_seconds += perf_now() - started;
            }
            ++o;
        }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* A compatible TRIANGLES continuation consumes no new record slot. Decide
 * before a capacity drain can discard the unfinished primitive tail. Keep the
 * predicate shared with the post-upload merge so reservation and merge agree. */
