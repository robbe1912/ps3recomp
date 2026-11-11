/* Native D24 texture reads (#137). Included after the sampler tables.
 * Opt-in: RSX_DEPTH_TEXTURE=1. A unit the record's program reads, whose guest
 * format is D24_D8 and whose address is a live depth attachment of this batch,
 * binds that attachment through an R24_UNORM_X8 view and is compiled with a
 * _rsx_depthN helper (rsx_depth_sample.h). Everything else keeps the existing
 * path. Views are resolved after every attachment of the batch is prepared,
 * so the resources they name stay fixed until checked retirement. */
static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping);

static int native_depth_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_DEPTH_TEXTURE"); on = e && e[0] == '1' && !e[1]; }
    return on;
}

/* RSX_DEPTH_DEBUG=lit|shadow|stored|reference replaces the helper result to
 * isolate a wrong comparison input in a native run. Diagnostics only. */
static u32 native_depth_debug(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("RSX_DEPTH_DEBUG");
        static const char* names[] = {"", "lit", "shadow", "stored", "reference"};
        mode = 0;
        for (int i = 1; e && i < 5; ++i) if (!strcmp(e, names[i])) mode = i;
        if (mode) fprintf(stderr, "[RSXDEPTH] debug mode %s\n", names[mode]);
    }
    return (u32)mode;
}

static void native_depth_clear(D3D12DrawRecord* dr)
{
    memset(dr->depth_view, 0, sizeof(dr->depth_view));
    memset(dr->depth_sampled_slot, 0, sizeof(dr->depth_sampled_slot));
    dr->depth_sample_mask = 0;
    memset(dr->depth_alias_index, 0, sizeof(dr->depth_alias_index));
    memset(dr->depth_alias_order, 0, sizeof(dr->depth_alias_order));
    dr->depth_alias_mask = 0;
}

/* ---- Packed depth (A5) ---------------------------------------------------
 * RSX_DEPTH_ALIAS=1: a unit read as A8R8G8B8 (base format 0x85) at the address
 * of a live depth attachment whose last write was depth reads the Z24S8 word
 * the guest memory holds there. A compute pass packs the attachment into an
 * RGBA8 texture just before the consuming record: R=Z[15:8], G=Z[7:0],
 * B=stencil, A=Z[23:16] (the ARGB8 bytes of the big-endian word). A record
 * that also has the attachment bound as its DSV reads that copy. */
static int depth_alias_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_DEPTH_ALIAS"); on = e && e[0] == '1' && !e[1]; }
    return on;
}
static const char depth_alias_cs[] =
    "Texture2D<float> ztex : register(t0);\n"
    "Texture2D<uint2> stex : register(t1);\n"
    "RWTexture2D<unorm float4> outtex : register(u0);\n"
    "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {\n"
    " uint w,h; outtex.GetDimensions(w,h); if(p.x>=w || p.y>=h) return;\n"
    " uint z=(uint)round(saturate(ztex.Load(int3(p.xy,0)))*16777215.0);\n"
    " uint s=stex.Load(int3(p.xy,0)).y;\n"
    " outtex[p.xy]=float4((z>>8)&255,z&255,s,(z>>16)&255)/255.0;\n}\n";
typedef struct {
    ID3D12Resource* rgba;
    ID3D12Resource* depth;      /* attachment the descriptors name */
    unsigned slot, w, h;
    u64 batch; u32 order;       /* last conversion: batch and source record */
    D3D12_RESOURCE_STATES state;
} DepthAlias;
static DepthAlias s_alias[DEPTH_ALIAS_CAPACITY];
static unsigned s_alias_n;
static u64 s_alias_batch;
static u8 s_alias_colour_last[RSX_D3D12_DEPTH_CAPACITY + 1];   /* last write was colour */
static ID3D12RootSignature* s_alias_root;
static ID3D12PipelineState* s_alias_pso;
static int s_alias_broken;

