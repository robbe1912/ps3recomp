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
 *   RSX_DEFORM_CHAIN=1                     section A: also dump the 64
 *                                          records that follow in FIFO order.
 *   RSX_DEFORM_BUFFER=1                    section B: keep level 0 of the
 *                                          listed surfaces in guest memory.
 *   RSX_DEFORM_SHADOW=1                    section C: with the buffer, paint
 *                                          a private copy and compare.
 *   RSX_DEFORM_OP=<name>                   the pixel operation (section B).
 *   RSX_DEFORM_LOG=<frames>                one [DEFORM] line each n frames
 *                                          that paint (300).
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
/* Records of the chain: the passes that follow the first record of the dump. */
#ifndef RSX_DEFORM_CHAIN_MAX
#define RSX_DEFORM_CHAIN_MAX 64u
#endif
/* Upper bound of one fragment program in guest memory (VP_FP_UCODE_MAX). */
#define RSX_DEFORM_FP_UCODE_MAX 16384u

/* ---- the pixel operation of a paint draw (section B) --------------------
 * dest is the byte of the target, brush the byte of the brush texture and a
 * the first colour byte of the vertex, the intensity. All are 0..255, and so
 * is the result. The names are the values of RSX_DEFORM_OP. */
static u8 deform_op_f32(u32 dest, u32 brush, u32 a)
{
    /* The program in double precision, without the half precision steps. */
    double value = ((double)brush / 255.0 - 0.25) * ((double)a / 255.0) + (double)dest / 255.0;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    return (u8)(value * 255.0 + 0.5);
}

/* float to half to float, round to nearest even: f16tof32(f32tof16(x)). */
static float deform_f16(float x)
{
    u32 bits, sign, exponent, mantissa, half, rest;
    int e;
    memcpy(&bits, &x, sizeof bits);
    sign = bits & 0x80000000u;
    exponent = (bits >> 23) & 0xFFu;
    mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu) return x;
    e = (int)exponent - 127 + 15;
    if (e >= 31) {
        bits = sign | 0x7F800000u;
        memcpy(&x, &bits, sizeof bits);
        return x;
    }
    if (e <= 0) {
        /* A half subnormal has the unit 2^-24. */
        if (e < -10) return sign ? -0.0f : 0.0f;
        u32 moved = mantissa | 0x800000u, shift = (u32)(14 - e);
        half = moved >> shift;
        rest = moved & ((1u << shift) - 1u);
        if (rest > (1u << (shift - 1)) || (rest == (1u << (shift - 1)) && (half & 1u))) half++;
        return (float)((double)half * 0.000000059604644775390625) * (sign ? -1.0f : 1.0f);
    }
    half = ((u32)e << 10) | (mantissa >> 13);
    rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) half++;
    bits = sign | (((half >> 10) + 112u) << 23) | ((half & 0x3FFu) << 13);
    memcpy(&x, &bits, sizeof bits);
    return x;
}

/* The fragment program of the dump:
 *   out = fp16(saturate(fp16(brush - 0.25) * colour.x + dest)).
 * Each float step rounds once: the product and the sum go through double,
 * so the compiler makes no fused multiply-add. */
static u8 deform_op_fp16(u32 dest, u32 brush, u32 a)
{
    float h = deform_f16((float)((double)brush / 255.0 - 0.25));
    float product = (float)((double)h * ((double)a / 255.0));
    float value = (float)((double)product + (double)dest / 255.0);
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    value = deform_f16(value);
    return (u8)(value * 255.0f + 0.5f);
}

typedef struct {
    const char* name;
    u8 (*fn)(u32 dest, u32 brush, u32 a);
} RsxDeformOp;

static const RsxDeformOp s_deform_ops[] = {
    { "fp16", deform_op_fp16 },
    { "f32",  deform_op_f32 },
};
#define RSX_DEFORM_OP_COUNT ((int)(sizeof s_deform_ops / sizeof s_deform_ops[0]))

typedef struct {
    _Atomic int config_state;     /* -1 unread, 1 a thread reads it, 2 on, 0 off */
    int        any;               /* 1 when one switch is on                  */
    u32        offsets[RSX_DEFORM_TARGETS_MAX];
    int        n_offsets;
    int        capture;           /* RSX_DEFORM_CAPTURE=1                     */
    int        chain;             /* RSX_DEFORM_CHAIN=1 with the capture      */
    int        buffer;            /* RSX_DEFORM_BUFFER=1                      */
    int        shadow;            /* RSX_DEFORM_SHADOW=1 with the buffer      */
    int        op;                /* index into s_deform_ops                   */
    u64        log_frames;        /* frames between two [DEFORM] lines         */
    u64        log_frame;         /* frame of the last [DEFORM] line           */
} RsxDeformConfig;

static RsxDeformConfig s_deform = { -1, 0, { 0 }, 0, 0, 0, 0, 0, 0, 300, 0 };

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

/* 1 when the variable exists and is not "0". */
static int deform_switch(const char* name)
{
    const char* value = getenv(name);
    return value && *value && strcmp(value, "0") != 0;
}

