/* rsx_deform_surface.inc -- CPU copy of a paint target that the game reads
 * back on the CPU (#346).
 *
 * MotorStorm paints wheel ruts into one 4096 x 4096 B8 surface with two
 * triangles per stamp and then reads single bytes of it on the CPU. Each read
 * is a GPU round trip. This file keeps level 0 of the surface in host memory,
 * replays the paint draws into it, and serves the reads from it.
 *
 * Every part is opt-in and off by default. The switches are read once:
 *   RSX_DEFORM_TARGETS=<hex offset>[,...]  RSX offsets of level 0 of the
 *                                          surfaces to own. Nothing is on
 *                                          without this list.
 *   RSX_DEFORM_CAPTURE=1                   section A: dump the first draws
 *                                          into the listed surfaces.
 *
 * rsx_d3d12_backend.c includes this file after rsx_cpu_surface.inc. It uses
 * the record type D3D12DrawRecord, rsx_state, s_d3d, vm_base, the vertex
 * fetch and the two shader decompilers of the backend.
 */
#include <stdatomic.h>
#include "rsx_vp_decompiler.h"

#ifndef RSX_DEFORM_TARGETS_MAX
#define RSX_DEFORM_TARGETS_MAX 4u
#endif
/* Records per run: one clear and the first draws. A draw holds up to 100
 * stamps (800 vertices), so 64 records cover many frames of painting. */
#ifndef RSX_DEFORM_CAPTURE_MAX
#define RSX_DEFORM_CAPTURE_MAX 64u
#endif
/* Upper bound of one fragment program in guest memory (VP_FP_UCODE_MAX). */
#define RSX_DEFORM_FP_UCODE_MAX 16384u

typedef struct {
    _Atomic int config_state;     /* -1 unread, 1 a thread reads it, 2 on, 0 off */
    int        any;               /* 1 when one switch is on                  */
    u32        offsets[RSX_DEFORM_TARGETS_MAX];
    int        n_offsets;
    int        capture;           /* RSX_DEFORM_CAPTURE=1                     */
} RsxDeformConfig;

static RsxDeformConfig s_deform = { -1, 0, { 0 }, 0, 0 };

/* Comma separated hex offsets. Returns the count, at most RSX_DEFORM_TARGETS_MAX. */
static int deform_read_offsets(const char* list, u32* out)
{
    int n = 0;
    const char* p = list;
    while (p && *p && n < (int)RSX_DEFORM_TARGETS_MAX) {
        char* end = NULL;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        out[n++] = (u32)v;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}

static void deform_config_read(void)
{
    const char* targets = getenv("RSX_DEFORM_TARGETS");
    if (!targets || !*targets) return;
    s_deform.n_offsets = deform_read_offsets(targets, s_deform.offsets);
    if (!s_deform.n_offsets) return;
    const char* capture = getenv("RSX_DEFORM_CAPTURE");
    s_deform.capture = capture && *capture && strcmp(capture, "0") != 0;
    s_deform.any = s_deform.capture;
}

/* 1 when a switch is on. 0 when all are off, or while another thread reads
 * them: that call takes the GPU path, which is always correct. */
static int deform_on(void)
{
    int st = atomic_load_explicit(&s_deform.config_state, memory_order_acquire);
    if (st == -1) {
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&s_deform.config_state, &expected, 1,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            deform_config_read();
            atomic_store_explicit(&s_deform.config_state, s_deform.any ? 2 : 0,
                                  memory_order_release);
        } else {
            st = atomic_load_explicit(&s_deform.config_state, memory_order_acquire);
        }
    }
    return st == 2;
}

