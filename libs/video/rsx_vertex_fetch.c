/*
 * ps3recomp - RSX guest vertex fetch (see rsx_vertex_fetch.h)
 */
#include "rsx_vertex_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline float rd_bef(const u8* p)
{
    u32 w; memcpy(&w, p, 4);
    w = ((w >> 24) & 0xFFu) | ((w >> 8) & 0xFF00u) |
        ((w << 8) & 0xFF0000u) | ((w << 24) & 0xFF000000u);
    float f; memcpy(&f, &w, 4); return f;
}

static inline float rd_half_be(const u8* p)
{
    u16 h = (u16)((p[0] << 8) | p[1]);
    u32 sgn = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, f;
    if (exp == 0)        f = sgn << 31;
    else if (exp == 31)  f = (sgn << 31) | 0x7F800000u | (man << 13);
    else                 f = (sgn << 31) | ((exp - 15u + 127u) << 23) | (man << 13);
    float o; memcpy(&o, &f, 4); return o;
}

float rsx_rd_bef(const u8* p) { return rd_bef(p); }
float rsx_rd_half_be(const u8* p) { return rd_half_be(p); }

/* VP_NOFREQ: immutable launch switch, cached once per calling thread. */
static int vertex_nofreq(void)
{
    static _Thread_local int nofreq = -1;
    if (nofreq < 0) nofreq = getenv("VP_NOFREQ") ? 1 : 0;
    return nofreq;
}

/* Types the prepared loop decodes itself; any other type goes through
 * rsx_fetch_attrib (which reads it as float32 and can log it). */
static inline int prepared_type(u32 type) { return type >= 1 && type <= 7; }

void rsx_vertex_fetch_prepare(rsx_vertex_fetch_batch* batch, const rsx_state* st)
{
    batch->state = st;
    batch->count = 0;
    for (u32 i = 0; i < 16; ++i)
        if (st->vertex_attribs[i].enabled && st->vertex_attribs[i].stride)
            batch->active[batch->count++] = (u8)i;
    if (batch->count > 8) return;
    memcpy(batch->constants, st->vertex_data4f, sizeof(batch->constants));
    batch->nofreq = vertex_nofreq();
    for (u32 i = 0; i < batch->count; ++i) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[batch->active[i]];
        rsx_vertex_fetch_slot* s = &batch->slots[i];
        u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
        s->offset = a->offset & 0x7FFFFFFFu;
        s->location = (a->offset & 0x80000000u) ? 0 : 1;   /* MAIN : LOCAL */
        s->stride = a->stride;
        s->frequency = a->frequency;
        s->modulo = (st->frequency_divider_op & (1u << batch->active[i])) ? 1 : 0;
        s->slot = batch->active[i];
        s->type = a->type;
        s->n = (u8)n;
    }
}

void rsx_fetch_vertex_prepared(const rsx_vertex_fetch_batch* batch, u32 vi,
                               rsx_vertex_value out[16])
{
    if (batch->count > 8) {
        for (u32 i = 0; i < 16; ++i)
            rsx_fetch_attrib(batch->state, (int)i, vi, out[i].v);
        return;
    }
    memcpy(out, batch->constants, sizeof(batch->constants));
    if (!vm_base) return;          /* every slot keeps its constant register */
    for (u32 i = 0; i < batch->count; ++i) {
        const rsx_vertex_fetch_slot* s = &batch->slots[i];
        float* o = out[s->slot].v;
        if (!prepared_type(s->type)) {
            rsx_fetch_attrib(batch->state, (int)s->slot, vi, o);
            continue;
        }
        u32 ei = vi;
        if (s->frequency > 1 && !batch->nofreq)
            ei = s->modulo ? (vi % s->frequency) : (vi / s->frequency);
        const u8* p = vm_base + cellGcmResolveLocated(s->location, s->offset + ei * s->stride);
        u32 n = s->n;
        switch (s->type) {
        case 2: for (u32 k = 0; k < n; k++) o[k] = rd_bef(p + k * 4); break;
        case 3: for (u32 k = 0; k < n; k++) o[k] = rd_half_be(p + k * 2); break;
        case 4: for (u32 k = 0; k < n; k++) o[k] = p[k] / 255.0f; break;
        case 1:
            for (u32 k = 0; k < n; k++) {
                s16 v = (s16)((p[k*2] << 8) | p[k*2+1]);
                o[k] = (float)v / 32767.0f;
            }
            break;
        case 5: for (u32 k = 0; k < n; k++) o[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]); break;
        case 6: {
            u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FFu), y = (s32)((w >> 11) & 0x7FFu), z = (s32)((w >> 22) & 0x3FFu);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            o[0] = (float)x / 1023.0f; o[1] = (float)y / 1023.0f; o[2] = (float)z / 511.0f; o[3] = 1.0f;
            break;
        }
        default: /* 7: UB256 */ for (u32 k = 0; k < n; k++) o[k] = (float)p[k]; break;
        }
    }
}

