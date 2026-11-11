/* Guest texture sampler state for the D3D12 VP path: addressing, filtering,
 * LOD and anisotropy, recorded per draw and bound as immutable sampler tables.
 *
 * Addressing follows ps3recomp 9c08b94 ("preserve guest sampler addressing").
 * Filtering decodes NV4097_SET_TEXTURE_FILTER / CONTROL0 instead of the old
 * fixed policy (unit 0 point+clamp, every other unit linear+wrap). States this
 * decoder cannot express exactly (legacy GL clamp, mirror-once-border,
 * convolution filters, unenabled units) keep that old fixed policy. */
#ifndef RSX_D3D12_GUEST_SAMPLER_H
#define RSX_D3D12_GUEST_SAMPLER_H
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rsx_depth_sample.h"

typedef struct {
    uint32_t address, filter, control0, border;
} rsx_guest_sampler_regs;

/* The pre-guest-state policy, kept for fallback pipelines and undecodable
 * states: unit 0 point+clamp (the backend's own glyph atlas), others
 * trilinear+wrap. */
static void rsx_guest_sampler_legacy(unsigned unit, D3D12_SAMPLER_DESC* out)
{
    memset(out, 0, sizeof(*out));
    D3D12_TEXTURE_ADDRESS_MODE m = unit == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                             : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out->AddressU = out->AddressV = out->AddressW = m;
    out->Filter = unit == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    out->MaxLOD = D3D12_FLOAT32_MAX;
    out->MaxAnisotropy = 1;
    out->ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
}

static int rsx_guest_address_mode(unsigned value, D3D12_TEXTURE_ADDRESS_MODE* out)
{
    switch (value) {
    case 1: *out = D3D12_TEXTURE_ADDRESS_MODE_WRAP; return 1;
    case 2: *out = D3D12_TEXTURE_ADDRESS_MODE_MIRROR; return 1;
    case 3: *out = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; return 1;   /* clamp to edge */
    case 4: *out = D3D12_TEXTURE_ADDRESS_MODE_BORDER; return 1;
    case 6: *out = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE; return 1;
    default: return 0; /* 5 legacy clamp, 7 mirror-once-border: no D3D equivalent */
    }
}

/* Decode one enabled unit. Returns 0 (out untouched) when the state cannot be
 * expressed exactly; the caller then keeps the legacy policy for that unit. */