static int deform_listed(u32 offset)
{
    for (int i = 0; i < s_deform.n_offsets; i++)
        if (s_deform.offsets[i] == offset) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * A. Capture: the inputs of the paint draws.
 *
 * RSX_DEFORM_CAPTURE=1 and RSX_DEFORM_TARGETS write one record per clear and
 * per draw whose colour target is a listed offset, up to RSX_DEFORM_CAPTURE_MAX
 * records. The file is rsx_deform_draws.txt next to rsx_phases.csv, so it
 * needs the RSX_PHASE_CSV path. Each record holds the state words the backend
 * replays the draw with, the vertex stream, and the decompiled programs. The
 * shader text is printed once per program; later records name the record
 * that holds it.
 */

typedef struct {
    FILE* file;
    u32   count;        /* records written                              */
    u32   fp_seen[16];  /* fp_addr of a printed fragment program        */
    u32   fp_record[16];
    int   fp_n;
    u32   vp_seen[16];  /* start slot of a printed vertex program       */
    u32   vp_record[16];
    int   vp_n;
} RsxDeformCapture;

static RsxDeformCapture s_deform_capture;

static void deform_capture_open(void)
{
    const char* phases = getenv("RSX_PHASE_CSV");
    char path[1024];
    const char* slash = phases ? strrchr(phases, '\\') : NULL;
    const char* other = phases ? strrchr(phases, '/') : NULL;
    if (other && (!slash || other > slash)) slash = other;
    if (!phases || !*phases
        || (size_t)(slash ? slash - phases + 1 : 0) + sizeof "rsx_deform_draws.txt" > sizeof path) {
        fprintf(stderr, "[DEFORM_CAPTURE] off: RSX_DEFORM_CAPTURE needs the"
                        " RSX_PHASE_CSV path\n");
        s_deform.capture = 0;
        return;
    }
    size_t dir = slash ? (size_t)(slash - phases) + 1 : 0;
    memcpy(path, phases, dir);
    memcpy(path + dir, "rsx_deform_draws.txt", sizeof "rsx_deform_draws.txt");
    s_deform_capture.file = fopen(path, "wb");
    if (!s_deform_capture.file) {
        fprintf(stderr, "[DEFORM_CAPTURE] off: cannot open %s\n", path);
        s_deform.capture = 0;
        return;
    }
    fprintf(stderr, "[DEFORM_CAPTURE] on: %d target%s, first %u records into %s\n",
            s_deform.n_offsets, s_deform.n_offsets == 1 ? "" : "s",
            (u32)RSX_DEFORM_CAPTURE_MAX, path);
}

/* Index k of the guest index array. Same decode as read_guest_index. */
static u32 deform_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    const u8* p = vm_base + cellGcmResolveLocated(local, st->index_array_offset);
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void deform_capture_hex(FILE* f, const u8* p, u32 n)
{
    for (u32 i = 0; i < n; i++) fprintf(f, "%02X", p[i]);
}

static void deform_capture_vertex_bytes(FILE* f, const rsx_state* st, const int* arrays,
                                        int na, u32 vi)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    for (int j = 0; j < na; j++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[arrays[j]];
        int main_memory = (a->offset & 0x80000000u) ? 0 : 1;
        u32 vm = cellGcmResolveLocated(main_memory,
                                       (a->offset & 0x7FFFFFFFu) + vi * a->stride);
        fprintf(f, " bytes[%d]=@0x%08X:", arrays[j], vm);
        deform_capture_hex(f, vm_base + vm, 64);
    }
    fprintf(f, "\n");
}

/* The vertex stream of the draw: the declarations, then the first vertices.
 * Every enabled array is decoded with the fetch of the backend. */
static void deform_capture_stream(FILE* f, const rsx_state* st, u32 first, u32 count,
                                  int indexed)
{
    int arrays[16], na = 0;
    for (int i = 0; i < 16; i++)
        if (st->vertex_attribs[i].enabled && st->vertex_attribs[i].stride)
            arrays[na++] = i;
    fprintf(f, " stream input_mask=0x%X output_mask=0x%X frequency_divider_op=0x%X"
               " index_array_offset=0x%08X index_array_dma=0x%X arrays=%d\n",
            st->vertex_attrib_input_mask, st->vertex_attrib_output_mask,
            st->frequency_divider_op, st->index_array_offset, st->index_array_dma, na);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        fprintf(f, " attr[%d] enabled=%d size=%u type=%u stride=%u offset=0x%08X"
                   " format=0x%08X frequency=%u constant=%g,%g,%g,%g\n",
                i, a->enabled, a->size, a->type, a->stride, a->offset, a->format,
                a->frequency, st->vertex_data4f[i][0], st->vertex_data4f[i][1],
                st->vertex_data4f[i][2], st->vertex_data4f[i][3]);
    }
    u32 nv = count < 16u ? count : 16u;
    for (u32 k = 0; k < nv; k++) {
        u32 vi = indexed ? deform_guest_index(st, first + k) : first + k;
        fprintf(f, " v%u index=%u", k, vi);
        for (int j = 0; j < na; j++) {
            float v[4];
            rsx_fetch_attrib(st, arrays[j], vi, v);
            fprintf(f, " a%d=%g,%g,%g,%g", arrays[j], v[0], v[1], v[2], v[3]);
        }
        fprintf(f, "\n");
        if (k < 6u) deform_capture_vertex_bytes(f, st, arrays, na, vi);
    }
}