static void deform_config_read(void)
{
    const char* targets = getenv("RSX_DEFORM_TARGETS");
    if (!targets || !*targets) return;
    s_deform.n_offsets = deform_read_offsets(targets, s_deform.offsets);
    if (!s_deform.n_offsets) return;
    s_deform.capture = deform_switch("RSX_DEFORM_CAPTURE");
    s_deform.chain = s_deform.capture && deform_switch("RSX_DEFORM_CHAIN");
    s_deform.buffer = deform_switch("RSX_DEFORM_BUFFER");
    s_deform.shadow = s_deform.buffer && deform_switch("RSX_DEFORM_SHADOW");
    const char* op = getenv("RSX_DEFORM_OP");
    if (op && *op) {
        s_deform.op = -1;
        for (int i = 0; i < RSX_DEFORM_OP_COUNT; i++)
            if (strcmp(op, s_deform_ops[i].name) == 0) s_deform.op = i;
        if (s_deform.op < 0) {
            fprintf(stderr, "[DEFORM] RSX_DEFORM_OP=%s is not a pixel operation;"
                            " %s is used\n", op, s_deform_ops[0].name);
            s_deform.op = 0;
        }
    }
    const char* log = getenv("RSX_DEFORM_LOG");
    if (log && *log) s_deform.log_frames = strtoull(log, NULL, 10);
    s_deform.any = s_deform.capture || s_deform.buffer;
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
 * records, into rsx_deform_draws.txt. RSX_DEFORM_CHAIN=1 adds the next
 * RSX_DEFORM_CHAIN_MAX records in FIFO order whose colour target 0 is not a
 * listed offset, into rsx_deform_chain.txt. Those are the passes that follow
 * the paint, such as the mip chain. Both files go next to rsx_phases.csv, so
 * the capture needs the RSX_PHASE_CSV path. Each record holds the state words
 * the backend replays the draw with, the vertex stream, and the decompiled
 * programs. The shader text is printed once per program; later records name
 * the file and the record that holds it.
 */

enum { DEFORM_CAPTURE_DRAWS, DEFORM_CAPTURE_CHAIN, DEFORM_CAPTURE_FILES };
static const char* const s_deform_capture_name[DEFORM_CAPTURE_FILES] = { "draws", "chain" };
static const char* const s_deform_capture_path[DEFORM_CAPTURE_FILES] = {
    "rsx_deform_draws.txt", "rsx_deform_chain.txt"
};

typedef struct {
    FILE* file[DEFORM_CAPTURE_FILES];
    u32   count[DEFORM_CAPTURE_FILES]; /* records written per file            */
    u64   fifo;                        /* records the hook has seen           */
    u32   fp_seen[16];                 /* fp_addr of a printed fragment program */
    u8    fp_file[16];
    u32   fp_record[16];
    int   fp_n;
    u32   vp_seen[16];                 /* start slot of a printed vertex program */
    u8    vp_file[16];
    u32   vp_record[16];
    int   vp_n;
    int   chain_started;               /* the chain file was requested         */
} RsxDeformCapture;

static RsxDeformCapture s_deform_capture;

/* Opens one capture file next to rsx_phases.csv. Returns 0 when there is no
 * RSX_PHASE_CSV path or the file cannot be opened. */
static int deform_capture_open(int which)
{
    const char* phases = getenv("RSX_PHASE_CSV");
    const char* name = s_deform_capture_path[which];
    char path[1024];
    const char* slash = phases ? strrchr(phases, '\\') : NULL;
    const char* other = phases ? strrchr(phases, '/') : NULL;
    if (other && (!slash || other > slash)) slash = other;
    size_t dir = slash ? (size_t)(slash - phases) + 1 : 0;
    if (!phases || !*phases || dir + strlen(name) + 1 > sizeof path) {
        fprintf(stderr, "[DEFORM_CAPTURE] %s: stopped, needs the RSX_PHASE_CSV path\n",
                s_deform_capture_name[which]);
        return 0;
    }
    memcpy(path, phases, dir);
    memcpy(path + dir, name, strlen(name) + 1);
    s_deform_capture.file[which] = fopen(path, "wb");
    if (!s_deform_capture.file[which]) {
        fprintf(stderr, "[DEFORM_CAPTURE] %s: stopped, cannot open %s\n",
                s_deform_capture_name[which], path);
        return 0;
    }
    fprintf(stderr, "[DEFORM_CAPTURE] %s: open %s\n", s_deform_capture_name[which], path);
    return 1;
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

/* Print a program once per identity. Later records name the file and the
 * record that holds the text. */
static void deform_capture_programs(int which, FILE* f, const D3D12DrawRecord* dr,
                                    const rsx_state* st)
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
            fprintf(f, " fp_text=%s:record=%u fp_vm=0x%08X fp_ucode_bytes=%u"
                       " fp_instrs=%d\n fp_ucode=", s_deform_capture_name[which],
                    s_deform_capture.count[which], vm, size, n);
            for (u32 b = 0; b + 4 <= size; b += 4)
                fprintf(f, " %02X%02X%02X%02X", ucode[b], ucode[b + 1], ucode[b + 2],
                        ucode[b + 3]);
            fprintf(f, "\nFP_HLSL_BEGIN\n%s\nFP_HLSL_END\n", n > 0 ? text : "");
            if (s_deform_capture.fp_n < 16) {
                s_deform_capture.fp_seen[s_deform_capture.fp_n] = dr->fp_addr;
                s_deform_capture.fp_file[s_deform_capture.fp_n] = (u8)which;
                s_deform_capture.fp_record[s_deform_capture.fp_n] = s_deform_capture.count[which];
                s_deform_capture.fp_n++;
            }
        } else {
            fprintf(f, " fp_text=%s:record=%u\n", s_deform_capture_name[s_deform_capture.fp_file[seen]],
                    s_deform_capture.fp_record[seen]);
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
            fprintf(f, " vp_text=%s:record=%u vp_start_slot=%u vp_store_bytes=%u"
                       " vp_instrs=%d vp_vs_idx=%d\nVP_HLSL_BEGIN\n%s\nVP_HLSL_END\n",
                    s_deform_capture_name[which], s_deform_capture.count[which],
                    start / 16u, st->vp_ucode_bytes, n, dr->vs_idx, n > 0 ? text : "");
            if (s_deform_capture.vp_n < 16) {
                s_deform_capture.vp_seen[s_deform_capture.vp_n] = start;
                s_deform_capture.vp_file[s_deform_capture.vp_n] = (u8)which;
                s_deform_capture.vp_record[s_deform_capture.vp_n] = s_deform_capture.count[which];
                s_deform_capture.vp_n++;
            }
        } else {
            fprintf(f, " vp_text=%s:record=%u vp_start_slot=%u vp_vs_idx=%d\n",
                    s_deform_capture_name[s_deform_capture.vp_file[seen]],
                    s_deform_capture.vp_record[seen], start / 16u, dr->vs_idx);
        }
    }
}

