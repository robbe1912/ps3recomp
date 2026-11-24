/* Fixture of tests/check_rsx_deform_surface.py.
 *
 * It compiles production text of libs/video/rsx_deform_surface.inc next to a
 * model of the backend names: the pixel operations, the painter, the mip row,
 * the sampler and the shadow comparison. The texture decode is the production
 * rsx_texture_layout.c. The state is the captured paint state of run C1
 * (#346): a 4096 x 4096 B8 target at RSX offset 00A80500, the viewport
 * 0,0,4096,4096, a stream of stride 20 (attr 0 two floats, attr 8 two floats,
 * attr 3 four normalized bytes) and B8 brushes.
 *
 * Guest memory is a model array. Level 0 starts at guest address BASE; row 0
 * of level 1 follows its last row. Bytes after that row catch a write past
 * it. */
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_deform_surface.h"
#include "rsx_texture_layout.h"
#include "rsx_surface_readback.h"

/* ---- the model of the backend names ------------------------------------- */

uint8_t* vm_base;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    (void)local;
    return offset;
}

typedef struct {
    int enabled;
    u32 size, type, stride, offset, format, frequency;
} rsx_vertex_attrib;

typedef struct {
    u32 offset, format, address, filter, image_rect, control3;
} rsx_texture_state;

/* The state words the painter reads. */
typedef struct {
    rsx_vertex_attrib vertex_attribs[16];
    rsx_texture_state textures[16];
    u32 viewport_x, viewport_y, viewport_w, viewport_h;
    u32 scissor_x, scissor_y, scissor_w, scissor_h;
    u32 index_array_dma, index_array_offset, transform_program_start;
    u32 blend_enable, blend_sfactor, blend_dfactor, blend_equation;
    u32 depth_test_enable, depth_mask, stencil_test_enable, alpha_test_enable;
    u32 color_mask;
} rsx_state;

/* The record words the painter reads. */
typedef struct {
    u32 rt_off, rt_w, rt_h, rt_fmt;
    u32 cpu_address[4], cpu_pitch[4];
    int is_clear;
    float cc[4];
    u32 vertex_count, fp_addr;
} D3D12DrawRecord;

#define CPU_SURFACE_CAPACITY 4

/* The surface words the comparison reads, and the model of the GPU image. */
typedef struct {
    void* res;
    u32 off, cpu_address, cpu_pitch;
    u64 gpu_generation, cpu_generation;
    u8* source;
    u32 rows;
} OffRT;

static struct {
    rsx_state* current_rsx_state;
    u64 frame_count;
    OffRT off_rt[CPU_SURFACE_CAPACITY];
} s_d3d;

/* The vertex fetch of the captured stream: float (type 2) and unsigned bytes
 * (type 4), as rsx_vertex_fetch.c reads them. */
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{
    const rsx_vertex_attrib* a = &st->vertex_attribs[idx];
    const u8* p = vm_base + a->offset + (size_t)vi * a->stride;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 k = 0; k < a->size; k++) {
        if (a->type == 2) memcpy(&out[k], p + k * 4, 4);
        else out[k] = p[k] / 255.0f;
    }
}

/* The readback copy of the backend: after the GPU fence, the bytes of the GPU
 * surface are in guest memory. */
static void cpu_surface_copy(OffRT* r)
{
    if (r->cpu_generation == r->gpu_generation) return;
    memcpy(vm_base + r->cpu_address, r->source, (size_t)r->cpu_pitch * r->rows);
    r->cpu_generation = r->gpu_generation;
}

/* The log of the production text goes to log_text. */
static char log_text[1 << 15];
static int test_vfprintf(FILE* file, const char* format, ...);
#define fprintf test_vfprintf
/* PRODUCTION_PIXEL */
/* PRODUCTION_BUFFER */
#undef fprintf
static int test_vfprintf(FILE* file, const char* format, ...)
{
    va_list list;
    size_t used = strlen(log_text);
    (void)file;
    va_start(list, format);
    vsnprintf(log_text + used, sizeof log_text - used, format, list);
    va_end(list);
    return 0;
}

/* ---- the test ------------------------------------------------------------ */

static void test_fail(int line, const char* format, ...)
{
    va_list list;
    va_start(list, format);
    fflush(stdout);
    fprintf(stderr, "FAIL line %d: ", line);
    vfprintf(stderr, format, list);
    fputc('\n', stderr);
    exit(1);
}
#define FAIL(...) test_fail(__LINE__, __VA_ARGS__)

enum {
    BASE = 0x1000, PITCH = 4096, ROWS = 4096, MIP = BASE + PITCH * ROWS, GUARD = 0x8000,
    VB = 0x100, IB = 0x80, BRUSH = 0x200000, MIP_TEXELS = PITCH / 2
};

static u8 *gpu0, *gpu1, *before, *expected;
static const u8* current;
static rsx_state st;
static D3D12DrawRecord dr;
static u8 brush_bytes[32 * 32];
static u32 random_state = 12345u;

static u32 random_u32(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state >> 8;
}

static void fill(u8* image, u32 seed)
{
    random_state = seed;
    for (size_t i = 0; i < (size_t)PITCH * ROWS; i++) image[i] = (u8)random_u32();
}

