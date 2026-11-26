/* RSX draw merge: one DrawInstanced per run of records that draw with the
 * state the VP pass bound for the first record of the run.
 *
 * Switch: RSX_DRAW_MERGE=1. Unset or 0 keeps one draw per record. Read once.
 * The switch is off while VP_PROBE is set: that debug path clears a target
 * before each record.
 *
 * The scan runs at the draw issue of the VP pass, after the head record bound
 * its whole state. A record joins the run only when the VP pass would issue
 * no command for it and its shaders would read the bytes of the head:
 *  - it is a VP record, not a clear, with the triangle list topology, and its
 *    vertices follow those of the run without a gap;
 *  - same targets, depth surface, viewport rect and stencil reference;
 *  - it reads no depth surface (no barrier, no conversion, no depth sampler),
 *    and its texture units reference the same offscreen targets;
 *  - same guest pipeline: the PSO lookup of the VP pass gives the pipeline,
 *    the sampled units and the fallback choice of the head;
 *  - same SRV window and the same set flags; the same sampler registers on
 *    the units that the program samples (the inputs of the sampler table);
 *  - same b0 bytes, and the same b1 bytes after the b1 refresh of the VP pass.
 * The GPU then draws the vertices of the run in their order under the state
 * of the head.
 *
 * A run ends at each record that is not such a tail: a clear, a legacy
 * record, a record without its target, the first use of a depth surface, a
 * record with another state or other constant bytes. RSX_SUBMIT_DRAWS and
 * MAX_DRAWS end a batch between two records, so a run never crosses a batch.
 * The record row and the timestamps stay one per DrawInstanced: a merged row
 * shows the first record of the run.
 *
 * The [RSXPHASE] report prints one more line per report window while the
 * switch merges: runs with two or more records, guest draws, DrawInstanced.
 *
 * Include after vp_select_draw_pso. Check: tests/check_rsx_draw_merge.py. */

static int s_draw_merge_mode = 0;   /* 0 = not read, 1 = off, 2 = on */

static int draw_merge_on(void)
{
    if (!s_draw_merge_mode) {
        const char* e = rsx_env_once("RSX_DRAW_MERGE");
        s_draw_merge_mode = e && atoi(e) > 0 && !rsx_env_once("VP_PROBE") ? 2 : 1;
    }
    return s_draw_merge_mode == 2;
}

/* State that the VP pass bound for the head of a run. */
typedef struct {
    const D3D12DrawRecord* head;
    u32                 head_index;
    ID3D12PipelineState* pso;          /* guest pipeline, NULL = fallback */
    u32                 sampler_mask;  /* units the program of the head samples */
    ID3D12PipelineState* selected;     /* pipeline bound for the head */
    u64                 vb_end;        /* byte after the last vertex of the run */
} DrawMergeRun;

/* A record starts a run when its own draw needs no per-record work after the
 * state is bound, and its vertices are whole triangles of a list: a draw of
 * another topology drops its last vertices, and they must not join the next
 * record. A depth read transitions and samples the depth surface around the
 * draw of its own record. */
static int draw_merge_head_ok(const D3D12DrawRecord* dr)
{
    return !dr->depth_sample_mask && !dr->depth_alias_mask && dr->vertex_count &&
           dr->topology == RSX_TOPOLOGY_TRIANGLES;
}

/* Does record `index` draw with the state of the run? The PSO lookup of the
 * tail comes last: it is the costly test. */