static void deform_capture_record(int which, const D3D12DrawRecord* dr, const rsx_state* st,
                                  u32 first, u32 count, int indexed)
{
    FILE* f = s_deform_capture.file[which];
    u32 index = ++s_deform_capture.count[which];
    fprintf(f, "[DEFORM_CAPTURE] file=%s record=%u frame=%llu fifo=%llu kind=%s\n",
            s_deform_capture_name[which], index, (unsigned long long)s_d3d.frame_count,
            (unsigned long long)s_deform_capture.fifo, dr->is_clear ? "clear" : "draw");
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
    /* One line with the targets and the bound units. */
    fprintf(f, " rt0..3=0x%08X,0x%08X,0x%08X,0x%08X pitch0=0x%X pitch1=0x%X"
               " clip=%ux%u format=0x%X tex=",
            st->surface_color_offset[0], st->surface_color_offset[1],
            st->surface_color_offset[2], st->surface_color_offset[3],
            st->surface_color_pitch[0], st->surface_color_pitch[1],
            st->surface_clip_w, st->surface_clip_h, st->surface_format);
    for (int u = 0; u < 16; u++)
        fprintf(f, "%s0x%08X", u ? "," : "", st->textures[u].offset);
    fprintf(f, "\n");
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
    deform_capture_programs(which, f, dr, st);
    fflush(f);
}

/* ---------------------------------------------------------------------------
 * B. The CPU buffer of a paint target.
 *
 * RSX_DEFORM_BUFFER=1 with RSX_DEFORM_TARGETS keeps level 0 of each listed
 * surface in guest memory. When the renderer records a clear or a draw into
 * a listed offset, the hook at the end of this file paints it there. The CPU
 * reads then take their bytes from guest memory: no copy of the surface, and
 * no wait for a recorded draw. The GPU keeps the paint draws, because the
 * shaders sample the surface on the GPU.
 *
 * The painter obeys the programs and the state words of the dump of run C1
 * (section A; 64 draws into 00A80500, all with the same two programs):
 *   vertex program   position.xy = attr 0; position.zw = constant 467.xy;
 *                    texcoord0.xy = attr 8 (the brush); colour.x = attr 3.x;
 *                    texcoord0.zw = the target at the pixel itself.
 *                    [INFERENCE] constant 467.y is 1 and 467.z is 0.5: the
 *                    dump has no constants.
 *   stream           stride 20: attr 0 two floats, attr 8 two floats,
 *                    attr 3 four normalized bytes.
 *   primitives       triangles, six per stamp: (p0, p1, p2) and (p0, p2, p3).
 *                    A stamp with equal positions, or positions that are not
 *                    finite, covers no pixel. The dump has many of both.
 *   viewport         the guest rect (0, 0, 4096, 4096). The backend uses the
 *                    guest rect when it fits the target, else the full target.
 *   fragment program out = fp16(saturate(fp16(brush - 0.25) * colour.x + dest)).
 *                    dest is blue of tex[0] (the target) at the pixel, brush
 *                    is blue of tex[1]. The target is B8 and stores blue.
 *   blend, depth test, stencil test, alpha test: off. Colour mask: all.
 *   samplers         the decode of the backend (rsx_d3d12_guest_sampler.h):
 *                    address 1 wrap, 2 mirror, 3 clamp, 4 border, 6 mirror
 *                    once; filter bits 18:16 min, 1 nearest, 2 linear. The
 *                    dump: tex[0] nearest; the path brush (32 x 1) nearest;
 *                    the 2D brush (32 x 32) linear.
 *   clear            byte = blue of the clear colour.
 *   mip pass         8 quads. Level k is tex[0] of the record, sampled at
 *                    the texel centres of the target with a linear filter:
 *                    each texel of level k is the mean of a 2 x 2 block of
 *                    level k - 1. The CPU holds row 0 of level 1 only.
 *
 * RSX_DEFORM_SHADOW=1 with RSX_DEFORM_BUFFER=1 paints into a private copy.
 * The readback path stays, the game reads the GPU bytes, and section C
 * compares the two copies.
 *
 * RSX_DEFORM_OP=<name> selects the pixel operation. "fp16" is the program.
 */

typedef struct RsxDeformSurface {
    _Atomic u64 sequence;      /* 0 = empty; odd while the renderer changes the record or paints */
    u32        offset;         /* RSX offset of level 0                                    */
    u32        guest_address;  /* guest address of texel (0, 0) of level 0                 */
    u32        pitch;          /* guest bytes per row of level 0 and of level 1            */
    u32        width, height;  /* texels per row and rows of level 0                       */
    u32        mip_address;    /* guest address of texel (0, 0) of level 1                 */
    u32        mip_width;      /* texels of row 0 of level 1 that the painter holds        */
    u8*        cpu;            /* the private copy of the shadow mode, else 0              */
    /* Renderer thread only. */
    void (*note)(struct RsxDeformSurface* s, u32 x, u32 y, u32 dest, u32 brush, u32 a);
    u32        dirty_y0, dirty_y1;
    u32        painted, skipped;
    u64        texels;
    u32        programs[8];
    u32        program_count;
    int        noted_state;
    int        noted_bad;
    u32        noted_skip;
} RsxDeformSurface;

