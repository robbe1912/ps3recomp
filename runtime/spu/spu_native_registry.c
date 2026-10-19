/* Native image identity and staged-module metadata. Register before execution. */
#include "spu_workload.h"
#include <stdio.h>
#include <string.h>

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL; /* Established registry offset basis. */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            lift_base;
    int                 staged;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

void spu_workload_register_img_at(uint64_t fingerprint, spu_lifted_entry_fn fn,
                                int image_id, uint32_t lift_base,
                                const char* name)
{
    if (!fn) return;
    if (lift_base >= SPU_LS_SIZE) {
        fprintf(stderr, "[spu_workload] invalid native image load base\n");
        return;
    }
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint && s_registry[i].lift_base == lift_base) {
            s_registry[i].fn       = fn;
            s_registry[i].image_id = image_id;
            s_registry[i].lift_base = lift_base;
            s_registry[i].staged = 1;
            s_registry[i].name     = name;
            spu_workload_entry updated = s_registry[i];
            memmove(&s_registry[i], &s_registry[i + 1],
                    (s_registry_count - i - 1) * sizeof s_registry[0]);
            s_registry[s_registry_count - 1] = updated;
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        return;
    }
    s_registry[s_registry_count].fp       = fingerprint;
    s_registry[s_registry_count].fn       = fn;
    s_registry[s_registry_count].image_id = image_id;
    s_registry[s_registry_count].lift_base = lift_base;
    s_registry[s_registry_count].staged = 1;
    s_registry[s_registry_count].name     = name;
    s_registry_count++;
}

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                              int image_id, const char* name)
{
    spu_workload_register_img_at(fingerprint, fn, image_id, 0, name);
    if (s_registry_count && s_registry[s_registry_count - 1].fp == fingerprint &&
        !s_registry[s_registry_count - 1].lift_base)
        s_registry[s_registry_count - 1].staged = 0;
}

uint32_t spu_workload_img_base(uint64_t fingerprint)
{
    for (unsigned i = s_registry_count; i > 0; --i)
        if (s_registry[i - 1].fp == fingerprint) return s_registry[i - 1].lift_base;
    return 0;
}

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    spu_workload_register_img(fingerprint, fn, 0, name);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = s_registry_count; i > 0; --i)
        if (s_registry[i - 1].fp == fingerprint)
            return s_registry[i - 1].fn;
    return NULL;
}

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out)
{
    for (unsigned i = s_registry_count; i > 0; --i)
        if (s_registry[i - 1].fp == fingerprint) {
            if (image_id_out) *image_id_out = s_registry[i - 1].image_id;
            return s_registry[i - 1].fn;
        }
    if (image_id_out) *image_id_out = 0;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* Metadata is published at startup and immutable during execution. A prior
 * match in another context must never authorize different local-store bytes. */
#define SPU_STAGE_PROBE_MAX 64
typedef struct {
    uint64_t fp;
    int image_id;
    uint32_t lift_base, entry, seg_vaddr, seg_size;
    const char* name;
} spu_stage_probe;
static spu_stage_probe s_stage_probes[SPU_STAGE_PROBE_MAX];
static unsigned s_stage_probe_count;
static uint64_t s_stage_entry_words[SPU_LS_SIZE / 256];

void spu_workload_register_stage_probe(uint64_t fingerprint, int image_id,
                                     uint32_t lift_base, uint32_t entry,
                                     uint32_t seg_vaddr, uint32_t seg_size,
                                     const char* name)
{
    uint64_t span = (uint64_t)seg_vaddr + seg_size;
    if (!fingerprint || image_id <= 0 || !seg_size ||
        span > SPU_LS_SIZE || (uint64_t)lift_base + span > SPU_LS_SIZE ||
        entry < seg_vaddr || entry >= span) {
        fprintf(stderr, "[spu_workload] invalid staged native-module metadata\n");
        return;
    }
    unsigned i;
    for (i = 0; i < s_stage_probe_count; ++i)
        if (s_stage_probes[i].fp == fingerprint &&
            s_stage_probes[i].lift_base == lift_base &&
            s_stage_probes[i].entry == entry) break;
    if (i == SPU_STAGE_PROBE_MAX) {
        fprintf(stderr, "[spu_workload] staged native-module registry full\n");
        return;
    }
    s_stage_probes[i] = (spu_stage_probe){
        fingerprint, image_id, lift_base, entry, seg_vaddr, seg_size, name
    };
    uint32_t pc = lift_base + entry;
    s_stage_entry_words[pc >> 8] |= UINT64_C(1) << ((pc >> 2) & 63);
    if (i == s_stage_probe_count) ++s_stage_probe_count;
}

void spu_workload_stage_note_dma(spu_context* ctx, uint32_t ea,
                                 uint32_t lsa, uint32_t size)
{
    if (!ctx || !size || lsa >= SPU_LS_SIZE || size > SPU_LS_SIZE - lsa) return;
    spu_native_note_write(ctx, lsa, size);
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (!ctx->native_dma[i].size) continue;
        if (lsa == ctx->native_dma[i].base + ctx->native_dma[i].size &&
            (uint64_t)ctx->native_dma[i].source_ea + ctx->native_dma[i].size == ea) {
            ctx->native_dma[i].size += size;
            return;
        }
        if (lsa == ctx->native_dma[i].base) slot = i;
    }
    if (slot == 4) slot = ctx->native_dma_next++ & 3;
    ctx->native_dma[slot].base = lsa;
    ctx->native_dma[slot].size = size;
    ctx->native_dma[slot].source_ea = ea;
}

