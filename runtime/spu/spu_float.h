/* SPU single-precision values, authored from SPU ISA v1.2 section 9.
 * Exponent 255 is finite. Denormals flush; results truncate, overflow saturates.
 * Integer significands avoid host NaN/Inf and host rounding-mode state.
 * FPSCR exception accumulation is separate from these pure value helpers. */
#ifndef PS3RECOMP_SPU_FLOAT_H
#define PS3RECOMP_SPU_FLOAT_H
#include <stdint.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

static inline unsigned spu_sf_top(uint64_t m) {
#ifdef _MSC_VER
    unsigned long index;
    uint32_t high=(uint32_t)(m >> 32);
    _BitScanReverse(&index, high ? high : (uint32_t)m);
    return (unsigned)index + (high ? 32u : 0u);
#else
    return 63u-(unsigned)__builtin_clzll(m);
#endif
}

/* Encode sign * m * 2^exponent. Zero has no sign. */
static inline uint32_t spu_sf_pack(uint64_t m, int exponent, uint32_t sign) {
    if (!m) return 0;
    unsigned top=spu_sf_top(m);
    int biased=exponent+(int)top+127;
    if (biased < 1) return 0;
    if (biased > 255) return sign | UINT32_C(0x7fffffff);
    uint32_t significand=(uint32_t)(top>23 ? m >> (top-23) : m << (23-top));
    return sign | (uint32_t)biased << 23 | (significand & UINT32_C(0x7fffff));
}

static inline uint64_t spu_sf_mantissa(uint32_t raw) {
    return (raw & UINT32_C(0x7f800000)) ? (raw & UINT32_C(0x7fffff)) | UINT32_C(0x800000) : 0;
}

/* Right shift with sticky bit; never shift by the word width. */
static inline uint64_t spu_sf_jam(uint64_t m, unsigned shift) {
    if (!shift) return m;
    if (shift>=64) return m!=0;
    return (m >> shift) | ((m << (64-shift))!=0);
}

/* Each operand has at most 48 significant bits. Align leading bits at 60,
 * retaining exact cancellation and guard/sticky bits for one final truncation.
 */
static inline uint32_t spu_sf_sum(uint64_t a,int ae,uint32_t as,
                                  uint64_t b,int be,uint32_t bs) {
    if (!a) return spu_sf_pack(b,be,bs);
    if (!b) return spu_sf_pack(a,ae,as);
    unsigned at=spu_sf_top(a),bt=spu_sf_top(b);
    ae+=(int)at;be+=(int)bt;
    a<<=60-at;b<<=60-bt;
    int exponent=ae>be ? ae : be;
    a=spu_sf_jam(a,(unsigned)(exponent-ae));
    b=spu_sf_jam(b,(unsigned)(exponent-be));
    if (as==bs) return spu_sf_pack(a+b,exponent-60,as);
    return a>=b ? spu_sf_pack(a-b,exponent-60,as) : spu_sf_pack(b-a,exponent-60,bs);
}

static inline uint32_t spu_sf_add(uint32_t a,uint32_t b) {
    return spu_sf_sum(spu_sf_mantissa(a),(int)((a>>23)&255)-150,a & UINT32_C(0x80000000),
                      spu_sf_mantissa(b),(int)((b>>23)&255)-150,b & UINT32_C(0x80000000));
}

static inline uint32_t spu_sf_mul(uint32_t a,uint32_t b) {
    return spu_sf_pack(spu_sf_mantissa(a)*spu_sf_mantissa(b),
                       (int)((a>>23)&255)+(int)((b>>23)&255)-300,
                       (a^b) & UINT32_C(0x80000000));
}

/* Exact, unbounded product until addition (ISA pp.208-212). */
static inline uint32_t spu_sf_madd(uint32_t a,uint32_t b,uint32_t c,uint32_t negate_product) {
    return spu_sf_sum(spu_sf_mantissa(a)*spu_sf_mantissa(b),
                      (int)((a>>23)&255)+(int)((b>>23)&255)-300,
                      (a^b^negate_product) & UINT32_C(0x80000000),
                      spu_sf_mantissa(c),(int)((c>>23)&255)-150,c & UINT32_C(0x80000000));
}

/* FI packed base/step equation (ISA p.219), in exact units of 2^-32. */
static inline uint32_t spu_sf_interpolate(uint32_t a,uint32_t b) {
    uint64_t base=(uint64_t)(UINT32_C(0x800000)+(b & UINT32_C(0x7ffc00))) << 9;
    uint64_t correction=(uint64_t)(b & UINT32_C(0x3ff))*(a & UINT32_C(0x7ffff));
    return spu_sf_pack(base-correction,(int)((b>>23)&255)-159,b & UINT32_C(0x80000000));
}
#endif