static D3D12_CPU_DESCRIPTOR_HANDLE depth_alias_cpu(unsigned descriptor)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (SIZE_T)(SRV_HEAP_SIZE + descriptor) * s_d3d.srv_inc;
    return h;
}
static int depth_alias_pipeline(void)
{
    if (s_alias_pso) return 1;
    if (s_alias_broken) return 0;
    s_alias_broken = 1;
    D3D12_DESCRIPTOR_RANGE ranges[2];
    memset(ranges, 0, sizeof(ranges));
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER param; memset(&param, 0, sizeof(param));
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2; param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd, 0, sizeof(rd));
    rd.NumParameters = 1; rd.pParameters = &param;
    ID3DBlob *sig = NULL, *err = NULL, *cs = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr)) { fprintf(stderr, "[RSXDEPTH] alias root signature %08lX\n", (unsigned long)hr); return 0; }
    hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_alias_root);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) { fprintf(stderr, "[RSXDEPTH] alias root %08lX\n", (unsigned long)hr); return 0; }
    err = NULL;
    hr = D3DCompile(depth_alias_cs, sizeof(depth_alias_cs) - 1, "depth_alias", NULL, NULL,
                    "main", "cs_5_0", 0, 0, &cs, &err);
    if (err) { fprintf(stderr, "[RSXDEPTH] alias compile: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }
    if (FAILED(hr) || !cs) return 0;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = s_alias_root;
    pd.CS.pShaderBytecode = cs->lpVtbl->GetBufferPointer(cs);
    pd.CS.BytecodeLength = cs->lpVtbl->GetBufferSize(cs);
    hr = s_d3d.device->lpVtbl->CreateComputePipelineState(s_d3d.device, &pd,
        &IID_ID3D12PipelineState, (void**)&s_alias_pso);
    cs->lpVtbl->Release(cs);
    if (FAILED(hr)) { fprintf(stderr, "[RSXDEPTH] alias pipeline %08lX\n", (unsigned long)hr); s_alias_pso = NULL; return 0; }
    s_alias_broken = 0;
    return 1;
}
/* Runs during prepare, after checked retirement: replacing a resource or a
 * descriptor here never touches anything the GPU still reads. */
static int depth_alias_entry(unsigned slot, unsigned w, unsigned h)
{
    ID3D12Resource* depth = s_d3d.depth_cache.surfaces[slot - 1].resource;
    unsigned i;
    for (i = 0; i < s_alias_n; ++i)
        if (s_alias[i].slot == slot && s_alias[i].w == w && s_alias[i].h == h) break;
    if (i == s_alias_n) {
        if (s_alias_n == DEPTH_ALIAS_CAPACITY || !depth_alias_pipeline()) return -1;
        D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd; memset(&rd, 0, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* rgba = NULL;
        HRESULT hr = s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&rgba);
        if (FAILED(hr)) { fprintf(stderr, "[RSXDEPTH] alias texture %ux%u %08lX\n", w, h, (unsigned long)hr); return -1; }
        memset(&s_alias[i], 0, sizeof(s_alias[i]));
        s_alias[i].rgba = rgba; s_alias[i].slot = slot; s_alias[i].w = w; s_alias[i].h = h;
        s_alias[i].state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv; memset(&uv, 0, sizeof(uv));
        uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        s_d3d.device->lpVtbl->CreateUnorderedAccessView(s_d3d.device, rgba, NULL, &uv, depth_alias_cpu(i * 3 + 2));
        ++s_alias_n;
    }
    DepthAlias* a = &s_alias[i];
    if (a->depth != depth) {   /* new or regrown attachment */
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof(sv));
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, depth, &sv, depth_alias_cpu(i * 3));
        sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT; sv.Texture2D.PlaneSlice = 1;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, depth, &sv, depth_alias_cpu(i * 3 + 1));
        a->depth = depth; a->batch = 0;
    }
    return (int)i;
}
static void native_depth_batch_begin(void) { ++s_alias_batch; }