static int rsx_guest_sampler(const rsx_guest_sampler_regs* r, D3D12_SAMPLER_DESC* out)
{
    D3D12_SAMPLER_DESC d;
    memset(&d, 0, sizeof(d));
    if (!(r->control0 & 0x80000000u)) return 0;                 /* unit disabled */
    if (!rsx_guest_address_mode(r->address & 15u, &d.AddressU) ||
        !rsx_guest_address_mode((r->address >> 8) & 15u, &d.AddressV) ||
        !rsx_guest_address_mode((r->address >> 16) & 15u, &d.AddressW)) return 0;

    /* FILTER: [12:0] signed 1.4.8 LOD bias, [18:16] min, [26:24] mag.
     * min 1 nearest, 2 linear, 3 nearest/mip-nearest, 4 linear/mip-nearest,
     * 5 nearest/mip-linear, 6 linear/mip-linear, 7 convolution; mag 1/2/4. */
    unsigned min = (r->filter >> 16) & 7u, mag = (r->filter >> 24) & 7u;
    if (min < 1 || min > 6 || mag < 1 || mag > 2) return 0;
    int min_linear = (min == 2 || min == 4 || min == 6);
    int mag_linear = mag == 2;
    int mip_linear = min >= 5;
    int bias = (int)(r->filter & 8191u);
    if (bias & 4096) bias -= 8192;
    d.MipLODBias = (float)bias / 256.0f;

    /* CONTROL0: [6:4] max anisotropy, [18:7] max LOD, [30:19] min LOD (4.8). */
    d.MinLOD = (float)((r->control0 >> 19) & 4095u) / 256.0f;
    d.MaxLOD = (float)((r->control0 >> 7) & 4095u) / 256.0f;
    if (d.MaxLOD < d.MinLOD) return 0;
    if (min <= 2) d.MinLOD = d.MaxLOD = 0.0f;   /* RSX NEAREST/LINEAR: no mips */

    static const unsigned aniso[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    unsigned level = (r->control0 >> 4) & 7u;
    if (level && min_linear && mag_linear && mip_linear) {
        /* D3D12_FILTER_ANISOTROPIC is linear-mip only. Anisotropy with a point
         * mip filter would need the optional MIN_MAG_ANISOTROPIC_MIP_POINT
         * mode; those draws keep their exact min/mag/mip choice at 1x. */
        d.Filter = D3D12_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = aniso[level];
    } else {
        d.Filter = D3D12_ENCODE_BASIC_FILTER(
            min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        d.MaxAnisotropy = 1;
    }
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.BorderColor[0] = (float)((r->border >> 16) & 255u) / 255.0f;
    d.BorderColor[1] = (float)((r->border >> 8) & 255u) / 255.0f;
    d.BorderColor[2] = (float)(r->border & 255u) / 255.0f;
    d.BorderColor[3] = (float)(r->border >> 24) / 255.0f;
    *out = d;
    return 1;
}

/* A use of the individually declared sampler NAME (rsx_samp3, rsx_cmp3) other
 * than its declaration: the name followed by a non-digit and not " :". */
static int rsx_guest_sampler_name_used(const char* hlsl, const char* name)
{
    const size_t n = strlen(name);
    for (const char* at = strstr(hlsl, name); at; at = strstr(at + 1, name)) {
        const char next = at[n];
        if (next >= '0' && next <= '9') continue;
        if (next == ' ' && at[n + 1] == ':') continue;   /* "rsx_samp3 : register(s3)" */
        return 1;
    }
    return 0;
}
/* Units a compiled guest pixel shader samples. The FP decompiler names unit u
 * "rsx_samp[u]" at every sample site (the declaration is rsx_samp[16]); a
 * program with native depth units declares rsx_sampU / rsx_cmpU instead. */
static unsigned rsx_guest_sampler_mask(const char* hlsl)
{
    unsigned mask = 0;
    char token[24];
    for (unsigned u = 0; u < 16; ++u) {
        snprintf(token, sizeof(token), "rsx_samp[%u]", u);
        if (strstr(hlsl, token)) { mask |= 1u << u; continue; }
        snprintf(token, sizeof(token), "rsx_samp%u", u);
        if (rsx_guest_sampler_name_used(hlsl, token)) { mask |= 1u << u; continue; }
        snprintf(token, sizeof(token), "rsx_cmp%u", u);
        if (rsx_guest_sampler_name_used(hlsl, token)) mask |= 1u << u;
    }
    return mask;
}

/* Sampler for a native D24 unit: the guest addressing when it maps exactly
 * (else clamp to edge, which the view helper requires for cropped axes),
 * point or linear without mips, and the reversed comparison when the view
 * compares. */
static void rsx_guest_sampler_depth(const rsx_guest_sampler_regs* r,
                                    const rsx_depth_sample_view* v, D3D12_SAMPLER_DESC* out)
{
    D3D12_SAMPLER_DESC d;
    memset(&d, 0, sizeof(d));
    if (!rsx_guest_address_mode(r->address & 15u, &d.AddressU)) d.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if (!rsx_guest_address_mode((r->address >> 8) & 15u, &d.AddressV)) d.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    const D3D12_FILTER_TYPE t = v->linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    d.Filter = D3D12_ENCODE_BASIC_FILTER(t, t, D3D12_FILTER_TYPE_POINT,
        v->compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.ComparisonFunc = v->compare ? (D3D12_COMPARISON_FUNC)rsx_depth_sample_d3d_func(v->func)
                                  : D3D12_COMPARISON_FUNC_ALWAYS;
    d.MaxAnisotropy = 1;
    d.MinLOD = d.MaxLOD = 0.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
    *out = d;
}

/* Sampled-unit masks by FP structural hash, so record-time draw merging can
 * ignore units the program never samples. Direct-mapped; a miss (never
 * compiled, or evicted) returns every unit, which only costs a merge. */
#define RSX_GUEST_SAMPLER_MASKS 1024u
typedef struct { uint64_t hash; unsigned mask; } rsx_guest_sampler_mask_slot;
static void rsx_guest_sampler_mask_note(rsx_guest_sampler_mask_slot* table,
                                        uint64_t hash, unsigned mask)
{
    if (!hash) return;
    rsx_guest_sampler_mask_slot* s = &table[hash % RSX_GUEST_SAMPLER_MASKS];
    if (s->hash == hash) s->mask |= mask;          /* variants of one program */
    else { s->hash = hash; s->mask = mask; }
}
static unsigned rsx_guest_sampler_mask_for(const rsx_guest_sampler_mask_slot* table,
                                           uint64_t hash)
{
    const rsx_guest_sampler_mask_slot* s = &table[hash % RSX_GUEST_SAMPLER_MASKS];
    return hash && s->hash == hash ? s->mask : 0xFFFFu;
}

/* Two register sets sample identically on one unit: both disabled (the unit
 * then keeps the legacy policy whatever else changed), or every register equal. */
static int rsx_guest_sampler_same(const rsx_guest_sampler_regs* a, const rsx_guest_sampler_regs* b)
{
    if (!(a->control0 & 0x80000000u) && !(b->control0 & 0x80000000u)) return 1;
    return a->address == b->address && a->filter == b->filter &&
           a->control0 == b->control0 && a->border == b->border;
}

/* One draw's 16-unit table. mask = units the bound pipeline's pixel shader
 * samples (0 for the fallback atlas/colour pipelines). Units outside the mask,
 * or without a bound texture, get the canonical legacy sampler, so their
 * registers never create a distinct table. Returns the guest-decoded count. */
static unsigned rsx_guest_sampler_table(const rsx_guest_sampler_regs regs[16],
                                        const int set[16], unsigned mask,
                                        const rsx_depth_sample_view* depth,
                                        D3D12_SAMPLER_DESC out[16])
{
    unsigned decoded = 0;
    for (unsigned u = 0; u < 16; ++u) {
        rsx_guest_sampler_legacy(u, &out[u]);
        if (!(mask & (1u << u)) || !set[u]) continue;
        if (depth && depth[u].enabled) { rsx_guest_sampler_depth(&regs[u], &depth[u], &out[u]); ++decoded; }
        else if (rsx_guest_sampler(&regs[u], &out[u])) ++decoded;
    }
    return decoded;
}
#endif