static RsxDeformSurface s_deform_surface[RSX_DEFORM_TARGETS_MAX];
static _Atomic u64 s_deform_reads;

/* The buffer is on: the reads take their bytes from guest memory. */
static int deform_buffer_on(void)
{
    return deform_on() && s_deform.buffer && !s_deform.shadow;
}

/* The record of the listed level 0 at an RSX offset, or 0. */
static RsxDeformSurface* deform_surface_at(u32 offset)
{
    for (int i = 0; i < s_deform.n_offsets; i++)
        if (s_deform.offsets[i] == offset) return &s_deform_surface[i];
    return NULL;
}

/* 1 when the CPU buffer of s holds every byte of the range. */
static int deform_surface_holds(const RsxDeformSurface* s, u32 address, u32 bytes)
{
    u64 begin = s->guest_address, end = begin + (u64)s->pitch * s->height;
    u64 mip = s->mip_address, mip_end = mip + s->mip_width;
    return (begin && (u64)address >= begin && (u64)address + bytes <= end)
        || (mip && (u64)address >= mip && (u64)address + bytes <= mip_end);
}

/* Any thread. The published record that holds the guest range, or 0. A
 * record the renderer changes now is skipped: that read takes the GPU path,
 * which is always correct. */
static RsxDeformSurface* deform_surface_of(u32 address, u32 bytes)
{
    for (int i = 0; i < s_deform.n_offsets; i++) {
        RsxDeformSurface* s = &s_deform_surface[i];
        u64 sequence = atomic_load_explicit(&s->sequence, memory_order_acquire);
        if (!sequence || (sequence & 1) || !deform_surface_holds(s, address, bytes)) continue;
        if (atomic_load_explicit(&s->sequence, memory_order_acquire) == sequence) return s;
    }
    return NULL;
}

/* 1 when the CPU buffer holds every byte of the guest range, so a read of it
 * needs no copy of a GPU surface. */
static int rsx_deform_serve_range(u32 address, u32 bytes)
{
    if (!bytes || !deform_buffer_on() || !deform_surface_of(address, bytes)) return 0;
    (void)atomic_fetch_add_explicit(&s_deform_reads, 1, memory_order_relaxed);
    return 1;
}

/* 1 when the OffRT at this RSX offset is a level 0 the CPU buffer holds: the
 * backend does not copy, prefetch or publish it. */
static int rsx_deform_owned(u32 offset)
{
    RsxDeformSurface* s = deform_buffer_on() ? deform_surface_at(offset) : NULL;
    return s && atomic_load_explicit(&s->sequence, memory_order_acquire) != 0;
}

/* 1 when the target keeps its clip size: the backend must not replace the
 * size of a deformation target by the display size. */
static int rsx_deform_true_size(u32 offset)
{
    return deform_on() && s_deform.buffer && deform_listed(offset);
}

/* The sum of the paint sequence numbers. A snapshot path adds it to its
 * check words: a paint between two checks makes the words differ. */
static u64 rsx_deform_sequence(void)
{
    u64 sum = 0;
    if (!deform_buffer_on()) return 0;
    for (int i = 0; i < s_deform.n_offsets; i++)
        sum += atomic_load_explicit(&s_deform_surface[i].sequence, memory_order_acquire);
    return sum;
}

/* Renderer thread. The record of the target of dr. The slot follows the
 * listed offset. The words cpu_surface_capture wrote into the draw record
 * give the guest address, the pitch and the size. Returns 0 for a target
 * that is not a B8 surface inside guest memory. */
static RsxDeformSurface* deform_surface_get(const D3D12DrawRecord* dr)
{
    RsxDeformSurface* s = deform_surface_at(dr->rt_off);
    u32 address = dr->cpu_address[0], pitch = dr->cpu_pitch[0];
    if (!s) return NULL;
    if (address == 0xffffffffu || !pitch || !dr->rt_w || !dr->rt_h || pitch < dr->rt_w
        || rsx_surface_readback_bpp(dr->rt_fmt) != 1
        || (u64)address + (u64)pitch * dr->rt_h > 0x100000000ull) {
        if (!s->noted_bad) {
            s->noted_bad = 1;
            fprintf(stderr, "[DEFORM] target %08X: not painted. The buffer holds a B8"
                            " surface inside guest memory (address %08X pitch %u size %ux%u"
                            " format 0x%X)\n",
                    dr->rt_off, address, pitch, dr->rt_w, dr->rt_h, dr->rt_fmt);
        }
        return NULL;
    }
    if (s->guest_address == address && s->pitch == pitch && s->width == dr->rt_w
        && s->height == dr->rt_h)
        return s;
    u64 sequence = atomic_load_explicit(&s->sequence, memory_order_relaxed);
    atomic_store_explicit(&s->sequence, sequence + 1, memory_order_relaxed);
    if (s->cpu && (s->pitch != pitch || s->height != dr->rt_h)) { free(s->cpu); s->cpu = NULL; }
    s->offset = dr->rt_off;
    s->guest_address = address;
    s->pitch = pitch;
    s->width = dr->rt_w;
    s->height = dr->rt_h;
    s->mip_address = address + pitch * dr->rt_h;
    s->mip_width = dr->rt_w / 2;
    s->dirty_y0 = dr->rt_h;
    s->dirty_y1 = 0;
    s->painted = s->skipped = 0;
    s->texels = 0;
    s->program_count = 0;
    s->noted_state = s->noted_bad = 0;
    s->noted_skip = 0;
    if (s_deform.shadow && !s->cpu) {
        s->cpu = (u8*)malloc((size_t)pitch * dr->rt_h);
        if (s->cpu) memset(s->cpu, 0, (size_t)pitch * dr->rt_h);
    }
    atomic_store_explicit(&s->sequence, sequence + 2, memory_order_release);
    fprintf(stderr, "[DEFORM] target %08X: guest %08X, %ux%u, pitch %u, level 1 row 0 at %08X"
                    " (%u texels), mode %s\n",
            s->offset, s->guest_address, s->width, s->height, s->pitch, s->mip_address,
            s->mip_width, s->cpu ? "shadow" : "on");
    return s;
}