static void depth_alias_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
}
/* Before the consuming draw: convert each attachment once per source record. */
static void depth_alias_convert(const D3D12DrawRecord* dr, ID3D12PipelineState* graphics)
{
    int converted = 0;
    for (unsigned u = 0; u < 16; ++u) {
        if (!(dr->depth_alias_mask & (1u << u))) continue;
        DepthAlias* a = &s_alias[dr->depth_alias_index[u] - 1];
        if (a->batch == s_alias_batch && a->order == dr->depth_alias_order[u]) continue;
        a->batch = s_alias_batch; a->order = dr->depth_alias_order[u];
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &g);
        g.ptr += (UINT64)(SRV_HEAP_SIZE + (dr->depth_alias_index[u] - 1) * 3) * s_d3d.srv_inc;
        depth_alias_barrier(a->depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        depth_alias_barrier(a->rgba, a->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s_d3d.cmd_list->lpVtbl->SetComputeRootSignature(s_d3d.cmd_list, s_alias_root);
        s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, s_alias_pso);
        s_d3d.cmd_list->lpVtbl->SetComputeRootDescriptorTable(s_d3d.cmd_list, 0, g);
        s_d3d.cmd_list->lpVtbl->Dispatch(s_d3d.cmd_list, (a->w + 7) / 8, (a->h + 7) / 8, 1);
        depth_alias_barrier(a->rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        depth_alias_barrier(a->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        a->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        converted = 1;
    }
    /* Compute and graphics share the pipeline-state slot; root signatures,
     * heaps, targets, viewports and topology are untouched. */
    if (converted && graphics) s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, graphics);
}
static void native_depth_shutdown(void)
{
    for (unsigned i = 0; i < s_alias_n; ++i) if (s_alias[i].rgba) s_alias[i].rgba->lpVtbl->Release(s_alias[i].rgba);
    memset(s_alias, 0, sizeof(s_alias)); s_alias_n = 0;
    memset(s_alias_colour_last, 0, sizeof(s_alias_colour_last));
    if (s_alias_pso) s_alias_pso->lpVtbl->Release(s_alias_pso);
    if (s_alias_root) s_alias_root->lpVtbl->Release(s_alias_root);
    s_alias_pso = NULL; s_alias_root = NULL; s_alias_broken = 0;
}

/* One line per (address, format, reason), bounded; the first program seen is named. */
static void native_depth_notice(const D3D12DrawRecord* dr, unsigned u, const char* reason)
{
    static struct { u32 address, fmt; const char* reason; } seen[256];
    static unsigned n;
    for (unsigned i = 0; i < n; ++i)
        if (seen[i].address == dr->tex[u].raw && seen[i].fmt == dr->tex[u].fmt && seen[i].reason == reason) return;
    if (n == 256) return;
    seen[n].address = dr->tex[u].raw; seen[n].fmt = dr->tex[u].fmt; seen[n++].reason = reason;
    fprintf(stderr, "[RSXDEPTH] fp=%08X unit=%u address=%08X %ux%u fmt=%02X zfunc=%u %s\n",
            dr->fp_addr, u, dr->tex[u].raw, dr->tex[u].w, dr->tex[u].h, dr->tex[u].fmt,
            (dr->sampler[u].address >> 28) & 15u, reason);
}

static u32 native_depth_units(const D3D12DrawRecord* dr)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!vm_base || !dr->fp_addr) return 0;
    u32 off = cellGcmResolveLocated((dr->fp_addr & 3u) == 1, dr->fp_addr & ~3u);
    if (off == 0xFFFFFFFFu) return 0;
    const u8* uc = vp_fp_snap_bytes(dr->fp_snap, off);
    return rsx_fp_texture_units(uc ? uc : vm_base + off, 4096);
}

