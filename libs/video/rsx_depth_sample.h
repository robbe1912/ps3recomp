/* Native D24 depth-texture reads (#137): a texture unit whose guest format is
 * D24_D8 and whose address matches a live depth attachment samples that
 * attachment through an R24 view instead of uploading guest memory.
 * API-neutral: guest registers in, shader view identity and HLSL out. */
#ifndef RSX_DEPTH_SAMPLE_H
#define RSX_DEPTH_SAMPLE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t enabled, compare, func, linear, unnormalized;
    uint32_t width, height, resource_width, resource_height;
    uint32_t debug;   /* diagnostics only: 0 normal, 1 lit (1.0), 2 shadowed (0.0),
                       * 3 stored depth, 4 reference depth */
} rsx_depth_sample_view;

/* D24_D8 with the LN/UN layout bits ignored. D24 float and D16 stay on the
 * existing path. */
static int rsx_depth_sample_format(uint32_t format)
{
    return (format & 0x9Fu) == 0x90u;
}

/* 1: native view filled; 0: not a D24 texture; -1: a D24 read this adapter
 * cannot express exactly (the caller keeps the existing path and logs it).
 * RSX compares when the unit is enabled, alpha kill is off and the
 * TEXTURE_ADDRESS zfunc [31:28] is neither NEVER (0) nor ALWAYS (7); NEVER
 * and ALWAYS read the depth value itself. */
static int rsx_depth_sample_view_init(rsx_depth_sample_view* v, uint32_t format,
    uint32_t width, uint32_t height, uint32_t mips, int cube, uint32_t ctrl1,
    uint32_t address, uint32_t filter, uint32_t control0,
    uint32_t resource_width, uint32_t resource_height)
{
    memset(v, 0, sizeof(*v));
    if (!rsx_depth_sample_format(format) || !(control0 & 0x80000000u)) return 0;
    uint32_t func = (address >> 28) & 15u;
    uint32_t min = (filter >> 16) & 7u, mag = (filter >> 24) & 7u;
    int min_linear = min == 2 || min == 4 || min == 6, mag_linear = mag == 2;
    if (cube || mips > 1 || !width || !height || width > resource_width ||
        height > resource_height || func > 7 || min < 1 || min > 6 ||
        mag < 1 || mag > 2 || min_linear != mag_linear ||
        (ctrl1 && (ctrl1 & 0xFFFFu) != 0xAAE4u)) return -1;
    /* Hardware addressing wraps the whole native allocation, so a view smaller
     * than it is only exact when that axis clamps to edge (clamped in guest
     * texels in the helper below). */
    if ((width < resource_width && (address & 15u) != 3u) ||
        (height < resource_height && ((address >> 8) & 15u) != 3u)) return -1;
    v->enabled = 1;
    v->compare = !(control0 & 4u) && func != 0 && func != 7;
    v->func = func;
    v->linear = (uint32_t)min_linear;
    v->unnormalized = (format & 0x40u) != 0;
    v->width = width; v->height = height;
    v->resource_width = resource_width; v->resource_height = resource_height;
    return 1;
}

/* RSX compares stored_depth FUNC reference; D3D SampleCmp compares reference
 * FUNC stored_depth. Ordered functions reverse; equal/not-equal keep their
 * operator. Values are D3D12_COMPARISON_FUNC (NEVER=1 .. ALWAYS=8). */
static uint32_t rsx_depth_sample_d3d_func(uint32_t rsx_func)
{
    static const uint32_t map[8] = {1, 5, 3, 7, 2, 6, 4, 8};
    return map[rsx_func & 7u];
}

static uint64_t rsx_depth_sample_key(const rsx_depth_sample_view v[16])
{
    uint64_t h = 14695981039346656037ull;
    int any = 0;
    for (unsigned u = 0; u < 16; ++u) {
        if (!v[u].enabled) continue;
        any = 1;
        const uint32_t words[] = {u, v[u].compare, v[u].func, v[u].linear, v[u].unnormalized,
            v[u].width, v[u].height, v[u].resource_width, v[u].resource_height, v[u].debug};
        for (unsigned k = 0; k < sizeof words / sizeof words[0]; ++k)
            for (unsigned b = 0; b < 4; ++b) {
                h ^= (words[k] >> (8 * b)) & 255u;
                h *= 1099511628211ull;
            }
    }
    return any ? (h | 1u) : 0;
}

/* HLSL helper _rsx_depthN(float3 p) for TEX/TXP (TXP divides xyz by w first).
 * The comparison result, or the depth value, is replicated into RGBA. The
 * reference is clamped to 0..1 like the D24 value it is compared with. */
static int rsx_depth_sample_hlsl(char* out, size_t capacity, unsigned unit,
    const rsx_depth_sample_view* v)
{
    if (!v || v->enabled != 1 || unit >= 16 || !v->width || !v->height ||
        v->width > v->resource_width || v->height > v->resource_height) return -1;
    char body[768];
    int n = v->unnormalized ?
        snprintf(body, sizeof body,
            "float4 _rsx_depth%u(float3 p) {\n float2 uv=p.xy / float2(%u.0,%u.0);\n",
            unit, v->width, v->height) :
        snprintf(body, sizeof body, "float4 _rsx_depth%u(float3 p) {\n float2 uv=p.xy;\n", unit);
    if (n < 0 || (size_t)n >= sizeof body) return -1;
    for (unsigned a = 0; a < 2; ++a) {
        unsigned size = a ? v->height : v->width;
        unsigned resource = a ? v->resource_height : v->resource_width;
        if (size == resource) continue;
        char axis = a ? 'y' : 'x';
        int k = snprintf(body + n, sizeof body - (size_t)n,
            " uv.%c=clamp(uv.%c,0.5/%u.0,1.0-0.5/%u.0)*(%u.0/%u.0);\n",
            axis, axis, size, size, size, resource);
        if (k < 0 || (size_t)k >= sizeof body - (size_t)n) return -1;
        n += k;
    }
    int k;
    if (v->debug == 1 || v->debug == 2)
        k = snprintf(out, capacity, "%s return (float4)%s;\n}\n", body, v->debug == 1 ? "1.0" : "0.0");
    else if (v->debug == 3)
        k = snprintf(out, capacity, "%s return (float4)rsx_tex%u.Load(int3(int2(saturate(uv)*float2(%u.0,%u.0)),0)).r;\n}\n",
                     body, unit, v->resource_width - 1, v->resource_height - 1);
    else if (v->debug == 4)
        k = snprintf(out, capacity, "%s return (float4)saturate(p.z);\n}\n", body);
    else k = v->compare ?
        snprintf(out, capacity, "%s return (float4)rsx_tex%u.SampleCmpLevelZero(rsx_cmp%u,uv,saturate(p.z));\n}\n",
                 body, unit, unit) :
        snprintf(out, capacity, "%s return (float4)rsx_tex%u.SampleLevel(rsx_samp%u,uv,0.0).r;\n}\n",
                 body, unit, unit);
    return k < 0 || (size_t)k >= capacity ? -1 : k;
}
#endif