/* ---- the brush ----------------------------------------------------------- */

typedef struct {
    u64 key;             /* the texture words the brush was decoded from             */
    u32 width, height;
    u8* rgba;            /* width * height * 4 bytes: the decoded base level         */
    u32 wrap_s, wrap_t;  /* address mode: 1 wrap, 2 mirror, 3 clamp, 4 border, 6 once */
    int linear;
} DeformBrush;

static DeformBrush s_deform_brush[2];

static u64 deform_brush_key(const rsx_texture_state* t)
{
    const u32 words[] = { t->offset, t->format, t->address, t->filter, t->image_rect, t->control3 };
    u64 key = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        for (int b = 0; b < 4; b++) {
            key ^= (words[i] >> (8 * b)) & 0xFF;
            key *= 1099511628211ull;
        }
    return key;
}

/* One texel coordinate by the address mode of the axis. */
static int deform_wrap(int i, u32 n, u32 mode)
{
    int size = (int)n, period;
    if (size <= 0) return 0;
    switch (mode & 15u) {
    case 1:
        i %= size;
        return i < 0 ? i + size : i;
    case 2:
        period = size * 2;
        i %= period;
        if (i < 0) i += period;
        return i < size ? i : period - 1 - i;
    case 6:
        if (i < -size) return 0;
        if (i < 0) return -1 - i;
        if (i < size) return i;
        return i < size * 2 ? size * 2 - 1 - i : size - 1;
    default:
        return i < 0 ? 0 : i >= size ? size - 1 : i;
    }
}

/* Blue of one brush texel. The border colour of the dump is 0. */
static u32 deform_brush_texel(const DeformBrush* b, int x, int y)
{
    if ((b->wrap_s == 4 && (x < 0 || x >= (int)b->width))
        || (b->wrap_t == 4 && (y < 0 || y >= (int)b->height))) return 0;
    x = deform_wrap(x, b->width, b->wrap_s);
    y = deform_wrap(y, b->height, b->wrap_t);
    return b->rgba[((size_t)y * b->width + (size_t)x) * 4 + 2];
}

/* Blue of the brush at texture coordinates (u, v), 0..255. */
static u32 deform_brush_sample(const DeformBrush* b, float u, float v)
{
    if (!b->linear)
        return deform_brush_texel(b, (int)floorf(u * (float)b->width),
                                  (int)floorf(v * (float)b->height));
    /* Bilinear filtering as the host does it: the texel space in steps of 1 / 256. */
    float fx = floorf((u * (float)b->width - 0.5f) * 256.0f + 0.5f) * (1.0f / 256.0f);
    float fy = floorf((v * (float)b->height - 0.5f) * 256.0f + 0.5f) * (1.0f / 256.0f);
    int x = (int)floorf(fx), y = (int)floorf(fy);
    int wx = (int)((fx - (float)x) * 256.0f + 0.5f), wy = (int)((fy - (float)y) * 256.0f + 0.5f);
    u32 top = deform_brush_texel(b, x, y) * (u32)(256 - wx)
            + deform_brush_texel(b, x + 1, y) * (u32)wx;
    u32 bottom = deform_brush_texel(b, x, y + 1) * (u32)(256 - wx)
               + deform_brush_texel(b, x + 1, y + 1) * (u32)wx;
    return (top * (u32)(256 - wy) + bottom * (u32)wy + 32768u) >> 16;
}

/* Renderer thread. The decoded base level of the brush texture, or 0 when
 * the painter cannot sample it. */
static const DeformBrush* deform_brush_get(const rsx_texture_state* t)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 format = (t->format >> 8) & 0xFFu;
    u32 width = (t->image_rect >> 16) & 0xFFFFu, height = t->image_rect & 0xFFFFu;
    if (!width || !height || (t->format & 4u)) return NULL;      /* none, or a cube */
    u64 key = deform_brush_key(t);
    DeformBrush* b = &s_deform_brush[0];
    if (s_deform_brush[1].key == key) b = &s_deform_brush[1];
    else if (b->key != key) {
        rsx_tex_layout layout;
        rsx_texture_layout_pitched(format, width, height, t->control3 & 0xFFFFFu, &layout);
        if (layout.compressed || !layout.bytes_per_texel) return NULL;
        u32 address = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
        u8* rgba = (u8*)malloc((size_t)width * height * 4);
        if (!rgba) return NULL;
        memset(rgba, 0, (size_t)width * height * 4);
        rsx_texture_decode(rgba, width * 4, vm_base + address, width, height, &layout,
                           rsx_texture_argb_is_rgba());
        free(b->rgba);
        b->rgba = rgba;
        b->key = key;
        b->width = width;
        b->height = height;
        b->wrap_s = t->address & 15u;
        b->wrap_t = (t->address >> 8) & 15u;
        b->linear = ((t->filter >> 16) & 7u) == 2u;   /* the stamps minify the brush */
    }
    return b;
}

/* ---- vertices and triangles ---------------------------------------------- */

typedef struct {
    double x, y;   /* window position in texels; texel (i, j) has its centre at (i + 0.5, j + 0.5) */
    float  u, v, a;
} DeformVertex;