void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{
    /* A disabled attribute array feeds the CONSTANT vertex attribute register
     * (NV4097_SET_VERTEX_DATA4F_M), not zero -- same rule as glColor4f with
     * the colour array off. Defaulting these to black multiplied Rubber
     * Ducky's duck texture away in the fragment program. Components the array
     * does not supply (size < 4) keep the register's value too. */
    out[0] = st->vertex_data4f[idx][0];
    out[1] = st->vertex_data4f[idx][1];
    out[2] = st->vertex_data4f[idx][2];
    out[3] = st->vertex_data4f[idx][3];

    const rsx_vertex_attrib* a = &st->vertex_attribs[idx];
    if (!a->enabled || a->stride == 0 || !vm_base) return;

    /* Vertex frequency divisor (instancing). freq 0/1 = per-vertex. For
     * freq > 1 the element index is either vertex/freq (DIVIDE: per-instance
     * data advances once per freq verts) or vertex%freq (MODULO: a mesh
     * repeats every freq verts). NV4097_SET_FREQUENCY_DIVIDER_OPERATION's
     * per-attribute bit selects MODULO. DeferredShading's cube rings need
     * this: the shared cube mesh is MODULO, the per-instance transform in
     * attrib9 is DIVIDE -- without it attrib9 advanced every vertex and the
     * instanced cubes drew with garbage transforms (only the baseplate,
     * which isn't instanced, survived). */
    u32 ei = vi;
    if (a->frequency > 1) {
        if (!vertex_nofreq())                   /* VP_NOFREQ: instancing kill-switch */
            ei = (st->frequency_divider_op & (1u << idx))
                     ? (vi % a->frequency)      /* MODULO: repeat mesh */
                     : (vi / a->frequency);     /* DIVIDE: per-instance */
    }

    /* Bit 31 of the vertex-array OFFSET selects the context DMA: 0 = LOCAL
     * (VRAM), 1 = MAIN (IO-mapped system memory). Resolve LOCAL as local --
     * routing it through cellGcmResolveOffset lets the IO table win for any
     * offset whose page the IO region also covers. One title's vertex arrays
     * sit at offsets like 0x4480, shadowed by its 1MB IO window at 0x11100000:
     * the array was uploaded to VRAM 0xC0004480 but resolved to main
     * 0x11104480, which is empty -- so every INDEXED mesh fetched zeros and
     * collapsed to the origin, while non-indexed geometry whose arrays lie
     * outside the shadowed pages drew fine. */
    u32 off = (a->offset & 0x7FFFFFFFu) + ei * a->stride;
    const u8* p = vm_base + ((a->offset & 0x80000000u)
        ? cellGcmResolveLocated(0, off)   /* MAIN:  IO offset table          */
        : cellGcmResolveLocated(1, off)); /* LOCAL: VRAM, never the IO table */

    u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
    switch (a->type) {
    case 2: /* CELL_GCM_VERTEX_F: float32 BE */
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_bef(p + k * 4);
        break;
    case 3: /* SF: half float BE */
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_half_be(p + k * 2);
        break;
    case 4: /* UB: u8 normalized [0,1] */
        for (u32 k = 0; k < n; k++) out[k] = p[k] / 255.0f;
        break;
    case 1: /* S1: s16 normalized [-1,1] */
        for (u32 k = 0; k < n; k++) {
            s16 s = (s16)((p[k*2] << 8) | p[k*2+1]);
            out[k] = (float)s / 32767.0f;
        }
        break;
    case 5: /* S32K: s16 integer */
        for (u32 k = 0; k < n; k++)
            out[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]);
        break;
    case 6: { /* CMP: one BE word containing signed 11/11/10-bit XYZ. */
        u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                ((u32)p[2] << 8) | (u32)p[3];
        s32 x = (s32)(w & 0x7FFu);
        s32 y = (s32)((w >> 11) & 0x7FFu);
        s32 z = (s32)((w >> 22) & 0x3FFu);
        if (x & 0x400) x -= 0x800;
        if (y & 0x400) y -= 0x800;
        if (z & 0x200) z -= 0x400;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        out[3] = 1.0f;
        break;
    }
    case 7: /* UB256: u8 unnormalized */
        for (u32 k = 0; k < n; k++) out[k] = (float)p[k];
        break;
    default:
        { static int t = -1; if (t < 0) t = getenv("VTX_TYPEDBG") ? 1 : 0;
          if (t) { static u32 seen = 0;
              if (!(seen & (1u << (a->type & 31)))) { seen |= 1u << (a->type & 31);
                  fprintf(stderr, "[VTXTYPE] UNHANDLED type=%u attr=%d size=%u stride=%u\n",
                          a->type, idx, a->size, a->stride); } } }
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_bef(p + k * 4);
        break;
    }
}