/* written[slot]: an earlier record of this batch renders that attachment, so
 * it is initialized before this record reads it. */
static unsigned native_depth_slot_at(u32 address)
{
    if (!address) return 0;
    for (unsigned i = 0; i < s_d3d.depth_cache.count; ++i)
        if (s_d3d.depth_cache.surfaces[i].resource && s_d3d.depth_cache.surfaces[i].offset == address) return i + 1;
    return 0;
}
static void depth_alias_prepare(D3D12DrawRecord* dr, unsigned u, const u8* written, const u32* last_user)
{
    unsigned slot = native_depth_slot_at(dr->tex[u].raw);
    if (!slot) return;                                       /* an ordinary colour texture */
    const rsx_d3d12_depth_surface* source = &s_d3d.depth_cache.surfaces[slot - 1];
    if (s_alias_colour_last[slot]) { native_depth_notice(dr, u, "alias_colour_last_skip"); return; }
    if (!source->initialized && !written[slot]) { native_depth_notice(dr, u, "alias_unwritten_skip"); return; }
    if (dr->tex[u].cube || dr->tex[u].mips > 1 || !dr->tex[u].w || !dr->tex[u].h ||
        dr->tex[u].w > source->width || dr->tex[u].h > source->height ||
        (dr->tex[u].ctrl1 && (dr->tex[u].ctrl1 & 0xFFFFu) != 0xAAE4u)) {
        native_depth_notice(dr, u, "alias_layout_skip"); return;
    }
    int index = depth_alias_entry(slot, dr->tex[u].w, dr->tex[u].h);
    if (index < 0) { native_depth_notice(dr, u, "alias_capacity_skip"); return; }
    dr->depth_alias_index[u] = (u8)(index + 1);
    dr->depth_alias_order[u] = last_user[slot];
    dr->depth_alias_mask |= 1u << u;
    native_depth_notice(dr, u, "alias_A5");
}
/* After this record's reads: what it writes, for the records after it. */
static void depth_alias_note_writes(const D3D12DrawRecord* dr, u32 index, u32* last_user)
{
    if (dr->depth_slot &&
        (dr->is_clear ? (dr->clear_flags & 3u) != 0 : ((dr->ds_key & RSX_DSK_DEPTH_EN) && (dr->ds_key & RSX_DSK_WRITE)))) {
        last_user[dr->depth_slot] = index + 1;      /* the last record that changed it */
        s_alias_colour_last[dr->depth_slot] = 0;
    }
    if (!dr->is_clear && dr->cmask) {
        const u32 targets[4] = {dr->rt_off, dr->rt_mrt[0], dr->rt_mrt[1], dr->rt_mrt[2]};
        for (unsigned t = 0; t < 4; ++t)
            if (((dr->cmask >> (4 * t)) & 15u) && targets[t]) {
                unsigned slot = native_depth_slot_at(targets[t]);
                if (slot) s_alias_colour_last[slot] = 1;
            }
    }
}