static double deform_edge(const DeformVertex* a, const DeformVertex* b, double x, double y)
{
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

/* Vertex index of the stream of the dump. rect is the viewport of the
 * backend: x, y, width, height. Returns 0 for a value that is not finite. */
static int deform_vertex(const rsx_state* st, u32 index, const float rect[4], DeformVertex* out)
{
    float p[4], t[4], c[4];
    rsx_fetch_attrib(st, 0, index, p);
    rsx_fetch_attrib(st, 8, index, t);
    rsx_fetch_attrib(st, 3, index, c);
    double x = rect[0] + ((double)p[0] + 1.0) * 0.5 * (double)rect[2];
    double y = rect[1] + (1.0 - (double)p[1]) * 0.5 * (double)rect[3];
    if (x - x != 0.0 || y - y != 0.0 || t[0] - t[0] != 0.0f || t[1] - t[1] != 0.0f
        || c[0] - c[0] != 0.0f)
        return 0;
    /* The rasterizer of the host holds the position in steps of 1 / 256 texel. */
    out->x = floor(x * 256.0 + 0.5) * (1.0 / 256.0);
    out->y = floor(y * 256.0 + 0.5) * (1.0 / 256.0);
    out->u = t[0];
    out->v = t[1];
    out->a = c[0];
    return 1;
}

/* Paints one triangle with the top left rule of the host. clip is the
 * scissor: left, top, right, bottom. Right and bottom are outside. */
static void deform_triangle(RsxDeformSurface* s, u8* image, const DeformVertex* v0,
                            const DeformVertex* v1, const DeformVertex* v2,
                            const DeformBrush* brush, const RsxDeformOp* op, const int clip[4])
{
    DeformVertex a = *v0, b = *v1, c = *v2, swap;
    double area = deform_edge(&a, &b, c.x, c.y);
    if (area == 0.0) return;
    if (area < 0.0) { swap = b; b = c; c = swap; area = -area; }
    /* Inside the triangle the three edge functions are positive. A pixel
     * centre on an edge is painted only when the edge is a top edge or a
     * left edge. */
    int on_ab = b.y < a.y || (b.y == a.y && b.x > a.x);
    int on_bc = c.y < b.y || (c.y == b.y && c.x > b.x);
    int on_ca = a.y < c.y || (a.y == c.y && a.x > c.x);
    double minx = a.x < b.x ? a.x : b.x, maxx = a.x > b.x ? a.x : b.x;
    double miny = a.y < b.y ? a.y : b.y, maxy = a.y > b.y ? a.y : b.y;
    if (c.x < minx) minx = c.x;
    if (c.x > maxx) maxx = c.x;
    if (c.y < miny) miny = c.y;
    if (c.y > maxy) maxy = c.y;
    if (minx < clip[0]) minx = clip[0];
    if (miny < clip[1]) miny = clip[1];
    if (maxx > clip[2]) maxx = clip[2];
    if (maxy > clip[3]) maxy = clip[3];
    int x0 = (int)ceil(minx - 0.5), x1 = (int)floor(maxx - 0.5);
    int y0 = (int)ceil(miny - 0.5), y1 = (int)floor(maxy - 0.5);
    for (int py = y0; py <= y1; py++) {
        u8* row = image + (size_t)py * s->pitch;
        double wy = py + 0.5;
        for (int px = x0; px <= x1; px++) {
            double wx = px + 0.5;
            double e0 = deform_edge(&b, &c, wx, wy);
            double e1 = deform_edge(&c, &a, wx, wy);
            double e2 = deform_edge(&a, &b, wx, wy);
            if (e0 < 0.0 || (e0 == 0.0 && !on_bc) || e1 < 0.0 || (e1 == 0.0 && !on_ca)
                || e2 < 0.0 || (e2 == 0.0 && !on_ab))
                continue;
            double w0 = e0 / area, w1 = e1 / area, w2 = e2 / area;
            float u = (float)(w0 * a.u + w1 * b.u + w2 * c.u);
            float v = (float)(w0 * a.v + w1 * b.v + w2 * c.v);
            u32 intensity = (u32)((w0 * a.a + w1 * b.a + w2 * c.a) * 255.0 + 0.5);
            u32 dest = row[px], value = deform_brush_sample(brush, u, v);
            row[px] = op->fn(dest, value, intensity);
            s->texels++;
            if (s->note) s->note(s, (u32)px, (u32)py, dest, value, intensity);
            if ((u32)py < s->dirty_y0) s->dirty_y0 = (u32)py;
            if ((u32)py + 1 > s->dirty_y1) s->dirty_y1 = (u32)py + 1;
        }
    }
}

/* Renderer thread. One draw of the target that the painter does not paint. */
static void deform_skip(RsxDeformSurface* s, int reason, const char* what)
{
    s->skipped++;
    if (s->noted_skip & (1u << reason)) return;
    s->noted_skip |= 1u << reason;
    fprintf(stderr, "[DEFORM] target %08X: a draw is not painted: %s\n", s->offset, what);
}

/* Renderer thread. The state words of the first paint, and a line when the
 * fragment program of a draw is not one of the dump. */
static void deform_state_log(RsxDeformSurface* s, const D3D12DrawRecord* dr, const rsx_state* st)
{
    u32 program = dr->fp_addr, known = 0;
    for (u32 i = 0; i < s->program_count; i++) known |= s->programs[i] == program;
    if (!known && s->program_count < 8) s->programs[s->program_count++] = program;
    if (s->noted_state) return;
    s->noted_state = 1;
    fprintf(stderr, "[DEFORM] target %08X: paint state: fp=%08X vp_start=%u blend_enable=%u"
                    " sfactor=%08X dfactor=%08X equation=%08X depth_test=%u depth_mask=%u"
                    " stencil_test=%u alpha_test=%u color_mask=%08X viewport=%u,%u,%ux%u"
                    " scissor=%u,%u,%ux%u tex0=%08X tex1=%08X filter1=%08X address1=%08X"
                    " attr0=%u,%u attr3=%u,%u attr8=%u,%u op=%s\n",
            s->offset, program, st->transform_program_start, st->blend_enable, st->blend_sfactor,
            st->blend_dfactor, st->blend_equation, st->depth_test_enable, st->depth_mask,
            st->stencil_test_enable, st->alpha_test_enable, st->color_mask, st->viewport_x,
            st->viewport_y, st->viewport_w, st->viewport_h, st->scissor_x, st->scissor_y,
            st->scissor_w, st->scissor_h, st->textures[0].offset, st->textures[1].offset,
            st->textures[1].filter, st->textures[1].address, st->vertex_attribs[0].size,
            st->vertex_attribs[0].type, st->vertex_attribs[3].size, st->vertex_attribs[3].type,
            st->vertex_attribs[8].size, st->vertex_attribs[8].type, s_deform_ops[s_deform.op].name);
    if (st->blend_enable || st->depth_test_enable || st->stencil_test_enable || st->alpha_test_enable)
        fprintf(stderr, "[DEFORM] target %08X: blend, depth, stencil or alpha test is on."
                        " The painter ignores them.\n", s->offset);
}

static void deform_paint_draw(RsxDeformSurface* s, u8* image, const D3D12DrawRecord* dr,
                              const rsx_state* st, u32 first, u32 count, int indexed, u32 primitive)
{
    if (primitive != 5) {
        deform_skip(s, 0, "the primitive type is not triangles");
        return;
    }
    if (!st->vertex_attribs[0].enabled || !st->vertex_attribs[0].stride
        || !st->vertex_attribs[3].enabled || !st->vertex_attribs[3].stride) {
        deform_skip(s, 1, "attr 0 or attr 3 has no array");
        return;
    }
    const DeformBrush* brush = deform_brush_get(&st->textures[1]);
    if (!brush) {
        deform_skip(s, 2, "tex[1] is not a texture the painter can decode");
        return;
    }
    /* The backend merges consecutive triangle draws of one stream into one
     * record. The record then starts at first + count - vertex_count. */
    if (dr->vertex_count > count) first -= dr->vertex_count - count;
    /* The viewport of the backend: the guest rect when it fits the target,
     * else the full target. The scissor is the same rect. */
    float rect[4] = { 0.0f, 0.0f, (float)s->width, (float)s->height };
    float vx = (float)st->viewport_x, vy = (float)st->viewport_y;
    float vw = (float)st->viewport_w, vh = (float)st->viewport_h;
    if (vw >= 1.0f && vh >= 1.0f && vx + vw <= rect[2] + 0.5f && vy + vh <= rect[3] + 0.5f) {
        rect[0] = vx;
        rect[1] = vy;
        rect[2] = vw;
        rect[3] = vh;
    }
    int clip[4] = { (int)rect[0], (int)rect[1], (int)(rect[0] + rect[2]), (int)(rect[1] + rect[3]) };
    deform_state_log(s, dr, st);
    const RsxDeformOp* op = &s_deform_ops[s_deform.op];
    DeformVertex v[3];
    for (u32 i = 0; i + 2 < dr->vertex_count; i += 3) {
        int finite = 1;
        for (u32 k = 0; k < 3 && finite; k++)
            finite = deform_vertex(st, indexed ? deform_guest_index(st, first + i + k) : first + i + k,
                                   rect, &v[k]);
        /* A triangle with a value that is not finite covers no pixel. */
        if (finite) deform_triangle(s, image, &v[0], &v[1], &v[2], brush, op, clip);
    }
    s->painted++;
}

/* Renderer thread. Row 0 of level 1 from rows 0 and 1 of level 0: the mip
 * pass of the dump. Round half up. */
static void deform_mip_row(RsxDeformSurface* s, const u8* level0)
{
    u8* row = vm_base + s->mip_address;
    for (u32 x = 0; x < s->mip_width; x++) {
        const u8* a = level0 + (size_t)x * 2, *b = a + s->pitch;
        row[x] = (u8)((a[0] + a[1] + b[0] + b[1] + 2) / 4);
    }
}

/* One [DEFORM] line per listed surface that painted, each RSX_DEFORM_LOG
 * frames that paint. */
static void deform_frame_log(void)
{
    u64 frame = s_d3d.frame_count;
    if (frame - s_deform.log_frame < s_deform.log_frames) return;
    s_deform.log_frame = frame;
    for (int i = 0; i < s_deform.n_offsets; i++) {
        RsxDeformSurface* s = &s_deform_surface[i];
        if (!s->painted && !s->skipped) continue;
        fprintf(stderr, "[DEFORM] frame=%llu target=%08X mode=%s op=%s painted=%u skipped=%u"
                        " texels=%llu dirty_rows=%u-%u reads=%llu\n",
                (unsigned long long)frame, s->offset, s_deform.shadow ? "shadow" : "on",
                s_deform_ops[s_deform.op].name, s->painted, s->skipped,
                (unsigned long long)s->texels, s->dirty_y0, s->dirty_y1,
                (unsigned long long)atomic_load_explicit(&s_deform_reads, memory_order_relaxed));
        s->painted = s->skipped = 0;
        s->texels = 0;
        s->dirty_y0 = s->height;
        s->dirty_y1 = 0;
    }
}

/* Renderer thread. Paints the clear or the draw of one record into the CPU
 * buffer of its target. The sequence number is odd while the bytes change. */
static void deform_paint(const D3D12DrawRecord* dr, u32 first, u32 count, int indexed, u32 primitive)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || !deform_listed(dr->rt_off)) return;
    RsxDeformSurface* s = deform_surface_get(dr);
    if (!s || (s_deform.shadow && !s->cpu)) return;
    u64 sequence = atomic_load_explicit(&s->sequence, memory_order_relaxed);
    atomic_store_explicit(&s->sequence, sequence + 1, memory_order_relaxed);
    u8* image = s->cpu ? s->cpu : vm_base + s->guest_address;
    if (dr->is_clear) {
        u32 value = dr->cc[2] <= 0.0f ? 0 : dr->cc[2] >= 1.0f ? 255 : (u32)(dr->cc[2] * 255.0f + 0.5f);
        for (u32 y = 0; y < s->height; y++) memset(image + (size_t)y * s->pitch, value, s->width);
        s->dirty_y0 = 0;
        s->dirty_y1 = s->height;
    } else {
        deform_paint_draw(s, image, dr, st, first, count, indexed, primitive);
    }
    if (!s->cpu && s->dirty_y0 < 2) deform_mip_row(s, image);
    atomic_store_explicit(&s->sequence, sequence + 2, memory_order_release);
    deform_frame_log();
}