/* Print a program once per identity. later records name the first record. */
static void deform_capture_programs(FILE* f, const D3D12DrawRecord* dr, const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    static char text[262144];
    /* Fragment program: the guest address names it. The low two bits of
     * SET_SHADER_PROGRAM are the location + 1 (1 = local memory). */
    if (dr->fp_addr) {
        int seen = -1;
        for (int i = 0; i < s_deform_capture.fp_n; i++)
            if (s_deform_capture.fp_seen[i] == dr->fp_addr) seen = i;
        if (seen < 0) {
            u32 vm = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1, dr->fp_addr & ~0x3u);
            const u8* ucode = vm_base + vm;
            u32 size = rsx_fp_program_size(ucode, RSX_DEFORM_FP_UCODE_MAX);
            int n = rsx_fp_decompile(ucode, RSX_DEFORM_FP_UCODE_MAX, st->shader_control,
                                     text, (u32)sizeof text);
            fprintf(f, " fp_text=record=%u fp_vm=0x%08X fp_ucode_bytes=%u"
                       " fp_instrs=%d\n fp_ucode=", s_deform_capture.count, vm, size, n);
            for (u32 b = 0; b + 4 <= size; b += 4)
                fprintf(f, " %02X%02X%02X%02X", ucode[b], ucode[b + 1], ucode[b + 2],
                        ucode[b + 3]);
            fprintf(f, "\nFP_HLSL_BEGIN\n%s\nFP_HLSL_END\n", n > 0 ? text : "");
            if (s_deform_capture.fp_n < 16) {
                s_deform_capture.fp_seen[s_deform_capture.fp_n] = dr->fp_addr;
                s_deform_capture.fp_record[s_deform_capture.fp_n] = s_deform_capture.count;
                s_deform_capture.fp_n++;
            }
        } else {
            fprintf(f, " fp_text=record=%u\n", s_deform_capture.fp_record[seen]);
        }
    }
    /* Vertex program: from the start slot SET_TRANSFORM_PROGRAM_START names
     * through the end of the store. The selected program comes first. */
    if (st->vp_ucode_bytes >= 16u) {
        u32 start = st->transform_program_start * 16u;
        int seen = -1;
        if (start >= st->vp_ucode_bytes) start = 0;
        for (int i = 0; i < s_deform_capture.vp_n; i++)
            if (s_deform_capture.vp_seen[i] == start) seen = i;
        if (seen < 0) {
            rsx_vp_set_branch_base(start / 16u);
            int n = rsx_vp_decompile(st->vp_ucode + start, st->vp_ucode_bytes - start,
                                     text, (u32)sizeof text);
            fprintf(f, " vp_text=record=%u vp_start_slot=%u vp_store_bytes=%u"
                       " vp_instrs=%d vp_vs_idx=%d\nVP_HLSL_BEGIN\n%s\nVP_HLSL_END\n",
                    s_deform_capture.count, start / 16u, st->vp_ucode_bytes, n, dr->vs_idx,
                    n > 0 ? text : "");
            if (s_deform_capture.vp_n < 16) {
                s_deform_capture.vp_seen[s_deform_capture.vp_n] = start;
                s_deform_capture.vp_record[s_deform_capture.vp_n] = s_deform_capture.count;
                s_deform_capture.vp_n++;
            }
        } else {
            fprintf(f, " vp_text=record=%u vp_start_slot=%u vp_vs_idx=%d\n",
                    s_deform_capture.vp_record[seen], start / 16u, dr->vs_idx);
        }
    }
}

