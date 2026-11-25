/* Value-only SIMD. A scope holds truncation, FTZ, DAZ and masked traps.
 * Host-arithmetic helpers use the outer caller's controls. */
#ifndef PS3RECOMP_SPU_FLOAT_SIMD_H
#define PS3RECOMP_SPU_FLOAT_SIMD_H
#include <stdint.h>
#if defined(_MSC_VER)
#define SPU_SF_TLS __declspec(thread)
#else
#define SPU_SF_TLS __thread
#endif
#ifdef __cplusplus
extern "C" {
#endif
extern SPU_SF_TLS int g_spu_sf_simd_scope;
extern SPU_SF_TLS uint32_t g_spu_sf_host_csr;
uint64_t spu_sf_simd_enter(void);
/* Save state before setjmp without entering a scope. */
uint64_t spu_sf_simd_mark(void);
void spu_sf_simd_leave(uint64_t saved);
int spu_sf_simd_fused(uint32_t *out,const uint32_t *a,const uint32_t *b,
                      const uint32_t *c,uint32_t negate_product,uint32_t negate_c);
#ifdef __cplusplus
}
#endif
#if defined(__SSE2__) || defined(_M_X64)
#define SPU_SF_X86_SIMD 1
#include <emmintrin.h>
/* The low six bits are sticky exception flags. */
#define SPU_SF_CSR_SPU 0xffc0u
static inline int spu_sf_csr_is_spu(uint32_t csr) {
    return (csr & SPU_SF_CSR_SPU)==SPU_SF_CSR_SPU;
}
static inline uint32_t spu_sf_simd_round_begin(void) {
    uint32_t saved=_mm_getcsr();
    _mm_setcsr(saved|SPU_SF_CSR_SPU);
    return saved;
}
/* Zero means that no switch occurs. A held MXCSR is never zero. */
static inline uint32_t spu_sf_host_begin(void) {
    uint32_t csr=_mm_getcsr();
    if (!spu_sf_csr_is_spu(csr) || !g_spu_sf_simd_scope) return 0;
    _mm_setcsr((g_spu_sf_host_csr & ~0x3fu)|(csr & 0x3fu));
    return csr;
}
static inline void spu_sf_host_end(uint32_t held) {
    if (held) _mm_setcsr(held);
}
static inline int spu_sf_simd_extended(__m128i a) {
    const __m128i exponent = _mm_set1_epi32(0x7f800000);
    return _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(a,exponent),exponent));
}
static inline int spu_sf_simd_store(uint32_t *out,__m128 value) {
    __m128i bits=_mm_castps_si128(value);
    __m128i magnitude=_mm_and_si128(bits,_mm_set1_epi32(0x7fffffff));
    /* Host RTZ overflow yields its largest finite value. Recompute the entire
     * vector near that boundary, including valid SPU exponent-255 results. */
    if (_mm_movemask_epi8(_mm_cmpgt_epi32(magnitude,_mm_set1_epi32(0x7effffff)))) return 0;
    bits=_mm_andnot_si128(_mm_cmpeq_epi32(magnitude,_mm_setzero_si128()),bits);
    _mm_storeu_si128((__m128i *)out,bits);
    return 1;
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("rounding-math")))
#endif
static inline int spu_sf_simd_binary(uint32_t *out,const uint32_t *a,
                                     const uint32_t *b,int operation) {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif
    if (!g_spu_sf_simd_scope) return 0;
    __m128i av=_mm_loadu_si128((const __m128i *)a),bv=_mm_loadu_si128((const __m128i *)b);
    if (spu_sf_simd_extended(av) || spu_sf_simd_extended(bv)) return 0;
    if (operation==1) bv=_mm_xor_si128(bv,_mm_set1_epi32((int)0x80000000u));
    __m128 af=_mm_castsi128_ps(av),bf=_mm_castsi128_ps(bv);
    if (spu_sf_csr_is_spu(_mm_getcsr()))
        return spu_sf_simd_store(out,operation==2 ? _mm_mul_ps(af,bf) : _mm_add_ps(af,bf));
    uint32_t saved=spu_sf_simd_round_begin();
    int result=spu_sf_simd_store(out,operation==2 ? _mm_mul_ps(af,bf) : _mm_add_ps(af,bf));
    _mm_setcsr(saved);
    return result;
}
#else
#define SPU_SF_X86_SIMD 0
static inline uint32_t spu_sf_host_begin(void) { return 0; }
static inline void spu_sf_host_end(uint32_t csr) { (void)csr; }
static inline int spu_sf_simd_binary(uint32_t *out,const uint32_t *a,
                                     const uint32_t *b,int operation) {
    (void)out;(void)a;(void)b;(void)operation;return 0;
}
#endif
#endif