/* ---------------------------------------------------------------------------
 * D. Reads of the game.
 *
 * rsx_deform_sample() is the native GetDepth of the game
 * (src/motorstorm/deformation_query.cpp). It returns the 4 bytes GetDepth
 * reads, in the order of the game: (x, y), (x + 1, y), (x, y + 1),
 * (x + 1, y + 1). x and y are the indices after the clamp of the game.
 * x + 1 and y + 1 are not clamped, as in the game: a byte outside level 0
 * is not returned. The count is 0 when no record holds the surface: the
 * caller then reads the guest memory.
 */
int rsx_deform_sample(u32 address, int x, int y, unsigned char out[4])
{
    if (!deform_buffer_on()) return 0;
    RsxDeformSurface* s = deform_surface_of(address, 0);
    if (!s || s->guest_address != address || x < 0 || y < 0) return 0;
    for (int tries = 0; tries < 4096; tries++) {
        u64 sequence = atomic_load_explicit(&s->sequence, memory_order_acquire);
        if (sequence & 1) continue;
        const u8* image = vm_base + s->guest_address;
        int count = 0;
        for (int k = 0; k < 4; k++) {
            int px = x + (k & 1), py = y + (k >> 1);
            if (px >= (int)s->width || py >= (int)s->height) continue;
            out[count++] = image[(size_t)py * s->pitch + (size_t)px];
        }
        if (atomic_load_explicit(&s->sequence, memory_order_acquire) == sequence) return count;
    }
    return 0;
}