static void set_switch(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Reads the switches again and drops the records and the brush cache. The
 * private copies of the last binding stay allocated, as in a long run. */
static void configure(const char* targets, const char* buffer, const char* shadow)
{
    set_switch("RSX_DEFORM_TARGETS", targets);
    set_switch("RSX_DEFORM_BUFFER", buffer);
    set_switch("RSX_DEFORM_SHADOW", shadow);
    set_switch("RSX_DEFORM_LOG", "3");
    s_deform.config_state = -1;
    memset(s_deform_surface, 0, sizeof s_deform_surface);
    memset(s_deform_brush, 0, sizeof s_deform_brush);
    memset(vm_base, 0, MIP + GUARD);
    log_text[0] = 0;
}

static void state_init(void)
{
    memset(&st, 0, sizeof st);
    st.vertex_attribs[0] = (rsx_vertex_attrib){ 1, 2, 2, 20, VB, 0x1422, 0 };
    st.vertex_attribs[8] = (rsx_vertex_attrib){ 1, 2, 2, 20, VB + 8, 0x1422, 0 };
    st.vertex_attribs[3] = (rsx_vertex_attrib){ 1, 4, 4, 20, VB + 16, 0x1444, 0 };
    st.viewport_w = st.viewport_h = st.scissor_w = st.scissor_h = ROWS;
    memset(&dr, 0, sizeof dr);
    dr.rt_off = 0x00A80500;
    dr.rt_w = dr.rt_h = ROWS;
    dr.rt_fmt = 0x149;
    dr.cpu_address[0] = BASE;
    dr.cpu_pitch[0] = PITCH;
    dr.vertex_count = 6;
    dr.fp_addr = 1;
    memset(s_d3d.off_rt, 0, sizeof s_d3d.off_rt);
    s_d3d.current_rsx_state = &st;
    s_d3d.frame_count = 100;
    s_deform.log_frame = 100;      /* no periodic [DEFORM] line in a scenario */
}

/* Puts a B8 brush of width x height at BRUSH and binds it to tex[1], with
 * the words of the capture: address modes wrap. */
static void brush(const u8* texels, u32 width, u32 height, int linear)
{
    rsx_tex_layout layout;
    rsx_texture_layout_pitched(0x81u, width, height, 0, &layout);
    u32 l2w = rsx_log2_ceil(width), l2h = rsx_log2_ceil(height);
    for (u32 y = 0; y < height; y++)
        for (u32 x = 0; x < width; x++) {
            u32 at = layout.swizzled ? rsx_swizzle_offset(x, y, l2w, l2h) : y * width + x;
            vm_base[BRUSH + at] = texels[y * width + x];
        }
    st.textures[1] = (rsx_texture_state){ BRUSH, 0x00018129u, 0x60030101u,
                                          linear ? 0x02023F34u : 0x01013F34u,
                                          width << 16 | height, 0 };
    /* The same words can hold another image: drop the cache. */
    memset(s_deform_brush, 0, sizeof s_deform_brush);
}

static const double path[4][2] = { { 0, 0.5 }, { 1, 0.5 }, { 1, 0.5 }, { 0, 0.5 } };
static const double corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

static void path_brush(void)
{
    for (u32 i = 0; i < 32; i++) brush_bytes[i] = (u8)(7 * i + 3);
    brush(brush_bytes, 32, 1, 0);
}

static void vertex(u32 index, double x, double y, double u, double v, unsigned intensity)
{
    u8* p = vm_base + VB + index * 20;
    float f[4] = { (float)(2.0 * x / ROWS - 1.0), (float)(1.0 - 2.0 * y / ROWS), (float)u, (float)v };
    memcpy(p, f, 8);
    memcpy(p + 8, f + 2, 8);
    p[16] = (u8)intensity;
    p[17] = p[18] = p[19] = 0;
}

/* One stamp of the capture at vertex `first`: two triangles, (p0, p1, p2) and
 * (p0, p2, p3), in window texels. reversed gives the other winding. */
static void quad(u32 first, double x0, double x1, double y0, double y1, const double uv[4][2],
                 unsigned intensity, int reversed)
{
    const double x[4] = { x0, x1, x1, x0 }, y[4] = { y0, y0, y1, y1 };
    static const u32 order[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 0, 2, 1, 0, 3, 2 } };
    for (int i = 0; i < 6; i++) {
        u32 k = order[reversed][i];
        vertex(first + (u32)i, x[k], y[k], uv[k][0], uv[k][1], intensity);
    }
}

/* ---- the reference ------------------------------------------------------- */

/* Half rounding on the half grid: round to nearest, ties to even. */
static double ref_half(double x)
{
    if (x == 0.0) return 0.0;
    double a = fabs(x), e = floor(log2(a));
    double step = e >= -14.0 ? ldexp(1.0, (int)e - 10) : ldexp(1.0, -24);
    double n = a / step, low = floor(n), rest = n - low;
    double rounded = rest > 0.5 || (rest == 0.5 && fmod(low, 2.0) != 0.0) ? low + 1 : low;
    return copysign(rounded * step, x);
}

/* out = fp16(saturate(fp16(brush - 0.25) * colour.x + dest)) */
static u8 ref_op(u32 dest, u32 brush, float colour)
{
    u32 a = (u32)(colour * 255.0f + 0.5f);
    double h = ref_half((double)brush / 255.0 - 0.25);
    double product = (float)(h * ((double)a / 255.0));
    double value = (float)(product + (double)dest / 255.0);
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    return (u8)(ref_half(value) * 255.0 + 0.5);
}

/* The nearest path brush (32 x 1) at column px of a stamp from xa to xb. */
static u32 path_texel(u32 px, double xa, double xb)
{
    double u = ((double)px + 0.5 - xa) / (xb - xa);
    return brush_bytes[(u32)floor(u * 32.0)];
}