static void native_depth_prepare(D3D12DrawRecord* dr, u32 index, u8 written[RSX_D3D12_DEPTH_CAPACITY + 1],
                                 u32 last_user[RSX_D3D12_DEPTH_CAPACITY + 1])
{
    native_depth_clear(dr);
    if ((native_depth_on() || depth_alias_on()) && dr->is_vp && !dr->is_clear && dr->fp_addr) {
        u32 read = native_depth_units(dr);
        for (unsigned u = 0; u < 16; ++u) {
            if (!(read & (1u << u)) || !dr->tex[u].set) continue;
            const u32 base = dr->tex[u].fmt & 0x9Fu;
            if (base == 0x85u) {
                if (depth_alias_on()) depth_alias_prepare(dr, u, written, last_user);
                continue;
            }
            if (!native_depth_on()) continue;
            if (base == 0x91u || base == 0x92u || base == 0x93u) {
                native_depth_notice(dr, u, "other_depth_format_skip");   /* D24 float / D16 */
                continue;
            }
            if (!rsx_depth_sample_format(dr->tex[u].fmt)) continue;
            unsigned slot = 0;
            for (unsigned i = 0; i < s_d3d.depth_cache.count; ++i)
                if (s_d3d.depth_cache.surfaces[i].resource &&
                    s_d3d.depth_cache.surfaces[i].offset == dr->tex[u].raw) { slot = i + 1; break; }
            if (!slot) { native_depth_notice(dr, u, "no_depth_attachment"); continue; }
            const rsx_d3d12_depth_surface* source = &s_d3d.depth_cache.surfaces[slot - 1];
            if (slot == dr->depth_slot) { native_depth_notice(dr, u, "feedback_skip"); continue; }
            if (!source->initialized && !written[slot]) { native_depth_notice(dr, u, "unwritten_skip"); continue; }
            const rsx_guest_sampler_regs* r = &dr->sampler[u];
            int ok = rsx_depth_sample_view_init(&dr->depth_view[u], dr->tex[u].fmt, dr->tex[u].w,
                dr->tex[u].h, dr->tex[u].mips, dr->tex[u].cube, dr->tex[u].ctrl1, r->address,
                r->filter, r->control0, source->width, source->height);
            if (ok < 1) {
                memset(&dr->depth_view[u], 0, sizeof(dr->depth_view[u]));
                native_depth_notice(dr, u, ok < 0 ? "unsupported_view_skip" : "disabled_unit_skip");
                continue;
            }
            dr->depth_view[u].debug = native_depth_debug();
            dr->depth_sampled_slot[u] = (u8)slot;
            dr->depth_sample_mask |= 1u << u;
            native_depth_notice(dr, u, dr->depth_view[u].compare ? "native_compare" : "native_depth");
        }
    }
    if (dr->is_vp && dr->depth_slot) written[dr->depth_slot] = 1;
    if (dr->is_vp) depth_alias_note_writes(dr, index, last_user);
}

static int native_depth_bind(const D3D12DrawRecord* dr, unsigned u, u32 descriptor)
{
    if (dr->depth_alias_mask & (1u << u)) {
        srv_write(descriptor, s_alias[dr->depth_alias_index[u] - 1].rgba,
                  DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        return 1;
    }
    if (!(dr->depth_sample_mask & (1u << u))) return 0;
    srv_write(descriptor, s_d3d.depth_cache.surfaces[dr->depth_sampled_slot[u] - 1].resource,
              DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    return 1;
}

/* Attachments rest in DEPTH_WRITE. A record that reads one makes it a pixel
 * shader resource for that draw only. The record never binds it as its DSV
 * (feedback reads are skipped in prepare). */
static void native_depth_transition(const D3D12DrawRecord* dr, int begin)
{
    u8 done[RSX_D3D12_DEPTH_CAPACITY + 1] = {0};
    D3D12_RESOURCE_BARRIER barriers[16];
    UINT n = 0;
    for (unsigned u = 0; u < 16; ++u) {
        unsigned slot = dr->depth_sampled_slot[u];
        if (!(dr->depth_sample_mask & (1u << u)) || done[slot]) continue;
        done[slot] = 1;
        D3D12_RESOURCE_BARRIER* b = &barriers[n++];
        memset(b, 0, sizeof(*b));
        b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b->Transition.pResource = s_d3d.depth_cache.surfaces[slot - 1].resource;
        b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b->Transition.StateBefore = begin ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                          : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b->Transition.StateAfter = begin ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (n) s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, n, barriers);
}
static void native_depth_begin(const D3D12DrawRecord* dr, ID3D12PipelineState* graphics)
{
    if (dr->depth_alias_mask) depth_alias_convert(dr, graphics);
    native_depth_transition(dr, 1);
}
static void native_depth_end(const D3D12DrawRecord* dr) { native_depth_transition(dr, 0); }