/* Backend init, after cpu_surface_init: the hooks of the read paths. */
static void rsx_deform_init(void)
{
    s_cpu_surface_deform.serve = rsx_deform_serve_range;
    s_cpu_surface_deform.owned = rsx_deform_owned;
    s_cpu_surface_deform.sequence = rsx_deform_sequence;
}

/* Hook of the backend: after the clear record fields are set in d3d12_clear
 * or record_surface_clear, and next to cpu_surface_capture(dr) at the three
 * draw record sites. first and count are the arguments of the draw, indexed
 * is 1 when the vertices come through the guest index array, and primitive
 * is the RSX primitive type of the draw (0 for a clear). */
static void rsx_deform_capture(const D3D12DrawRecord* dr, u32 first, u32 count, int indexed,
                               u32 primitive)
{
    if (!deform_on()) return;
    if (s_deform.buffer) deform_paint(dr, first, count, indexed, primitive);
    if (!s_deform.capture) return;
    if (!s_deform_capture.file[DEFORM_CAPTURE_DRAWS]) {
        if (!deform_capture_open(DEFORM_CAPTURE_DRAWS)) {
            s_deform.capture = 0;
            return;
        }
        fprintf(stderr, "[DEFORM_CAPTURE] on: %d target%s, first %u records\n",
                s_deform.n_offsets, s_deform.n_offsets == 1 ? "" : "s",
                (u32)RSX_DEFORM_CAPTURE_MAX);
    }
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return;
    s_deform_capture.fifo++;
    if (deform_listed(dr->rt_off) || deform_listed(dr->rt_mrt[0])
        || deform_listed(dr->rt_mrt[1]) || deform_listed(dr->rt_mrt[2])) {
        if (s_deform_capture.count[DEFORM_CAPTURE_DRAWS] >= RSX_DEFORM_CAPTURE_MAX) return;
        deform_capture_record(DEFORM_CAPTURE_DRAWS, dr, st, first, count, indexed);
        if (s_deform_capture.count[DEFORM_CAPTURE_DRAWS] == RSX_DEFORM_CAPTURE_MAX)
            fprintf(stderr, "[DEFORM_CAPTURE] full: %u records written\n",
                    (u32)RSX_DEFORM_CAPTURE_MAX);
        /* The chain starts with the record that follows the first one of the dump. */
        if (s_deform.chain && !s_deform_capture.chain_started) {
            s_deform_capture.chain_started = 1;
            deform_capture_open(DEFORM_CAPTURE_CHAIN);
        }
        return;
    }
    if (!s_deform_capture.file[DEFORM_CAPTURE_CHAIN]
        || s_deform_capture.count[DEFORM_CAPTURE_CHAIN] >= RSX_DEFORM_CHAIN_MAX)
        return;
    deform_capture_record(DEFORM_CAPTURE_CHAIN, dr, st, first, count, indexed);
    if (s_deform_capture.count[DEFORM_CAPTURE_CHAIN] == RSX_DEFORM_CHAIN_MAX)
        fprintf(stderr, "[DEFORM_CAPTURE] chain: %u records written\n",
                (u32)RSX_DEFORM_CHAIN_MAX);
}