static int draw_merge_tail(DrawMergeRun* run, u32 index)
{
    const D3D12DrawRecord* h = run->head;
    const D3D12DrawRecord* t = &s_d3d.draws[index];
    if (!t->is_vp || t->is_clear || t->topology != RSX_TOPOLOGY_TRIANGLES ||
        !t->vertex_count || t->vb_byte_offset != run->vb_end)
        return 0;
    if (t->rt_off != h->rt_off || t->rt_mrt[0] != h->rt_mrt[0] ||
        t->rt_mrt[1] != h->rt_mrt[1] || t->rt_mrt[2] != h->rt_mrt[2] ||
        t->depth_slot != h->depth_slot || t->rt_fmt != h->rt_fmt ||
        t->vp_x != h->vp_x || t->vp_y != h->vp_y || t->vp_w != h->vp_w ||
        t->vp_h != h->vp_h || t->stencil_ref != h->stencil_ref)
        return 0;
    if (t->depth_sample_mask || t->depth_alias_mask || t->fp_addr != h->fp_addr)
        return 0;
    /* The texture transitions read the offscreen target of each unit. The
     * sampler table reads the set flags, and the sampler registers of the
     * units that the program samples (rsx_guest_sampler_table). */
    for (u32 u = 0; u < 16; u++) {
        if (t->tex[u].set != h->tex[u].set) return 0;
        if (((run->sampler_mask >> u) & 1u) && h->tex[u].set &&
            memcmp(&t->sampler[u], &h->sampler[u], sizeof h->sampler[u])) return 0;
    }
    if (memcmp(t->tex_rt, h->tex_rt, sizeof t->tex_rt)) return 0;
    if (s_draw_srv_base[index] != s_draw_srv_base[run->head_index]) return 0;
    /* The vertex program reads b0. */
    if (!s_d3d.vp_cb_mapped) return 0;
    {
        const unsigned char* b0h = (const unsigned char*)s_d3d.vp_cb_mapped +
            ((u64)s_d3d.vp_parity * MAX_DRAWS + h->cb_slot) * VP_CB_STRIDE;
        const unsigned char* b0t = (const unsigned char*)s_d3d.vp_cb_mapped +
            ((u64)s_d3d.vp_parity * MAX_DRAWS + t->cb_slot) * VP_CB_STRIDE;
        if (memcmp(b0h, b0t, PHASE_DRAW_B0_BYTES)) return 0;
    }
    /* The pipeline, through the lookup of the VP pass. */
    ID3D12PipelineState* pso = NULL;
    u32 sampler_mask = 0, pso_n = 0;
    if (t->fp_addr) {
        u32 udim[16][2]; u16 n = 0;
        pso = vp_get_fp_pso(t->vs_idx, t->fp_addr, t->blend_key, dr_num_rts(t),
                           dr_rt_dxgi(t), t->fp_ctrl, dr_alpha_key(t),
                           dr_unnorm(t, udim), udim, t->cmask, t->cull,
                           t->bias_key, dr_cube_mask(t), t->ds_key, t->fp_snap,
                           &n, t);
        sampler_mask = s_pso_sampler_mask;
        pso_n = n;
    }
    if (pso != run->pso || sampler_mask != run->sampler_mask ||
        vp_select_draw_pso(t, pso) != run->selected)
        return 0;
    /* b1: the refresh of the VP pass, then the bytes the program reads. */
    if (s_d3d.vp_fpcb_mapped) {
        if (pso_n && t->fp_const_n != pso_n) {
            vp_record_fp_b1(t->cb_slot, (D3D12DrawRecord*)t);
            if (t->fp_const_n != pso_n)
                vp_serve_pso_consts(t->cb_slot, (D3D12DrawRecord*)t, pso_n);
        }
        if (t->fp_const_n != h->fp_const_n) return 0;
        u32 n = t->fp_const_n;
        u32 bytes = n >= VP_FPCB_MAX_CONSTS ? VP_FPCB_STRIDE : (n ? n + 1u : 2u) * 16u;
        const unsigned char* b1h = (const unsigned char*)s_d3d.vp_fpcb_mapped +
            ((u64)s_d3d.vp_parity * MAX_DRAWS + h->cb_slot) * VP_FPCB_STRIDE;
        const unsigned char* b1t = (const unsigned char*)s_d3d.vp_fpcb_mapped +
            ((u64)s_d3d.vp_parity * MAX_DRAWS + t->cb_slot) * VP_FPCB_STRIDE;
        if (memcmp(b1h, b1t, bytes)) return 0;
    }
    return 1;
}

/* Number of records of the run that starts at record `head`: 1 when no record
 * joins. `verts` gets the vertex count of the run. */
static u32 draw_merge_scan(u32 head, ID3D12PipelineState* pso, u32 sampler_mask,
                           ID3D12PipelineState* selected, u32* verts)
{
    DrawMergeRun run;
    memset(&run, 0, sizeof run);
    run.head = &s_d3d.draws[head];
    run.head_index = head;
    run.pso = pso;
    run.sampler_mask = sampler_mask;
    run.selected = selected;
    run.vb_end = (u64)run.head->vb_byte_offset + (u64)run.head->vertex_count * VP_VERT_STRIDE;
    u32 n = 1, count = run.head->vertex_count;
    for (u32 t = head + 1; t < s_d3d.draw_count && t < MAX_DRAWS; t++) {
        if (!draw_merge_tail(&run, t)) break;
        n++;
        count += s_d3d.draws[t].vertex_count;
        run.vb_end += (u64)s_d3d.draws[t].vertex_count * VP_VERT_STRIDE;
    }
    *verts = count;
    return n;
}

static u64 s_draw_merge_runs, s_draw_merge_saved;

static void draw_merge_count(u32 n)
{
    if (phase_on()) { s_draw_merge_runs++; s_draw_merge_saved += n - 1u; }
}

/* One line per report window of the phase statistics, after a window with a
 * merged run. `draws` is the count of DrawInstanced of the window. */
static void draw_merge_report(unsigned long tid, u64 frames, u64 draws)
{
    if (s_draw_merge_runs)
        fprintf(stderr, "[RSXPHASE] tid=%lu draw merge: frames=%llu runs=%llu draws=%llu -> %llu\n",
                tid, (unsigned long long)frames, (unsigned long long)s_draw_merge_runs,
                (unsigned long long)(draws + s_draw_merge_saved), (unsigned long long)draws);
    s_draw_merge_runs = s_draw_merge_saved = 0;
}