static void expect_path(double xa, double xb, u32 ya, u32 yb, unsigned intensity)
{
    float colour = (float)intensity / 255.0f;
    for (u32 y = ya; y < yb; y++)
        for (u32 x = (u32)xa; x < (u32)xb; x++) {
            size_t at = (size_t)y * PITCH + x;
            expected[at] = ref_op(before[at], path_texel(x, xa, xb), colour);
        }
}

/* before takes the image; expected starts as its copy. */
static void snap(void)
{
    memcpy(before, current, (size_t)PITCH * ROWS);
    memcpy(expected, before, (size_t)PITCH * ROWS);
}

static void compare(const char* what)
{
    for (size_t i = 0; i < (size_t)PITCH * ROWS; i++)
        if (current[i] != expected[i])
            FAIL("%s: texel %u of the image is %u, expected %u", what, (u32)i, current[i],
                 expected[i]);
}

/* ---- the checks ---------------------------------------------------------- */

static void check_off(void)
{
    unsigned char out[4] = { 9, 9, 9, 9 };
    const uint32_t offsets[4] = { 0, 1, PITCH, PITCH + 1 };
    state_init();
    configure("", "", "");
    if (rsx_deform_mode() != 0 || rsx_deform_sample(BASE, offsets, out) != 0
        || rsx_deform_serve_range(BASE, 4) || rsx_deform_owned(0x00A80500)
        || rsx_deform_true_size(0x00A80500) || rsx_deform_sequence())
        FAIL("without RSX_DEFORM_TARGETS a path is on");
    /* A listed target with RSX_DEFORM_BUFFER=0 is the capture mode. */
    configure("00A80500", "0", "0");
    if (rsx_deform_mode() != 0 || rsx_deform_sample(BASE, offsets, out) != 0
        || rsx_deform_serve_range(BASE, 4) || rsx_deform_owned(0x00A80500)
        || rsx_deform_true_size(0x00A80500))
        FAIL("RSX_DEFORM_BUFFER=0 keeps the GPU path");
    if (out[0] != 9) FAIL("a refused sample wrote a byte");
    /* The shadow mode needs the buffer. */
    configure("00A80500", "0", "1");
    if (rsx_deform_mode() != 0 || rsx_deform_true_size(0x00A80500))
        FAIL("the shadow mode is on without the buffer");
    puts("PASS off: no switch, no served read, no target of the true size");
}

static void check_formula(void)
{
    static const u32 levels[] = { 0, 1, 7, 64, 128, 174, 190, 254, 255 };
    u64 checked = 0;
    for (u32 dest = 0; dest < 256; dest++)
        for (u32 b = 0; b < 256; b++)
            for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
                u32 a = levels[i];
                float colour = (float)a / 255.0f;
                if (deform_op_fp16(dest, b, colour) != ref_op(dest, b, colour))
                    FAIL("fp16 operation: dest %u brush %u intensity %u gives %u, expected %u",
                         dest, b, a, deform_op_fp16(dest, b, colour), ref_op(dest, b, colour));
                double value = ((double)b / 255.0 - 0.25) * ((double)a / 255.0) + (double)dest / 255.0;
                if (value < 0.0) value = 0.0;
                if (value > 1.0) value = 1.0;
                if (deform_op_f32(dest, b, colour) != (u8)(value * 255.0 + 0.5))
                    FAIL("f32 operation: dest %u brush %u intensity %u", dest, b, a);
                checked++;
            }
    for (int i = 0; i < 200000; i++) {
        u32 dest = random_u32() & 255u, b = random_u32() & 255u, a = random_u32() & 255u;
        float colour = (float)a / 255.0f;
        if (deform_op_fp16(dest, b, colour) != ref_op(dest, b, colour))
            FAIL("fp16 operation: dest %u brush %u intensity %u", dest, b, a);
        checked++;
    }
    printf("PASS formula: %llu inputs of fp16(saturate(fp16(brush - 0.25) * colour + dest))\n",
           (unsigned long long)checked);
}

