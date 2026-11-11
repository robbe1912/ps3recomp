/* ps3recomp: sample guest XY16F words stored through an RGBA8 render target.
 * The guest A,R,G,B bytes are host texel lanes W,X,Y,Z. Decode each half
 * before filtering; filtering packed bytes first changes the represented value.
 * This adapter serves the backend's existing one-mip static sampler states.
 */
#ifndef PS3RECOMP_RSX_TEXTURE_ALIAS_H
#define PS3RECOMP_RSX_TEXTURE_ALIAS_H
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rsx_texture_alias_xy16(unsigned texture_format, int rgba8_surface)
{
    return rgba8_surface && (texture_format & 0x9fu) == 0x9fu;
}

static unsigned long long rsx_texture_alias_key(unsigned mask,
    const unsigned remap[16], unsigned point_clamp_mask)
{
    if (!mask) return 0;
    unsigned long long h = 1469598103934665603ull;
    h = (h ^ mask) * 1099511628211ull;
    h = (h ^ (point_clamp_mask & mask)) * 1099511628211ull;
    for (unsigned u = 0; u < 16; ++u)
        if (mask & (1u << u)) h = (h ^ remap[u]) * 1099511628211ull;
    return h;
}

static int rsx_alias_replace(char* text, size_t capacity,
                             const char* before, const char* after)
{
    int count = 0;
    char* p = text;
    size_t a = strlen(before), b = strlen(after);
    while ((p = strstr(p, before)) != NULL) {
        size_t used = strlen(text), tail = strlen(p + a);
        if (used - a + b + 1 > capacity) return -1;
        memmove(p + b, p + a, tail + 1);
        memcpy(p, after, b);
        p += b;
        ++count;
    }
    return count;
}

/* Apply after unnormalized-coordinate scaling. Units in point_clamp_mask
 * use point/clamp; other units use linear/wrap, matching the static samplers.
 * A mask/cube overlap or an insufficient output buffer is an explicit error.
 * The caller must include the mask, remaps and sampler policy in PSO identity.
 */
static int rsx_texture_alias_apply(char* hlsl, size_t capacity, unsigned mask,
    const unsigned remap[16], unsigned cube_mask, unsigned point_clamp_mask)
{
    if (!mask) return 0;
    if (!hlsl || !remap || (mask & (cube_mask | (cube_mask >> 16)))) return -1;   /* cube or 3D unit */
    char* helpers = (char*)calloc(1, 65536);
    if (!helpers) return -1;
    size_t used = 0;
    int count = 0;
    const char* shared =
        "float4 rsx_alias_half(float4 c) {\n"
        " uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        " return float4(f16tof32((b.w << 8) | b.x),\n"
        "               f16tof32((b.y << 8) | b.z), 0.0, 1.0);\n}\n"
        "float4 rsx_alias_read(Texture2D<float4> t, int2 p, int2 n, bool is_point) {\n"
        " p = is_point ? clamp(p, int2(0,0), n-1) : ((p % n + n) % n);\n"
        " return rsx_alias_half(t.Load(int3(p,0)));\n}\n";
    used = strlen(shared);
    memcpy(helpers, shared, used + 1);
    for (unsigned u = 0; u < 16; ++u) {
        if (!(mask & (1u << u))) continue;
        char tex[32], before[96], after[64], wrapper[2048];
        if (strstr(hlsl, "rsx_tex[16]")) snprintf(tex, sizeof tex, "rsx_tex[%u]", u);
        else snprintf(tex, sizeof tex, "rsx_tex%u", u);
        const char* methods[] = {"Sample", "SampleLevel", "SampleBias"};
        for (unsigned m = 0; m < 6; ++m) {
            /* m >= 3: individual sampler names (programs with native depth units). */
            snprintf(before, sizeof before, m < 3 ? "%s.%s(rsx_samp[%u]," : "%s.%s(rsx_samp%u,",
                     tex, methods[m % 3], u);
            snprintf(after, sizeof after, "rsx_alias_%u%s(", u, m % 3 ? "_lod" : "");
            int n = rsx_alias_replace(hlsl, capacity, before, after);
            if (n < 0) { free(helpers); return -1; }
            count += n;
        }
        unsigned char lanes[4];
        rsx_texture_component_remap(remap[u], 0x9fu, lanes);
        char selected[4][16];
        for (unsigned c = 0; c < 4; ++c) {
            unsigned s = lanes[c];
            if (s < 4) snprintf(selected[c], sizeof selected[c], "v[%u]", s);
            else snprintf(selected[c], sizeof selected[c], "%s", s == 4 ? "0.0" : "1.0");
        }
        char body[1024];
        if (point_clamp_mask & (1u << u))
            snprintf(body, sizeof body,
                " v=rsx_alias_read(%s,int2(floor(uv*float2(w,h))),n,true);\n", tex);
        else
            snprintf(body, sizeof body,
                " float2 p=frac(uv)*float2(w,h)-0.5; int2 i=int2(floor(p)); float2 f=frac(p);\n"
                " v=lerp(lerp(rsx_alias_read(%s,i,n,false),rsx_alias_read(%s,i+int2(1,0),n,false),f.x),\n"
                "        lerp(rsx_alias_read(%s,i+int2(0,1),n,false),rsx_alias_read(%s,i+1,n,false),f.x),f.y);\n",
                tex, tex, tex, tex);
        int n = snprintf(wrapper, sizeof wrapper,
            "float4 rsx_alias_%u(float2 uv) {\n"
            " uint w,h; %s.GetDimensions(w,h); int2 n=int2(w,h);\n"
            " float4 v;\n%s"
            " return float4(%s,%s,%s,%s);\n}\n"
            "float4 rsx_alias_%u_lod(float2 uv, float lod) { return rsx_alias_%u(uv); }\n",
            u, tex, body,
            selected[1], selected[2], selected[3], selected[0], u, u);
        if (n < 0 || (size_t)n >= sizeof wrapper) { free(helpers); return -1; }
        if (used + (size_t)n + 1 > 65536) { free(helpers); return -1; }
        memcpy(helpers + used, wrapper, (size_t)n + 1); used += (size_t)n;
    }
    char* main = strstr(hlsl, " main(");
    if (!main || strlen(hlsl) + used + 1 > capacity) { free(helpers); return -1; }
    while (main > hlsl && main[-1] != '\n') --main;
    memmove(main + used, main, strlen(main) + 1);
    memcpy(main, helpers, used);
    free(helpers);
    return count;
}
#endif
