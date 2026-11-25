/* Check held controls against the same helpers in the caller environment. */
#include <stdio.h>
#include <string.h>
#include "spu_helpers.h"
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
static unsigned failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"FAIL %s line %d\n",#test,__LINE__); ++failures; } } while (0)
static u128 doubles(uint64_t bits) {
    u128 value;
    for (int lane=0;lane<2;++lane) spu__dw_put(&value,lane,bits);
    return value;
}
static u128 host_op(unsigned op,u128 a,u128 b,u128 c) {
    switch(op) {
    case 0:return spu_dfa(a,b);case 1:return spu_dfs(a,b);case 2:return spu_dfm(a,b);
    case 3:return spu_dfma(a,b,c);case 4:return spu_dfms(a,b,c);
    case 5:return spu_dfnms(a,b,c);case 6:return spu_dfnma(a,b,c);
    case 7:return spu_dfceq(a,b);case 8:return spu_dfcmeq(a,b);
    case 9:return spu_dfcgt(a,b);case 10:return spu_dfcmgt(a,b);
    case 11:return spu_fesd(a);default:return spu_frds(a);
    }
}
int main(void) {
#if SPU_SF_X86_SIMD
    const uint32_t original=_mm_getcsr();
    const uint64_t edges[]={0,0x8000000000000000ull,1,0x8000000000000001ull,
        0x3ff0000000000000ull,0x3ca0000000000001ull,0x7fefffffffffffffull,
        0x7ff0000000000000ull,0x7ff8000000000001ull,0xbff0000000000000ull};
    for(unsigned mode=0;mode<4;++mode) {
        uint32_t caller=0x1f80u|(mode<<13);_mm_setcsr(caller);
        uint64_t outer=spu_sf_simd_enter();
        CHECK(g_spu_sf_simd_scope>0 && spu_sf_csr_is_spu(_mm_getcsr()));
        CHECK(g_spu_sf_host_csr==caller);
        for(unsigned op=0;op<13;++op) for(unsigned i=0;i<10;++i) for(unsigned j=0;j<10;++j) {
            u128 a=doubles(edges[i]),b=doubles(edges[j]),c=doubles(edges[(i+j)%10]);
            uint32_t held=_mm_getcsr();
            u128 inside=host_op(op,a,b,c);
            CHECK(spu_sf_csr_is_spu(_mm_getcsr()));
            _mm_setcsr(caller);u128 outside=host_op(op,a,b,c);_mm_setcsr(held);
            CHECK(!memcmp(&inside,&outside,sizeof inside));
        }
        u128 a,b;for(int lane=0;lane<4;++lane){a._u32[lane]=0x3f800000u;b._u32[lane]=0x33800001u;}
        CHECK(spu_fa(a,b)._u32[0]==0x3f800000u);
        CHECK(spu_sf_csr_is_spu(_mm_getcsr()));
        spu_sf_simd_leave(outer);CHECK(_mm_getcsr()==caller && !g_spu_sf_simd_scope);
    }
    u128 one=doubles(0x3ff0000000000000ull),tiny=doubles(0x3c90000000000000ull);
    _mm_setcsr(0x5f80u);uint64_t seed=spu_sf_simd_enter();spu_sf_simd_leave(seed);
    _mm_setcsr(SPU_SF_CSR_SPU);
    CHECK(spu__dw_get(spu_dfa(one,tiny),0)==0x3ff0000000000000ull);
    uint64_t outer=spu_sf_simd_enter();
    CHECK(spu__dw_get(spu_dfa(one,tiny),0)==0x3ff0000000000000ull);
    spu_sf_simd_leave(outer);
    _mm_setcsr(0x5f80u);outer=spu_sf_simd_enter();_mm_setcsr(0x3f80u);
    uint64_t inner=spu_sf_simd_enter();
    CHECK(spu__dw_get(spu_dfa(one,tiny),0)==0x3ff0000000000001ull);
    spu_sf_simd_leave(inner);spu_sf_simd_leave(outer);
    _mm_setcsr(original);
#else
    fprintf(stderr,"This fixture requires x86 SSE2.\n");return 2;
#endif
    printf("scope failures=%u\n",failures);return failures?1:0;
}