static void deform_capture_record(const D3D12DrawRecord* dr, u32 first, u32 count, int indexed)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    FILE* f = s_deform_capture.file;
    if (!st) return;
    s_deform_capture.count++;
    fprintf(f, "[DEFORM_CAPTURE] record=%u frame=%llu kind=%s\n", s_deform_capture.count,
            (unsigned long long)s_d3d.frame_count, dr->is_clear ? "clear" : "draw");
    fprintf(f, " target rt=0x%08X rt_mrt=0x%08X,0x%08X,0x%08X rt_wh=%u,%u rt_fmt=0x%X"
               " zeta=0x%08X\n",
            dr->rt_off, dr->rt_mrt[0], dr->rt_mrt[1], dr->rt_mrt[2], dr->rt_w, dr->rt_h,
            dr->rt_fmt, st->surface_zeta_offset);
    fprintf(f, " surface format=0x%X color_target=0x%X clip_xy=%u,%u clip_wh=%u,%u\n",
            st->surface_format, st->color_target, st->surface_clip_x, st->surface_clip_y,
            st->surface_clip_w, st->surface_clip_h);
    fprintf(f, " color_offset=0x%08X,0x%08X,0x%08X,0x%08X"
               " color_pitch=0x%X,0x%X,0x%X,0x%X color_dma=0x%X,0x%X,0x%X,0x%X\n",
            st->surface_color_offset[0], st->surface_color_offset[1],
            st->surface_color_offset[2], st->surface_color_offset[3],
            st->surface_color_pitch[0], st->surface_color_pitch[1],
            st->surface_color_pitch[2], st->surface_color_pitch[3],
            st->surface_color_dma[0], st->surface_color_dma[1], st->surface_color_dma[2],
            st->surface_color_dma[3]);
    fprintf(f, " zeta_offset=0x%08X zeta_pitch=0x%X clear_color=0x%08X"
               " clear_zs=0x%08X\n",
            st->surface_zeta_offset, st->surface_zeta_pitch, st->color_clear_value,
            st->zstencil_clear_value);
    fprintf(f, " viewport_xywh=%u,%u,%u,%u viewport_offset=%g,%g,%g,%g"
               " viewport_scale=%g,%g,%g,%g clip_minmax=%g,%g\n",
            st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h,
            st->viewport_offset[0], st->viewport_offset[1], st->viewport_offset[2],
            st->viewport_offset[3], st->viewport_scale[0], st->viewport_scale[1],
            st->viewport_scale[2], st->viewport_scale[3], st->clip_min, st->clip_max);
    fprintf(f, " scissor_xywh=%u,%u,%u,%u\n",
            st->scissor_x, st->scissor_y, st->scissor_w, st->scissor_h);
    fprintf(f, " blend enable=%d enable_mrt=0x%X sfactor=0x%X dfactor=0x%X equation=0x%X"
               " color=0x%08X record_enable=%d record_key=0x%X\n",
            st->blend_enable, st->blend_enable_mrt, st->blend_sfactor, st->blend_dfactor,
            st->blend_equation, st->blend_color, dr->blend, dr->blend_key);
    fprintf(f, " depth test_enable=%d func=0x%X mask=%d\n",
            st->depth_test_enable, st->depth_func, st->depth_mask);
    fprintf(f, " stencil test_enable=%d func=0x%X ref=0x%X mask=0x%X op_fail=0x%X"
               " op_zfail=0x%X op_zpass=0x%X\n",
            st->stencil_test_enable, st->stencil_func, st->stencil_ref, st->stencil_mask,
            st->stencil_op_fail, st->stencil_op_zfail, st->stencil_op_zpass);
    fprintf(f, " color write mask=0x%X mask_mrt=0x%X record_cmask=0x%X"
               " alpha test_enable=%d func=0x%X ref=0x%X\n",
            st->color_mask, st->color_mask_mrt, dr->cmask, st->alpha_test_enable,
            st->alpha_func, st->alpha_ref);
    for (int u = 0; u < 16; u++) {
        const rsx_texture_state* t = &st->textures[u];
        if (!(t->format | t->offset)) continue;
        fprintf(f, " tex[%d] offset=0x%08X format=0x%08X pitch=0x%X image_rect=0x%08X"
                   " wh=%u,%u address=0x%08X filter=0x%08X control0=0x%08X control1=0x%08X"
                   " control3=0x%08X border=0x%08X\n",
                u, t->offset, t->format, t->control3 & 0xFFFFFu, t->image_rect,
                (t->image_rect >> 16) & 0xFFFFu, t->image_rect & 0xFFFFu, t->address,
                t->filter, t->control0, t->control1, t->control3, t->border_color);
    }
    if (dr->is_clear) {
        fprintf(f, " clear color=%g,%g,%g,%g\n",
                dr->cc[0], dr->cc[1], dr->cc[2], dr->cc[3]);
        fflush(f);
        return;
    }
    fprintf(f, " draw primitive_type=%u record_topology=%u record_vertex_count=%u"
               " indexed=%d first=%u count=%u\n",
            st->primitive_type, dr->topology, dr->vertex_count, indexed, first, count);
    deform_capture_stream(f, st, first, count, indexed);
    deform_capture_programs(f, dr, st);
    fflush(f);
}

/* Hook of the backend: after the clear record fields are set in d3d12_clear
 * or record_surface_clear, and next to cpu_surface_capture(dr) at the three
 * draw record sites. first and count are the arguments of the draw; indexed
 * is 1 when the vertices come through the guest index array. */
static void rsx_deform_capture(const D3D12DrawRecord* dr, u32 first, u32 count, int indexed)
{
    if (!deform_on() || !s_deform.capture) return;
    if (!s_deform_capture.file) {
        deform_capture_open();
        if (!s_deform.capture) return;
    }
    if (s_deform_capture.count >= RSX_DEFORM_CAPTURE_MAX) return;
    if (!deform_listed(dr->rt_off) && !deform_listed(dr->rt_mrt[0])
        && !deform_listed(dr->rt_mrt[1]) && !deform_listed(dr->rt_mrt[2]))
        return;
    deform_capture_record(dr, first, count, indexed);
    if (s_deform_capture.count == RSX_DEFORM_CAPTURE_MAX)
        fprintf(stderr, "[DEFORM_CAPTURE] full: %u records written\n",
                s_deform_capture.count);
}
