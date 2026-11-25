/* Value-only SIMD. A scope holds truncation, FTZ, DAZ and masked traps.
 * Host-arithmetic helpers use the outer caller's controls.
 * The fast forms at the end make the float helpers one x86 operation inside a scope. */
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
/* ---- Fast forms of the float helpers ----------------------------------------------
 * Inside a scope the default path is one x86 operation and no MXCSR access. It
 * applies when all of these hold:
 *  - a scope is active, so the masks are those of the scope;
 *  - a probe of the host mode proves rounding toward zero, FTZ and DAZ. The probe
 *    replaces the MXCSR read of spu_sf_simd_binary;
 *  - no input lane has exponent field 255;
 *  - no result lane reaches 2^127 (the host overflow value lies there too).
 * The operation is followed by "+ (+0)", which makes every zero result +0. The
 * SPU has one zero; the host keeps the sign of a zero sum and of a flushed result.
 * Every other call takes the earlier path, out of line.
 * The probe raises the underflow and inexact flags in every call in the mode
 * of a scope. A scope restores the MXCSR of its caller at leave. In a scope
 * whose MXCSR was changed from outside, the probe raises also the precision or
 * denormal flag; the values stay exact there.
 * The fused family needs FMA3 in the host. It calls spu_sf_fma3 out of line: clang
 * does not inline a target("fma") function into a unit compiled without FMA. */
#if (defined(__clang__) || defined(__GNUC__)) && !defined(SPU_SCALAR_HELPERS) && !(defined(SPU_FLOAT_FAST) && !SPU_FLOAT_FAST)
#define SPU_SF_FAST 1
#else
#define SPU_SF_FAST 0
#endif
#if SPU_SF_FAST
#include <immintrin.h>
#if defined(__clang__)
#include <avxintrin.h>
#include <fmaintrin.h>
#endif
typedef union { uint32_t u[4]; __m128i v; } spu_sf_vec4;
/* The tables of the probe are volatile: the compiler executes the probe for
 * every call and does not move a probe operation out of its branch.
 * Lanes of the input: 2^30, -2^30, 1.5*2^-126, 0. */
static const volatile spu_sf_vec4 spu_sf_probe_input = { { 0x4e800000u, 0xce800000u, 0x00c00000u, 0x00000000u } };
/* Step A adds this. Lanes 0 and 1: half an ulp of 2^30 and more, and the
 * negative of it. The sums stay at the input only with rounding toward zero.
 * Lane 2: -2^-126. The sum is the tiny 2^-127: zero only with FTZ. */
static const volatile spu_sf_vec4 spu_sf_probe_add = { { 0x42800001u, 0xc2800001u, 0x80800000u, 0x00000000u } };
static const volatile spu_sf_vec4 spu_sf_probe_sum = { { 0x4e800000u, 0xce800000u, 0x00000000u, 0x00000000u } };
/* Step B multiplies by this. The product is all +0 only with DAZ. Without DAZ a
 * denormal operand also raises the denormal flag, so step B comes after the
 * branch of step A. Lanes: the smallest denormal, its negative, the smallest
 * denormal, 1. */
static const volatile spu_sf_vec4 spu_sf_probe_mul = { { 0x00000001u, 0x80000001u, 0x00000001u, 0x3f800000u } };
/* 1 when a scope is active and the host mode is the mode of a scope. The compares
 * are integer compares: a float compare of a denormal raises the denormal flag. */
static __attribute__((always_inline)) inline int spu_sf_fast_mode(void) {
    if (!g_spu_sf_simd_scope) return 0;
    const __m128i input = spu_sf_probe_input.v;
    const __m128i sum = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(input), _mm_castsi128_ps(spu_sf_probe_add.v)));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(sum, spu_sf_probe_sum.v)) != 0xffff) return 0;
    const __m128i product = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(input), _mm_castsi128_ps(spu_sf_probe_mul.v)));
    return _mm_movemask_epi8(_mm_cmpeq_epi32(product, _mm_setzero_si128())) == 0xffff;
}
/* The operation, then "+ (+0)". Inline assembly: the compiler cannot fold a call
 * with constant operands, cannot move the operation across an MXCSR write, and
 * takes it as a dependence on the rounding mode it does not model. */
#define SPU_SF_FAST_OP(name, instruction) \
static __attribute__((always_inline)) inline __m128i name(__m128i a, __m128i b) { \
    __asm__ __volatile__(instruction " %[b], %[a]\n\taddps %[z], %[a]" \
                         : [a] "+x"(a) : [b] "x"(b), [z] "x"(_mm_setzero_ps())); \
    return a; \
}
SPU_SF_FAST_OP(spu_sf_fast_add, "addps")
SPU_SF_FAST_OP(spu_sf_fast_sub, "subps")
SPU_SF_FAST_OP(spu_sf_fast_mul, "mulps")
/* The lanes that must take the earlier path: an input with exponent field 255, or
 * a result of 2^127 and above. */
static __attribute__((always_inline)) inline __m128i spu_sf_fast_bad(__m128i value, int is_result) {
    if (is_result)
        return _mm_cmpgt_epi32(_mm_and_si128(value, _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7effffff));
    const __m128i exponent = _mm_set1_epi32(0x7f800000);
    return _mm_cmpeq_epi32(_mm_and_si128(value, exponent), exponent);
}
/* operation: 0 add, 1 subtract, 2 multiply. Returns 1 and stores the result when
 * the fast form applies. */
static __attribute__((always_inline)) inline int spu_sf_fast_binary(uint32_t *out, const uint32_t *a,
                                                                   const uint32_t *b, int operation) {
    if (!spu_sf_fast_mode()) return 0;
    __m128i av = _mm_loadu_si128((const __m128i *)a), bv = _mm_loadu_si128((const __m128i *)b);
    __m128i bad = _mm_or_si128(spu_sf_fast_bad(av, 0), spu_sf_fast_bad(bv, 0));
    av = operation == 2 ? spu_sf_fast_mul(av, bv) : operation == 1 ? spu_sf_fast_sub(av, bv) : spu_sf_fast_add(av, bv);
    bad = _mm_or_si128(bad, spu_sf_fast_bad(av, 1));
    if (_mm_movemask_epi8(bad)) return 0;
    _mm_storeu_si128((__m128i *)out, av);
    return 1;
}
#else
static inline int spu_sf_fast_binary(uint32_t *out, const uint32_t *a, const uint32_t *b, int operation) {
    (void)out; (void)a; (void)b; (void)operation; return 0;
}
static inline int spu_sf_fast_fused(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *c,
                                    uint32_t negate_product, uint32_t negate_c) {
    (void)out; (void)a; (void)b; (void)c; (void)negate_product; (void)negate_c; return 0;
}
#endif
#else
#define SPU_SF_X86_SIMD 0
#define SPU_SF_FAST 0
static inline uint32_t spu_sf_host_begin(void) { return 0; }
static inline void spu_sf_host_end(uint32_t csr) { (void)csr; }
static inline int spu_sf_simd_binary(uint32_t *out,const uint32_t *a,
                                     const uint32_t *b,int operation) {
    (void)out;(void)a;(void)b;(void)operation;return 0;
}
static inline int spu_sf_fast_binary(uint32_t *out, const uint32_t *a, const uint32_t *b, int operation) {
    (void)out; (void)a; (void)b; (void)operation; return 0;
}
static inline int spu_sf_fast_fused(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *c,
                                    uint32_t negate_product, uint32_t negate_c) {
    (void)out; (void)a; (void)b; (void)c; (void)negate_product; (void)negate_c; return 0;
}
#endif
#endif