static void check_on(void)
{
    unsigned char out[4];
    configure("00A80500", "1", "0");
    state_init();
    if (rsx_deform_mode() != 1) FAIL("RSX_DEFORM_BUFFER=1 is the mode on");
    current = vm_base + BASE;
    fill(vm_base + BASE, 1);
    path_brush();
    /* Two path stamps in one draw, the second in the other winding of its two
     * triangles. Their edges run through the texel centres: the top edge and
     * the left edge are painted, the bottom edge and the right edge are not. */
    quad(0, 10.5, 20.5, 30.5, 40.5, path, 174, 0);
    quad(6, 40.5, 50.5, 30.5, 40.5, path, 174, 1);
    snap();
    expect_path(10.5, 20.5, 30, 40, 174);
    expect_path(40.5, 50.5, 30, 40, 174);
    dr.vertex_count = 12;
    deform_paint(&dr, 0, 12, 0, 5);
    compare("two path stamps");
    RsxDeformSurface* s = &s_deform_surface[0];
    if (s->texels != 200 || s->dirty_y0 != 30 || s->dirty_y1 != 40)
        FAIL("path stamps: %llu texels, rows %u to %u", (unsigned long long)s->texels,
             s->dirty_y0, s->dirty_y1);

    /* The capture draws through a 16 bit index array. */
    dr.vertex_count = 6;
    quad(12, 60, 70, 30, 40, path, 190, 0);
    st.index_array_dma = 0x10;
    st.index_array_offset = IB;
    for (u32 i = 0; i < 6; i++) {
        vm_base[IB + 2 * i] = 0;
        vm_base[IB + 2 * i + 1] = (u8)(12 + i);
    }
    snap();
    expect_path(60, 70, 30, 40, 190);
    deform_paint(&dr, 0, 6, 1, 5);
    compare("indexed stamp");

    /* The 2D brush, 32 x 32, bilinear. A 2 x 2 texel stamp: the centre of
     * each texel is the corner of four brush texels, so the brush byte is the
     * mean of the four, rounded half up. */
    u8 shape[32 * 32];
    random_state = 7;
    for (u32 i = 0; i < 32 * 32; i++) shape[i] = (u8)random_u32();
    shape[7 * 32 + 7] = 10, shape[7 * 32 + 8] = 10, shape[8 * 32 + 7] = 11, shape[8 * 32 + 8] = 11;
    shape[7 * 32 + 23] = 5, shape[7 * 32 + 24] = 5, shape[8 * 32 + 23] = 6, shape[8 * 32 + 24] = 6;
    shape[23 * 32 + 7] = 1, shape[23 * 32 + 8] = 2, shape[24 * 32 + 7] = 2, shape[24 * 32 + 8] = 2;
    shape[23 * 32 + 23] = 200, shape[23 * 32 + 24] = 200, shape[24 * 32 + 23] = 201, shape[24 * 32 + 24] = 201;
    brush(shape, 32, 32, 1);
    quad(18, 60, 62, 50, 52, corners, 190, 0);
    snap();
    static const u32 means[2][2] = { { 11, 6 }, { 2, 201 } };
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 2; x++) {
            size_t at = (size_t)(50 + y) * PITCH + 60 + x;
            expected[at] = ref_op(before[at], means[y][x], 190.0f / 255.0f);
        }
    deform_paint(&dr, 18, 6, 0, 5);
    compare("bilinear 2D stamp");

    /* A stamp at clip (-1, -1) and a stamp where every position is not finite
     * cover no pixel. */
    path_brush();
    u64 texels = s->texels;
    u32 y0 = s->dirty_y0, y1 = s->dirty_y1;
    quad(24, 70, 80, 30, 40, path, 174, 0);
    for (u32 i = 24; i < 30; i++) vertex(i, -1.0, -1.0, 0.5, 0.5, 174);
    quad(30, 70, 80, 30, 40, path, 174, 0);
    float not_finite = (float)NAN;
    for (u32 i = 30; i < 36; i++) memcpy(vm_base + VB + i * 20, &not_finite, 4);
    snap();
    deform_paint(&dr, 24, 6, 0, 5);
    deform_paint(&dr, 30, 6, 0, 5);
    compare("stamps that cover no pixel");
    if (s->texels != texels || s->dirty_y0 != y0 || s->dirty_y1 != y1)
        FAIL("a stamp that covers no pixel painted");

    /* A stamp with one position that is not finite paints its other triangle
     * only, as the GPU does. No texel centre lies on the diagonal. */
    quad(36, 80, 90, 30, 41, path, 174, 0);
    memcpy(vm_base + VB + 37 * 20, &not_finite, 4);
    snap();
    for (u32 y = 30; y < 41; y++)
        for (u32 x = 80; x < 90; x++)
            if ((double)y + 0.5 > 30.0 + 1.1 * ((double)x - 79.5)) {
                size_t at = (size_t)y * PITCH + x;
                expected[at] = ref_op(before[at], path_texel(x, 80, 90), 174.0f / 255.0f);
            }
    deform_paint(&dr, 36, 6, 0, 5);
    compare("a stamp with one position that is not finite");

    /* A stamp that reaches below the last row paints rows 4090 to 4095. */
    quad(36, 10, 20, 4090, 4100, path, 100, 0);
    snap();
    expect_path(10, 20, 4090, ROWS, 100);
    static u8 mip_save[MIP_TEXELS];
    memcpy(mip_save, vm_base + MIP, MIP_TEXELS);
    deform_paint(&dr, 36, 6, 0, 5);
    compare("row bounds");
    if (memcmp(mip_save, vm_base + MIP, MIP_TEXELS))
        FAIL("the painter wrote into row 0 of level 1");
    for (u32 i = 0; i < 3 * PITCH; i++)
        if (vm_base[MIP + PITCH + i]) FAIL("the painter wrote past row 0 of level 1");
    if (s->dirty_y1 != ROWS) FAIL("the dirty rows end at %u", s->dirty_y1);

    /* Row 0 of level 1 is the mean of each 2 x 2 block of rows 0 and 1. A
     * stamp in rows 0 or 1 computes it. */
    memset(vm_base + BASE, 0, 2 * PITCH);
    vm_base[BASE] = 1, vm_base[BASE + 1] = 2, vm_base[BASE + PITCH] = 3, vm_base[BASE + PITCH + 1] = 4;
    vm_base[BASE + 2] = vm_base[BASE + 3] = 5;
    vm_base[BASE + PITCH + 2] = vm_base[BASE + PITCH + 3] = 5;
    vm_base[BASE + 4] = 10, vm_base[BASE + PITCH + 5] = 1;
    vm_base[BASE + 6] = 3, vm_base[BASE + 8] = 2, vm_base[BASE + 10] = 1;
    quad(42, 200, 202, 0, 2, path, 0, 0);
    snap();
    expect_path(200, 202, 0, 2, 0);
    deform_paint(&dr, 42, 6, 0, 5);
    compare("stamp in rows 0 and 1");
    static const u8 block_means[6] = { 3, 5, 3, 1, 1, 0 };
    for (u32 x = 0; x < 6; x++)
        if (vm_base[MIP + x] != block_means[x])
            FAIL("row 0 of level 1, texel %u: %u, expected %u", x, vm_base[MIP + x], block_means[x]);

    /* The read of the game at its bottom row: two bytes of level 0, then the
     * two bytes of row 0 of level 1 that follow them in guest memory. */
    vm_base[BASE + 4095 * PITCH] = 0x11;
    vm_base[BASE + 4095 * PITCH + 1] = 0x22;
    const uint32_t fixed[4] = { 4095 * PITCH, 4095 * PITCH + 1, ROWS * PITCH, ROWS * PITCH + 1 };
    int count = rsx_deform_sample(BASE, fixed, out);
    if (count != 4 || out[0] != 0x11 || out[1] != 0x22 || out[2] != 3 || out[3] != 5)
        FAIL("read at the bottom row: %d bytes %u %u %u %u", count, out[0], out[1], out[2], out[3]);
    /* Right of the last column is the first byte of row 0 of level 1. The two
     * bytes below the bottom row are past the 2048 texels of that row. */
    const uint32_t edge[4] = { 4095 * PITCH + 4095, 4095 * PITCH + 4096, ROWS * PITCH + 4095,
                               ROWS * PITCH + 4096 };
    count = rsx_deform_sample(BASE, edge, out);
    if (count != 2 || out[0] != current[4095 * PITCH + 4095] || out[1] != 3)
        FAIL("read at the right column: %d bytes %u %u", count, out[0], out[1]);
    /* A read during a paint returns nothing. */
    u64 sequence = atomic_load_explicit(&s->sequence, memory_order_relaxed);
    atomic_store_explicit(&s->sequence, sequence + 1, memory_order_relaxed);
    count = rsx_deform_sample(BASE, fixed, out);
    int served = rsx_deform_serve_range(BASE, 4);
    atomic_store_explicit(&s->sequence, sequence, memory_order_relaxed);
    if (count != 0 || served) FAIL("a read during a paint returned %d bytes, served %d", count, served);

    static const struct { u32 address, bytes; int served; } ranges[] = {
        { BASE, 4, 1 }, { MIP - 4, 4, 1 }, { MIP, 2, 1 }, { MIP + MIP_TEXELS - 1, 1, 1 },
        { BASE - 1, 2, 0 }, { MIP - 2, 4, 0 }, { MIP + MIP_TEXELS, 1, 0 },
        { MIP + MIP_TEXELS - 1, 2, 0 },
    };
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        if (rsx_deform_serve_range(ranges[i].address, ranges[i].bytes) != ranges[i].served)
            FAIL("range %08X of %u bytes: served %d, expected %d", ranges[i].address, ranges[i].bytes,
                 rsx_deform_serve_range(ranges[i].address, ranges[i].bytes), ranges[i].served);
    if (!rsx_deform_owned(0x00A80500) || !rsx_deform_true_size(0x00A80500) || !rsx_deform_sequence())
        FAIL("a listed target is not owned in the mode on");
    if (rsx_deform_owned(0x01A80500) || rsx_deform_true_size(0x01A80500))
        FAIL("a target that is not listed is owned");
    size_t logged = strlen(log_text);
    rsx_deform_wheel_log(BASE, 30 * PITCH + 10, "point", out, out);
    if (strlen(log_text) != logged) FAIL("a wheel line outside the shadow mode");
    puts("PASS on: path stamps, indexed stamp, bilinear stamp, no-op stamps, row bounds, mip row, reads, ranges");
}