static int stage_probe(const uint8_t*, uint32_t, uint32_t*, int*, uint32_t*,
                       uint32_t*, uint32_t*);

int spu_workload_stage_resolve(spu_context* ctx, uint32_t pc, int* image_out)
{
    if (!ctx || !ctx->ls || pc >= SPU_LS_SIZE) return 0;
    int prior_owner = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!ctx->native_stage[i].image_id || pc < ctx->native_stage[i].base ||
            pc - ctx->native_stage[i].base >= ctx->native_stage[i].span) continue;
        if (pc - ctx->native_stage[i].base < ctx->native_stage[i].verified_offset) {
            if (image_out) *image_out = -1;
            return 1;
        }
        if (ctx->native_stage[i].image_id < 0 && ctx->native_stage[i].retry) {
            int entry = pc == ctx->native_stage[i].entry_pc;
            for (unsigned j = 0; !entry && j < s_stage_probe_count; ++j)
                entry = pc == ctx->native_stage[i].base + s_stage_probes[j].entry;
            if (entry) {
                ctx->native_stage[i].retry = 0;
                prior_owner = 1;
                break;
            }
        }
        if (image_out) *image_out = ctx->native_stage[i].image_id;
        return 1;
    }
    uint32_t word = pc >> 2;
    int written = (int)((ctx->native_requires_owner[word >> 6] >> (word & 63)) & 1);
    if (!written) {
        /* Direct initialization needs probes only at registered native entry
         * addresses. Normal branches never hash unrelated module contents. */
        if (!(s_stage_entry_words[word >> 6] & (UINT64_C(1) << (word & 63))))
            return 0;
    }
    uint32_t base, lift, span, verified_offset = 0;
    int image;
    int found = stage_probe(ctx->ls, pc, &base, &image, &lift, &span, &verified_offset);
    if (!found && written) {
        /* Register_img_at describes a complete staged payload. Hash only on
         * execution entering a written region, never on ordinary data DMA. */
        for (unsigned i = 0; i < 4 && !found; ++i) {
            if (!ctx->native_dma[i].size || pc < ctx->native_dma[i].base ||
                pc - ctx->native_dma[i].base >= ctx->native_dma[i].size) continue;
            uint64_t fp = spu_workload_fingerprint(ctx->ls + ctx->native_dma[i].base,
                                                   ctx->native_dma[i].size);
            for (unsigned pass = 0; pass < 2 && !found; ++pass)
              for (unsigned j = 0; j < s_registry_count; ++j)
                if (s_registry[j].staged && s_registry[j].image_id > 0 &&
                    ((ctx->native_dma[i].base == s_registry[j].lift_base) == (pass == 0)) &&
                    s_registry[j].fp == fp) {
                    base = ctx->native_dma[i].base;
                    span = ctx->native_dma[i].size;
                    image = s_registry[j].image_id;
                    lift = s_registry[j].lift_base;
                    found = 1; break;
                }
        }
    }
    if (!found) {
        if (image_out) *image_out = -1;
        return prior_owner ? 1 : written ? 2 : 0;
    }
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (ctx->native_stage[i].image_id && base < ctx->native_stage[i].base +
            ctx->native_stage[i].span && base + span > ctx->native_stage[i].base)
            ctx->native_stage[i].image_id = 0;
        if (!ctx->native_stage[i].image_id && slot == 4) slot = i;
    }
    if (slot == 4) slot = ctx->native_stage_next++ & 3;
    spu_native_require_owner(ctx, base, span);
    ctx->native_stage[slot].base = base;
    ctx->native_stage[slot].span = span;
    ctx->native_stage[slot].entry_pc = pc;
    ctx->native_stage[slot].verified_offset = verified_offset;
    ctx->native_stage[slot].image_id = base == lift ? image : -1;
    ctx->native_stage[slot].retry = 0;
    if (base != lift)
        fprintf(stderr, "[spu-stage] unsupported native load base: image=%d actual=0x%X compiled=0x%X\n",
                image, base, lift);
    if (image_out) *image_out = ctx->native_stage[slot].image_id;
    return 1;
}

static int stage_probe(const uint8_t* ls, uint32_t pc,
                            uint32_t* base_out, int* image_id_out,
                            uint32_t* lift_base_out, uint32_t* span_out,
                            uint32_t* verified_offset)
{
    if (!ls || pc >= SPU_LS_SIZE) return 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < s_stage_probe_count; ++i) {
            const spu_stage_probe* p = &s_stage_probes[i];
            if (pc < p->entry) continue;
            uint32_t base = pc - p->entry;
            uint32_t span = p->seg_vaddr + p->seg_size;
            if ((uint64_t)base + span > SPU_LS_SIZE ||
                (base == p->lift_base) != (pass == 0)) continue;
            if (spu_workload_fingerprint(ls + base + p->seg_vaddr,
                                        p->seg_size) != p->fp) continue;
            if (base_out) *base_out = base;
            if (image_id_out) *image_id_out = p->image_id;
            if (lift_base_out) *lift_base_out = p->lift_base;
            if (span_out) *span_out = span;
            if (verified_offset) *verified_offset = p->seg_vaddr;
            return 1;
        }
    }
    return 0;
}

int spu_workload_stage_probe(const uint8_t* ls, uint32_t pc,
                            uint32_t* base_out, int* image_id_out,
                            uint32_t* lift_base_out, uint32_t* span_out)
{
    return stage_probe(ls, pc, base_out, image_id_out, lift_base_out, span_out, NULL);
}
