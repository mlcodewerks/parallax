/* Bit-exact channel SIMD for the RDP pixel pipeline. GPL-2.0-or-later.
 * Lanes represent RGBA, never independent pixels: framebuffer feedback,
 * coverage, random state and pixel ordering remain serial. */
#ifndef AL_PIPELINE_SIMD_H
#define AL_PIPELINE_SIMD_H
#include "../simd.h"
#if defined(AL_SIMD_SSE42)
static inline __m128i al_ext9(__m128i v)
{
    __m128i sign=_mm_cmpeq_epi32(_mm_and_si128(v,_mm_set1_epi32(0x180)),_mm_set1_epi32(0x180));
    return _mm_or_si128(_mm_and_si128(v,_mm_set1_epi32(511)),_mm_and_si128(sign,_mm_set1_epi32(~511)));
}
static inline __m128i al_clamp9(__m128i v)
{
    v=_mm_and_si128(v,_mm_set1_epi32(511));
    /* 0..255 -> unchanged, 256..383 -> 255, 384..511 -> zero. */
    __m128i low=_mm_min_epi32(v,_mm_set1_epi32(255));
    return _mm_andnot_si128(_mm_cmpgt_epi32(v,_mm_set1_epi32(383)),low);
}
static inline __m128i al_combine4(__m128i a,__m128i b,__m128i c,__m128i d)
{
    a=al_ext9(a); b=al_ext9(b); d=al_ext9(d);
    c=_mm_srai_epi32(_mm_slli_epi32(c,23),23);
    __m128i v=_mm_add_epi32(_mm_mullo_epi32(_mm_sub_epi32(a,b),c),_mm_slli_epi32(d,8));
    v=_mm_and_si128(_mm_add_epi32(v,_mm_set1_epi32(128)),_mm_set1_epi32(0x1ffff));
    return _mm_blend_epi16(v,_mm_srli_epi32(v,8),0xc0);
}
static inline __m128i al_blend4(__m128i a,__m128i b,int factor_a,int factor_b)
{
    return _mm_add_epi32(_mm_mullo_epi32(a,_mm_set1_epi32(factor_a)),
                         _mm_mullo_epi32(b,_mm_set1_epi32(factor_b)));
}
static inline __m128i al_dither4(__m128i rgb,int mode,int dith)
{
    __m128i threshold=mode==2?_mm_setr_epi32(dith&7,(dith>>3)&7,(dith>>6)&7,0):_mm_set1_epi32(dith);
    __m128i rounded=_mm_add_epi32(_mm_and_si128(rgb,_mm_set1_epi32(0xf8)),_mm_set1_epi32(8));
    /* Signed comparison, including the exact scalar behavior outside 0..255. */
    rounded=_mm_blendv_epi8(rounded,_mm_set1_epi32(255),_mm_cmpgt_epi32(rgb,_mm_set1_epi32(247)));
    return _mm_blendv_epi8(rgb,rounded,_mm_cmpgt_epi32(_mm_and_si128(rgb,_mm_set1_epi32(7)),threshold));
}
static inline __m128i al_shade4(__m128i rgba,__m128i dx,__m128i dy,int x,int y,unsigned cvg)
{
    if(cvg==8) rgba=_mm_srai_epi32(rgba,2);
    else {
        __m128i correction=_mm_add_epi32(_mm_mullo_epi32(dx,_mm_set1_epi32(x)),_mm_mullo_epi32(dy,_mm_set1_epi32(y)));
        rgba=_mm_srai_epi32(_mm_add_epi32(_mm_slli_epi32(rgba,2),correction),4);
    }
    return al_clamp9(rgba);
}
static inline __m128i al_lerp4(__m128i t0,__m128i t1,__m128i t2,__m128i t3,
    int srg,int s,int t,int upperrg,int upper,int centerrg,int center)
{
    __m128i mask=_mm_setr_epi32(upperrg?-1:0,upperrg?-1:0,upper?-1:0,upper?-1:0);
    __m128i base=_mm_blendv_epi8(t0,t3,mask),a=_mm_blendv_epi8(t1,t2,mask),b=_mm_blendv_epi8(t2,t1,mask);
    __m128i sf=_mm_setr_epi32(srg,srg,s,s),tf=_mm_set1_epi32(t);
    sf=_mm_blendv_epi8(sf,_mm_sub_epi32(_mm_set1_epi32(32),sf),mask);
    tf=_mm_blendv_epi8(tf,_mm_sub_epi32(_mm_set1_epi32(32),tf),mask);
    __m128i result=_mm_add_epi32(base,_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(
        _mm_mullo_epi32(_mm_sub_epi32(a,base),sf),_mm_mullo_epi32(_mm_sub_epi32(b,base),tf)),_mm_set1_epi32(16)),5));
    if(centerrg || center) {
        __m128i sum=_mm_sub_epi32(_mm_add_epi32(_mm_add_epi32(t0,t1),t2),_mm_mullo_epi32(t3,_mm_set1_epi32(3)));
        __m128i avg=_mm_add_epi32(t3,_mm_srai_epi32(_mm_add_epi32(sum,_mm_set1_epi32(2)),2));
        result=_mm_blendv_epi8(result,avg,_mm_setr_epi32(centerrg?-1:0,centerrg?-1:0,center?-1:0,center?-1:0));
    }
    return result;
}
static inline __m128i al_tcclamp4(__m128i v)
{
    __m128i kind=_mm_and_si128(v,_mm_set1_epi32(0x18000));
    __m128i lo=_mm_or_si128(_mm_cmpeq_epi32(kind,_mm_set1_epi32(0x10000)),
                           _mm_cmpeq_epi32(_mm_and_si128(v,_mm_set1_epi32(0x20000)),_mm_set1_epi32(0x20000)));
    __m128i hi=_mm_or_si128(_mm_cmpeq_epi32(kind,_mm_set1_epi32(0x8000)),
                           _mm_cmpeq_epi32(_mm_and_si128(v,_mm_set1_epi32(0x40000)),_mm_set1_epi32(0x40000)));
    __m128i result=_mm_blendv_epi8(_mm_and_si128(v,_mm_set1_epi32(0xffff)),_mm_set1_epi32(0x8000),lo);
    /* Overflow bit has priority over underflow; underflow beats kind=0x8000. */
    hi=_mm_andnot_si128(lo,hi);
    hi=_mm_or_si128(hi,_mm_cmpeq_epi32(_mm_and_si128(v,_mm_set1_epi32(0x40000)),_mm_set1_epi32(0x40000)));
    return _mm_blendv_epi8(result,_mm_set1_epi32(0x7fff),hi);
}
static inline int al_key3(__m128i color,__m128i width)
{
    color=_mm_srai_epi32(_mm_slli_epi32(color,15),15);
    __m128i tie=_mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(color,_mm_set1_epi32(15)),_mm_set1_epi32(8)),_mm_set1_epi32(16));
    __m128i negative=_mm_add_epi32(_mm_sub_epi32(_mm_setzero_si128(),color),tie);
    color=_mm_blendv_epi8(color,negative,_mm_cmpgt_epi32(color,_mm_setzero_si128()));
    color=_mm_add_epi32(color,_mm_slli_epi32(width,4));
    color=_mm_min_epi32(color,_mm_shuffle_epi32(color,0x55));
    /* Use the original blue lane; the first reduction does not change the
     * minimum across RGB, so reducing its lane 2 is also exact. */
    color=_mm_min_epi32(color,_mm_shuffle_epi32(color,0xaa));
    color=_mm_max_epi32(color,_mm_setzero_si128());
    return _mm_cvtsi128_si32(_mm_min_epi32(color,_mm_set1_epi32(255)));
}
#endif
static inline void al_coverage_mask(unsigned char* buffer,int start,int end,unsigned mask)
{
    if(end<start) return;
    unsigned char* p=buffer+start; int count=end-start+1;
#if defined(AL_SIMD_SSE2)
    __m128i bits=_mm_set1_epi8((char)mask);
    while(count>=16) { _mm_storeu_si128((__m128i*)p,_mm_and_si128(_mm_loadu_si128((const __m128i*)p),bits)); p+=16; count-=16; }
#endif
    while(count--) *p++ &= mask;
}
#endif