/* 1 when the log holds the compare line of a frame, from its frame number and
 * the start of its fields. */
static int logged_line(const char* frame, const char* fields)
{
    char text[200];
    snprintf(text, sizeof text, "[DEFORM_SHADOW] frame=%s target=00A80500 %s", frame, fields);
    return strstr(log_text, text) != NULL;
}

static void check_shadow(void)
{
    unsigned char out[4], cpu[4] = { 1, 2, 3, 4 }, gpu[4] = { 5, 6, 7, 8 };
    configure("00A80500", "1", "1");
    set_switch("RSX_DEFORM_LOG", "5");
    state_init();
    if (rsx_deform_mode() != 2) FAIL("RSX_DEFORM_SHADOW=1 with the buffer is the shadow mode");
    path_brush();
    /* A stamp that covers no pixel makes the record. */
    for (u32 i = 12; i < 18; i++) vertex(i, -1.0, -1.0, 0.5, 0.5, 174);
    vm_base[BASE + 500 * PITCH + 12] = 77;
    vm_base[MIP + 700] = 66;      /* the private copies start as guest memory */
    deform_paint(&dr, 12, 6, 0, 5);
    RsxDeformSurface* s = &s_deform_surface[0];
    if (!s->ops[0] || !s->ops[RSX_DEFORM_OP_COUNT - 1] || !s->mip || !s->cols)
        FAIL("no private copies");
    if (s->ops[0][500 * PITCH + 12] != 77 || s->ops[RSX_DEFORM_OP_COUNT - 1][500 * PITCH + 12] != 77
        || s->mip[700] != 66)
        FAIL("the private copies do not start as guest memory");
    /* The GPU surface appears. Its bytes are guest memory with one change
     * at a texel without paint. The private copies start as the GPU bytes.
     * This frame does not compare. */
    memcpy(gpu0, vm_base + BASE, (size_t)PITCH * ROWS);
    gpu0[9 * PITCH + 9] = 123;
    s_d3d.off_rt[0] = (OffRT){ (void*)1, 0x00A80500, BASE, PITCH, 1, 0, gpu0, ROWS };
    s_d3d.frame_count = 100;
    rsx_deform_batch_end();
    if (!strstr(log_text, "[DEFORM_SHADOW] start target=00A80500 frame=100")
        || strstr(log_text, "frame=100 texels="))
        FAIL("no start line at the first batch end with a GPU surface: %s", log_text);
    if (vm_base[BASE + 9 * PITCH + 9] != 123)
        FAIL("the start did not copy the GPU surface to guest memory");
    for (int k = 0; k < RSX_DEFORM_OP_COUNT; k++)
        if (s->ops[k][9 * PITCH + 9] != 123) FAIL("the private copies do not start as the GPU bytes");
    log_text[0] = 0;
    quad(0, 10, 20, 30, 40, path, 174, 0);
    quad(6, 200, 202, 0, 2, path, 0, 0);
    dr.vertex_count = 6;
    deform_paint(&dr, 0, 6, 0, 5);
    /* The stamp at rows 0 to 2 makes the painter compute row 0 of level 1. */
    deform_paint(&dr, 6, 6, 0, 5);
    current = deform_image(s);
    for (u32 i = 0; i < (size_t)PITCH * ROWS; i += 997)
        if (vm_base[BASE + i] && i != 500 * PITCH + 12 && i != 9 * PITCH + 9)
            FAIL("the shadow painter wrote guest memory");
    memset(before, 0, (size_t)PITCH * ROWS);
    memcpy(expected, before, (size_t)PITCH * ROWS);
    expected[500 * PITCH + 12] = 77;
    expected[9 * PITCH + 9] = 123;      /* the GPU byte of the start */
    /* The brush of the fixture lies in the range of level 0: guest memory
     * holds its bytes, and so does each private copy. */
    for (u32 i = 0; i < 32; i++) expected[511 * PITCH + i] = (u8)(7 * i + 3);
    expect_path(10, 20, 30, 40, 174);
    expect_path(200, 202, 0, 2, 0);
    compare("shadow path stamps");
    for (u32 y = 30; y < 40; y++)
        for (u32 x = 10; x < 20; x++) {
            size_t at = (size_t)y * PITCH + x;
            for (int k = 0; k < RSX_DEFORM_OP_COUNT; k++)
                if (s->ops[k][at] != s_deform_ops[k].fn(0, path_texel(x, 10, 20), 174.0f / 255.0f))
                    FAIL("the image of operation %d is not painted", k);
        }
    if (rsx_deform_serve_range(BASE + 5, 4) || rsx_deform_owned(0x00A80500))
        FAIL("the shadow mode took the read paths of the mode on");
    const uint32_t read[4] = { 30 * PITCH + 10, 30 * PITCH + 11, 31 * PITCH + 10, 31 * PITCH + 11 };
    if (rsx_deform_sample(BASE, read, out) != 4 || out[0] != current[read[0]]
        || out[1] != current[read[1]] || out[2] != current[read[2]] || out[3] != current[read[3]])
        FAIL("a read in the shadow mode does not return the private copy");

    /* The GPU holds the private copy with two changed bytes. */
    memcpy(gpu0, current, (size_t)PITCH * ROWS);
    size_t a = (size_t)30 * PITCH + 10, b = (size_t)30 * PITCH + 15;
    gpu0[a] = (u8)(gpu0[a] < 255 ? gpu0[a] + 1 : gpu0[a] - 1);
    gpu0[b] = (u8)(gpu0[b] >= 7 ? gpu0[b] - 7 : gpu0[b] + 7);
    s_d3d.off_rt[0] = (OffRT){ (void*)1, 0x00A80500, BASE, PITCH, 1, 0, gpu0, ROWS };
    s_d3d.frame_count = 101;
    rsx_deform_batch_end();
    size_t logged = strlen(log_text);
    if (!logged) FAIL("no [DEFORM_SHADOW] line after a compare");
    if (memcmp(vm_base + BASE, gpu0, (size_t)PITCH * ROWS))
        FAIL("the compare did not copy the GPU surface to guest memory");
    if (!logged_line("101", "texels=104 equal=102 diff1=1 diffmax=7 first=0001F00")
        || strstr(log_text, "mip_") || strstr(log_text, "neighbour="))
        FAIL("the first compare line is wrong: %s", log_text);
    /* A second batch end in the frame: no line. */
    rsx_deform_batch_end();
    if (strlen(log_text) != logged) FAIL("a second compare in one frame");
    rsx_deform_wheel_log(BASE, 30 * PITCH + 10, "bilinear", cpu, gpu);
    if (strlen(log_text) == logged) FAIL("no wheel line in the shadow mode");
    logged = strlen(log_text);
    /* A second stamp of the frame does not compare in that frame. */
    quad(12, 40, 50, 30, 40, path, 174, 0);
    dr.vertex_count = 6;
    deform_paint(&dr, 12, 6, 0, 5);
    rsx_deform_batch_end();
    if (strlen(log_text) != logged) FAIL("a second compare in one frame");
    /* The next frame compares the stamp. The GPU holds it. */
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    s_d3d.off_rt[0].gpu_generation = 2;
    s_d3d.frame_count = 102;
    rsx_deform_batch_end();
    if (strlen(log_text) == logged) FAIL("no compare of the stamp of the frame before");
    if (!logged_line("102", "texels=100 equal=100 diff1=0 diffmax=0 first=00000000"))
        FAIL("the second compare line is wrong: %s", log_text);
    logged = strlen(log_text);
    /* The GPU surface is another resource: the backend created it again. Its
     * bytes are the private copies with one change at a texel without paint,
     * after a stamp that is not compared. The private copies start again as
     * the GPU bytes. This frame does not compare. */
    quad(14, 100, 110, 30, 40, path, 174, 0);
    deform_paint(&dr, 14, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    gpu0[8 * PITCH + 8] = 222;
    s_d3d.off_rt[0] = (OffRT){ (void*)2, 0x00A80500, BASE, PITCH, 1, 0, gpu0, ROWS };
    s_d3d.frame_count = 103;
    rsx_deform_batch_end();
    if (!strstr(log_text + logged, "[DEFORM_SHADOW] start target=00A80500 frame=103")
        || strstr(log_text + logged, "frame=103 texels="))
        FAIL("no start line after the backend created the GPU surface again: %s", log_text + logged);
    for (int k = 0; k < RSX_DEFORM_OP_COUNT; k++)
        if (s->ops[k][8 * PITCH + 8] != 222) FAIL("the private copies do not start again as the GPU bytes");
    logged = strlen(log_text);
    /* A frame without a painted column: no line. The GPU mip row is not a
     * reference: the emulator does not render it. */
    s_d3d.off_rt[1] = (OffRT){ (void*)1, 0x01A80500, MIP, MIP_TEXELS, 5, 0, gpu1, 1 };
    s_d3d.frame_count = 104;
    rsx_deform_batch_end();
    if (strlen(log_text) != logged) FAIL("a compare without a painted column");
    /* A third frame with a stamp: the third compare, the last of
     * RSX_DEFORM_LOG frames. */
    quad(18, 60, 70, 30, 40, path, 174, 0);
    deform_paint(&dr, 18, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    s_d3d.off_rt[0].gpu_generation = 3;
    s_d3d.frame_count = 105;
    logged = strlen(log_text);
    rsx_deform_batch_end();
    if (!logged_line("105", "texels=100 equal=100 diff1=0 diffmax=0 first=00000000")
        || !strstr(log_text, "[DEFORM_SHADOW] done target=00A80500 frames=5 texels=304 diff=2 diffmax=7"))
        FAIL("the third compare line is wrong: %s", log_text);
    logged = strlen(log_text);
    /* The periodic [DEFORM] line of the frame can come with a paint. */
    quad(24, 80, 90, 30, 40, path, 174, 0);
    deform_paint(&dr, 24, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    s_d3d.off_rt[0].gpu_generation = 4;
    s_d3d.frame_count = 106;
    logged = strlen(log_text);
    rsx_deform_batch_end();
    if (strlen(log_text) != logged) FAIL("a compare past RSX_DEFORM_LOG frames");
    puts("PASS shadow: private copies start as the GPU bytes, compare once a frame, wheel line");
    puts(log_text);
}

static u32 le32(const unsigned char* p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static float lefloat(const unsigned char* p)
{
    u32 bits = le32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* RSX_DEFORM_DUMP=2 writes the brushes, the edges and the two first frames
 * with a difference. The third frame with a difference is past the switch. */
static void check_dump(void)
{
    const char* file = getenv("RSX_DEFORM_DUMP_FILE");
    if (!file || !*file) FAIL("the check must set RSX_DEFORM_DUMP_FILE");
    set_switch("RSX_DEFORM_DUMP", "2");
    s_deform_dump_frames = 0;
    configure("00A80500", "1", "1");
    set_switch("RSX_DEFORM_LOG", "6");
    s_deform_dumped = 0;
    s_deform_dump_edges = 0;
    state_init();
    if (rsx_deform_mode() != 2 || s_deform_dump_frames != 2) FAIL("RSX_DEFORM_DUMP=2 is not read");
    path_brush();
    /* A stamp that covers no pixel makes the record. */
    for (u32 i = 12; i < 18; i++) vertex(i, -1.0, -1.0, 0.5, 0.5, 174);
    deform_paint(&dr, 12, 6, 0, 5);
    RsxDeformSurface* s = &s_deform_surface[0];
    if (!s->paints || !s_deform_dump_file) FAIL("the dump is not open");
    current = deform_image(s);
    /* The GPU surface appears before a compare: the private copies start as
     * its bytes, guest memory. Nothing is written. */
    memcpy(gpu0, current, (size_t)PITCH * ROWS);
    s_d3d.off_rt[0] = (OffRT){ (void*)1, 0x00A80500, BASE, PITCH, 1, 0, gpu0, ROWS };
    s_d3d.frame_count = 100;
    rsx_deform_batch_end();
    if (s_deform_dumped) FAIL("the start frame is written to the dump");
    quad(0, 10, 20, 30, 40, path, 174, 0);
    dr.vertex_count = 6;
    deform_paint(&dr, 0, 6, 0, 5);
    current = deform_image(s);
    /* Frame 1: one byte differs. The frame is written. */
    memcpy(gpu0, current, (size_t)PITCH * ROWS);
    gpu0[30 * PITCH + 10] = (u8)(gpu0[30 * PITCH + 10] + 1);
    s_d3d.off_rt[0] = (OffRT){ (void*)1, 0x00A80500, BASE, PITCH, 1, 0, gpu0, ROWS };
    s_d3d.frame_count = 101;
    rsx_deform_batch_end();
    if (s_deform_dumped != 1) FAIL("the frame with a difference is not written");
    long first = ftell(s_deform_dump_file);
    /* Frame 2: no difference. Nothing is written. */
    quad(6, 40, 50, 30, 40, path, 174, 0);
    deform_paint(&dr, 6, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    s_d3d.off_rt[0].gpu_generation = 2;
    s_d3d.frame_count = 102;
    rsx_deform_batch_end();
    if (s_deform_dumped != 1 || ftell(s_deform_dump_file) != first)
        FAIL("a frame without a difference is written");
    /* Frame 3: a difference. The second frame is written: the last. */
    quad(12, 60, 70, 30, 40, path, 174, 0);
    deform_paint(&dr, 12, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    gpu0[35 * PITCH + 65] = (u8)(gpu0[35 * PITCH + 65] ^ 3);
    s_d3d.off_rt[0].gpu_generation = 3;
    s_d3d.frame_count = 103;
    rsx_deform_batch_end();
    if (s_deform_dumped != 2) FAIL("the second frame with a difference is not written");
    long second = ftell(s_deform_dump_file);
    /* Frame 4: a difference past RSX_DEFORM_DUMP. Nothing is written. */
    quad(18, 80, 90, 30, 40, path, 174, 0);
    deform_paint(&dr, 18, 6, 0, 5);
    memcpy(gpu0, deform_image(s), (size_t)PITCH * ROWS);
    if (s->paint_n) FAIL("paint notes past the dump budget");
    gpu0[32 * PITCH + 82] = (u8)(gpu0[32 * PITCH + 82] ^ 1);
    s_d3d.off_rt[0].gpu_generation = 4;
    s_d3d.frame_count = 104;
    rsx_deform_batch_end();
    if (ftell(s_deform_dump_file) != second) FAIL("a frame past RSX_DEFORM_DUMP is written");
    fclose(s_deform_dump_file);
    s_deform_dump_file = NULL;
    set_switch("RSX_DEFORM_DUMP", "0");
    s_deform_dump_frames = 0;

    static unsigned char bytes[1 << 16];
    FILE* f = fopen(file, "rb");
    if (!f) FAIL("cannot read the dump");
    size_t size = fread(bytes, 1, sizeof bytes, f), at;
    fclose(f);
    if (size < 12 || memcmp(bytes, "DFRM", 4) || le32(bytes + 4) != 1 || le32(bytes + 8) != 28)
        FAIL("the dump header is wrong");
    u32 edges = 0, brushes = 0, frames = 0, ends = 0;
    for (at = 12; at + 4 <= size;) {
        u32 tag = le32(bytes + at);
        if (tag == 0x45) {
            u32 part = le32(bytes + at + 4), count = part == 2 ? MIP_TEXELS : PITCH;
            at += 8;
            for (u32 i = 0; i < count; i++)
                if (bytes[at + i]) FAIL("the edges record holds a byte that is not 0");
            at += count;
            edges++;
        } else if (tag == 0x42) {
            u32 slot = le32(bytes + at + 4), w = le32(bytes + at + 8), h = le32(bytes + at + 12);
            at += 24;
            if (slot == 0 && w == 32 && h == 1 && (bytes[at] != 3 || bytes[at + 31] != 220))
                FAIL("the path brush is wrong in the dump");
            at += (size_t)w * h;
            brushes++;
        } else if (tag == 0x46) {
            u32 rows = le32(bytes + at + 12);
            at += 32;
            for (u32 r = 0; r < rows; r++) {
                u32 y = le32(bytes + at), x0 = le32(bytes + at + 4), x1 = le32(bytes + at + 8);
                at += 12;
                for (u32 x = x0; x < x1; x++, at += 28) {
                    if (frames || y != 30 || x != 10) continue;
                    u32 brush = path_texel(10, 10, 20);
                    if (bytes[at] != current[30 * PITCH + 10] || bytes[at + 1] != bytes[at] + 1
                        || bytes[at + 2] != 1 || bytes[at + 3] != 0 || bytes[at + 4] != brush
                        || bytes[at + 5] != 0 || le32(bytes + at + 20) != 1 || le32(bytes + at + 24) != 0
                        || fabsf(lefloat(bytes + at + 16) - 174.0f / 255.0f) > 1e-6f)
                        FAIL("the first texel record is wrong");
                }
            }
            frames++;
        } else if (tag == 0x58) {
            at += 4;
            ends++;
        } else FAIL("unknown dump record %08X", tag);
    }
    if (edges != 3 || brushes != 1 || frames != 2 || ends != 1 || at != size)
        FAIL("dump records: edges %u, brushes %u, frames %u, ends %u, end at %u of %u",
             edges, brushes, frames, ends, (u32)at, (u32)size);
    puts("PASS dump: header, brush, edges, two frames with a difference, none past the switch");
}

int main(void)
{
    vm_base = (uint8_t*)calloc(MIP + GUARD, 1);
    gpu0 = (u8*)malloc((size_t)PITCH * ROWS);
    gpu1 = (u8*)malloc(MIP_TEXELS);
    before = (u8*)malloc((size_t)PITCH * ROWS);
    expected = (u8*)malloc((size_t)PITCH * ROWS);
    if (!vm_base || !gpu0 || !gpu1 || !before || !expected) FAIL("no memory");
    check_off();
    check_formula();
    check_on();
    check_shadow();
    check_dump();
    puts(log_text);
    return 0;
}
