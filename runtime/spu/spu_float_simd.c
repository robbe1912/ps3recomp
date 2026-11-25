#include "spu_float_simd.h"
SPU_SF_TLS int g_spu_sf_simd_scope;
/* Keep the outer caller's controls for host arithmetic. */
SPU_SF_TLS uint32_t g_spu_sf_host_csr=0x1f80u;
#if SPU_SF_X86_SIMD
#include <immintrin.h>
#ifdef __clang__
#include <avxintrin.h>
#include <fmaintrin.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif
static SPU_SF_TLS int s_capability;
static int capability(void) {
    if (s_capability) return s_capability;
    int fma=0;
#ifdef _MSC_VER
    int regs[4];__cpuid(regs,1);
    if ((regs[2] & ((1<<12)|(1<<27)|(1<<28)))==((1<<12)|(1<<27)|(1<<28)))
        fma=(_xgetbv(0)&6)==6;
#else
    fma=__builtin_cpu_supports("fma")!=0;
#endif
    return s_capability=fma ? 2 : 1;
}
#endif

/* Nested tokens restore both hardware state and the previous enable state.
 * An outer token also restores correctly after an inner drain longjmp.
 * Entry holds SPU mode until leave restores the saved token. */
uint64_t spu_sf_simd_enter(void) {
#if SPU_SF_X86_SIMD
    uint32_t csr=_mm_getcsr();
    uint64_t saved=((uint64_t)(unsigned)g_spu_sf_simd_scope<<32)|csr;
    if (!g_spu_sf_simd_scope) g_spu_sf_host_csr=csr;
    g_spu_sf_simd_scope=capability();
    _mm_setcsr(csr|SPU_SF_CSR_SPU);
    return saved;
#else
    return 0;
#endif
}
uint64_t spu_sf_simd_mark(void) {
#if SPU_SF_X86_SIMD
    return ((uint64_t)(unsigned)g_spu_sf_simd_scope<<32)|_mm_getcsr();
#else
    return 0;
#endif
}
void spu_sf_simd_leave(uint64_t saved) {
#if SPU_SF_X86_SIMD
    g_spu_sf_simd_scope=(int)(saved>>32);
    _mm_setcsr((uint32_t)saved);
#else
    (void)saved;
#endif
}

#if SPU_SF_X86_SIMD
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("fma")))
#endif
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("rounding-math")))
#endif
static int fused_host(uint32_t *out,const uint32_t *a,const uint32_t *b,
                       const uint32_t *c,uint32_t negate_product,uint32_t negate_c) {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif
    __m128i av=_mm_loadu_si128((const __m128i *)a),bv=_mm_loadu_si128((const __m128i *)b);
    __m128i cv=_mm_loadu_si128((const __m128i *)c);
    if (spu_sf_simd_extended(av) || spu_sf_simd_extended(bv) || spu_sf_simd_extended(cv)) return 0;
    av=_mm_xor_si128(av,_mm_set1_epi32((int)negate_product));
    cv=_mm_xor_si128(cv,_mm_set1_epi32((int)negate_c));
    if (spu_sf_csr_is_spu(_mm_getcsr()))
        return spu_sf_simd_store(out,_mm_fmadd_ps(_mm_castsi128_ps(av),_mm_castsi128_ps(bv),_mm_castsi128_ps(cv)));
    uint32_t saved=spu_sf_simd_round_begin();
    int result=spu_sf_simd_store(out,_mm_fmadd_ps(_mm_castsi128_ps(av),_mm_castsi128_ps(bv),_mm_castsi128_ps(cv)));
    _mm_setcsr(saved);
    return result;
}
#endif
int spu_sf_simd_fused(uint32_t *out,const uint32_t *a,const uint32_t *b,
                      const uint32_t *c,uint32_t negate_product,uint32_t negate_c) {
#if SPU_SF_X86_SIMD
    if (g_spu_sf_simd_scope==2) return fused_host(out,a,b,c,negate_product,negate_c);
#else
    (void)out;(void)a;(void)b;(void)c;(void)negate_product;(void)negate_c;
#endif
    return 0;
}
