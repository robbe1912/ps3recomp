/* Checked host byte-shuffle implementation. Header-only so standalone lifted
 * callers retain their existing link contract. No process-wide ISA change:
 * a unit built with SSSE3 enabled gets the SSSE3 form as plain inline code,
 * a baseline unit calls it after a CPUID test. */
#ifndef PS3RECOMP_SPU_SHUFFLE_H
#define PS3RECOMP_SPU_SHUFFLE_H
#include "../../include/ps3emu/ps3types.h"
#include <string.h>

#if !defined(SPU_SHUFB_FORCE_SCALAR) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#define SPU_SHUFB_X86 1
#include <tmmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#define SPU_SHUFB_X86 0
#endif

/* SPU_SHUFB_UNIT_SSSE3: the compiler builds this unit with SSSE3 enabled. */
#if SPU_SHUFB_X86 && defined(__SSSE3__)
#define SPU_SHUFB_UNIT_SSSE3 1
#else
#define SPU_SHUFB_UNIT_SSSE3 0
#endif

/* AArch64's existing NEON path is part of the host ABI, not an optional
 * process-wide ISA increase. Preserve it when integrating with newer trees. */
#if !defined(SPU_SHUFB_FORCE_SCALAR) && defined(__aarch64__) && defined(__ARM_NEON)
#define SPU_SHUFB_NEON 1
#include <arm_neon.h>
#else
#define SPU_SHUFB_NEON 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    u128 out;
    for (unsigned h = 0; h < 16; ++h) {
        unsigned selector = c._u8[h];
        unsigned index = (selector & 15) ^ 3; /* SPU byte to host word-value layout */
        if (selector & 128)
            out._u8[h] = (selector & 64) ? ((selector & 32) ? 128 : 255) : 0;
        else
            out._u8[h] = (selector & 16) ? b._u8[index] : a._u8[index];
    }
    return out;
}

static inline int spu_shufb_simd_available(void) {
#if SPU_SHUFB_X86
#ifdef _MSC_VER
    /* Per-thread cache avoids a data race between standalone C callers. SSSE3
     * uses only the SSE register state already supported by the host ABI. */
    static __declspec(thread) int cached;
    if (!cached) {
        int registers[4];
        __cpuid(registers, 1);
        cached = (registers[2] & (1 << 9)) ? 2 : 1;
    }
    return cached == 2;
#else
    return __builtin_cpu_supports("ssse3") != 0;
#endif
#elif SPU_SHUFB_NEON
    return 1;
#else
    return 0;
#endif
}

#if SPU_SHUFB_X86
#if !SPU_SHUFB_UNIT_SSSE3 && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("ssse3")))
#endif
static inline u128 spu_shufb_ssse3(u128 a, u128 b, u128 c) {
    __m128i av, bv, cv;
    memcpy(&av, &a, 16); memcpy(&bv, &b, 16); memcpy(&cv, &c, 16);
    /* Output and control bytes already share host positions. Only the source
     * index needs the within-word reversal; no three extra vector reversals. */
    __m128i index = _mm_and_si128(_mm_xor_si128(cv, _mm_set1_epi8(3)), _mm_set1_epi8(15));
    __m128i from_b = _mm_cmpeq_epi8(_mm_and_si128(cv, _mm_set1_epi8(16)), _mm_set1_epi8(16));
    __m128i value = _mm_or_si128(_mm_andnot_si128(from_b, _mm_shuffle_epi8(av, index)),
                               _mm_and_si128(from_b, _mm_shuffle_epi8(bv, index)));
    __m128i high = _mm_cmpgt_epi8(_mm_setzero_si128(), cv);
    __m128i c0 = _mm_cmpeq_epi8(_mm_and_si128(cv, _mm_set1_epi8((char)0xc0)), _mm_set1_epi8((char)0xc0));
    __m128i e0 = _mm_cmpeq_epi8(_mm_and_si128(cv, _mm_set1_epi8((char)0xe0)), _mm_set1_epi8((char)0xe0));
    __m128i special = _mm_or_si128(_mm_andnot_si128(e0, c0), _mm_and_si128(e0, _mm_set1_epi8((char)0x80)));
    value = _mm_or_si128(_mm_andnot_si128(high, value), special);
    u128 out;
    memcpy(&out, &value, 16);
    return out;
}
#endif

#if SPU_SHUFB_NEON
/* NEON version: tbl over the 32-byte {a,b} concatenation. SPU byte P lives at
 * host byte P^3 (each word's bytes reversed), so rev32 into SPU order,
 * select, and rev32 back. */
static inline u128 spu_shufb_neon(u128 a, u128 b, u128 c) {
    uint8x16x2_t t;
    t.val[0] = vrev32q_u8(vld1q_u8(a._u8));
    t.val[1] = vrev32q_u8(vld1q_u8(b._u8));
    const uint8x16_t C = vrev32q_u8(vld1q_u8(c._u8));
    uint8x16_t r = vqtbl2q_u8(t, vandq_u8(C, vdupq_n_u8(0x1F)));
    /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
    const uint8x16_t m80 = vtstq_u8(C, vdupq_n_u8(0x80));
    const uint8x16_t mC0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xC0)), vdupq_n_u8(0xC0));
    const uint8x16_t mE0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xE0)), vdupq_n_u8(0xE0));
    const uint8x16_t special = vbslq_u8(mE0, vdupq_n_u8(0x80), mC0);
    r = vrev32q_u8(vbslq_u8(m80, special, r));
    u128 out; vst1q_u8(out._u8, r); return out;
}
#endif

static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
#if SPU_SHUFB_UNIT_SSSE3
    return spu_shufb_ssse3(a, b, c);
#elif SPU_SHUFB_X86
    if (spu_shufb_simd_available()) return spu_shufb_ssse3(a, b, c);
#elif SPU_SHUFB_NEON
    return spu_shufb_neon(a, b, c);
#endif
    return spu_shufb_ref(a, b, c);
}

#ifdef __cplusplus
}
#endif
#endif
