/* Shared integer HLE kernels. SSE4.2 on x86; the existing sse2neon adapter
 * lowers the same operations to NEON. Scalar code remains the reference on
 * other targets and in HLE_SIMD_DISABLE validation builds. No float math. */
#ifndef PARALLAX_HLE_SIMD_H
#define PARALLAX_HLE_SIMD_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if !defined(HLE_SIMD_DISABLE)
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include "sse2neon.h"
#define HLE_SIMD_ENABLED 1
#define HLE_SIMD_BACKEND "NEON"
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define HLE_SIMD_ENABLED 1
#define HLE_SIMD_BACKEND "SSE4.2"
#endif
#endif
#ifndef HLE_SIMD_ENABLED
#define HLE_SIMD_ENABLED 0
#define HLE_SIMD_BACKEND "scalar"
#endif

static inline int16_t hle_sat16(int64_t x)
{ return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x; }

enum { HLE_VMUDL, HLE_VMUDM, HLE_VMADM, HLE_VMADN, HLE_VMADH,
       HLE_VMULF, HLE_VMACF, HLE_VMADL, HLE_VMUDN, HLE_VMUDH };

#if HLE_SIMD_ENABLED
/* SSE has no arithmetic 64-bit shift until AVX-512. These exact lane
 * operations also compile to native signed shifts through sse2neon. */
#define hle_sra64(a, n) _mm_or_si128(_mm_srli_epi64((a), (n)), \
    _mm_slli_epi64(_mm_cmpgt_epi64(_mm_setzero_si128(), (a)), 64-(n)))
static inline __m128i hle_select(__m128i mask, __m128i yes, __m128i no)
{ return _mm_blendv_epi8(no, yes, mask); }
static inline __m128i hle_clamp64_s16(__m128i a)
{
    __m128i hi = _mm_set1_epi64x(32767), lo = _mm_set1_epi64x(-32768);
    a = hle_select(_mm_cmpgt_epi64(a, hi), hi, a);
    return hle_select(_mm_cmpgt_epi64(lo, a), lo, a);
}
static inline __m128i hle_rsp_low64(__m128i a)
{
    __m128i hi = hle_sra64(a, 16), z = _mm_setzero_si128();
    __m128i out = _mm_and_si128(a, _mm_set1_epi64x(65535));
    out = hle_select(_mm_cmpgt_epi64(hi, _mm_set1_epi64x(32767)),
                     _mm_set1_epi64x(65535), out);
    return hle_select(_mm_cmpgt_epi64(_mm_set1_epi64x(-32768), hi), z, out);
}
static inline __m128i hle_join64_low32(__m128i a, __m128i b)
{
    a = _mm_shuffle_epi32(a, _MM_SHUFFLE(2,0,2,0));
    b = _mm_shuffle_epi32(b, _MM_SHUFFLE(2,0,2,0));
    return _mm_unpacklo_epi64(a, b);
}
static inline __m128i hle_mul_s16_lo(__m128i a, __m128i b)
{ return _mm_mullo_epi32(_mm_cvtepi16_epi32(a), _mm_cvtepi16_epi32(b)); }
static inline __m128i hle_vmulf16(__m128i a, __m128i b)
{
    __m128i lo = hle_mul_s16_lo(a,b);
    __m128i hi = hle_mul_s16_lo(_mm_srli_si128(a,8),_mm_srli_si128(b,8));
    lo = _mm_srai_epi32(_mm_add_epi32(lo,_mm_set1_epi32(0x4000)),15);
    hi = _mm_srai_epi32(_mm_add_epi32(hi,_mm_set1_epi32(0x4000)),15);
    return _mm_packs_epi32(lo,hi);
}
#endif

/* RSP element selection snapshots all operands before a destination write. */
static inline void hle_rsp_select8(int16_t *dst,const int16_t *src,unsigned e)
{
#if HLE_SIMD_ENABLED
    static const uint8_t indices[16][16]={
        {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
        {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
        {0,1,0,1,4,5,4,5,8,9,8,9,12,13,12,13},
        {2,3,2,3,6,7,6,7,10,11,10,11,14,15,14,15},
        {0,1,0,1,0,1,0,1,8,9,8,9,8,9,8,9},
        {2,3,2,3,2,3,2,3,10,11,10,11,10,11,10,11},
        {4,5,4,5,4,5,4,5,12,13,12,13,12,13,12,13},
        {6,7,6,7,6,7,6,7,14,15,14,15,14,15,14,15},
        {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
        {2,3,2,3,2,3,2,3,2,3,2,3,2,3,2,3},
        {4,5,4,5,4,5,4,5,4,5,4,5,4,5,4,5},
        {6,7,6,7,6,7,6,7,6,7,6,7,6,7,6,7},
        {8,9,8,9,8,9,8,9,8,9,8,9,8,9,8,9},
        {10,11,10,11,10,11,10,11,10,11,10,11,10,11,10,11},
        {12,13,12,13,12,13,12,13,12,13,12,13,12,13,12,13},
        {14,15,14,15,14,15,14,15,14,15,14,15,14,15,14,15},
    };
    _mm_storeu_si128((__m128i *)dst,_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)src),_mm_loadu_si128((const __m128i *)indices[e&15])));
#else
    int16_t result[8];unsigned i;e&=15;
    for(i=0;i<8;i++)result[i]=src[e<2?i:e<4?((i&~1)|(e&1)):e<8?((i&~3)|(e&3)):e&7];
    memcpy(dst,result,sizeof(result));
#endif
}

/* Eight independent vector accumulator lanes, including 48-bit wrap and
 * the different ACC_L / ACC_M clamps. Callers retain their load/store order. */
static inline void hle_acc8(int16_t *dst, int64_t *acc, const int16_t *vs,
                            const int16_t *vt, int e, int op)
{
#if HLE_SIMD_ENABLED
    __m128i x = _mm_loadu_si128((const __m128i *)vs);
    __m128i y = e < 0 ? _mm_loadu_si128((const __m128i *)vt) : _mm_set1_epi16(vt[e]);
    __m128i output[2];
    int half, pair;
    if(op==HLE_VMULF) {
        __m128i p[2]={hle_mul_s16_lo(x,y),hle_mul_s16_lo(_mm_srli_si128(x,8),_mm_srli_si128(y,8))};
        for(half=0;half<2;half++) {
            __m128i v=p[half];
            output[half]=_mm_srai_epi32(_mm_add_epi32(v,_mm_set1_epi32(0x4000)),15);
            for(pair=0;pair<2;pair++) {
                __m128i a=_mm_add_epi64(_mm_slli_epi64(_mm_cvtepi32_epi64(v),1),_mm_set1_epi64x(0x8000));
                _mm_storeu_si128((__m128i *)(acc+4*half+2*pair),a);v=_mm_srli_si128(v,8);
            }
        }
        _mm_storeu_si128((__m128i *)dst,_mm_packs_epi32(output[0],output[1]));
        return;
    }
    for (half=0;half<2;half++) {
        __m128i a = (op==HLE_VMUDL || op==HLE_VMADL || op==HLE_VMADN || op==HLE_VMUDN) ? _mm_cvtepu16_epi32(x) : _mm_cvtepi16_epi32(x);
        __m128i b = (op==HLE_VMUDL || op==HLE_VMADL || op==HLE_VMUDM || op==HLE_VMADM) ? _mm_cvtepu16_epi32(y) : _mm_cvtepi16_epi32(y);
        __m128i p = _mm_mullo_epi32(a,b), out[2], low[2];
        if (op==HLE_VMUDL || op==HLE_VMADL) p = _mm_srli_epi32(p,16);
        for (pair=0;pair<2;pair++) {
            unsigned at = (unsigned)(half*4+pair*2);
            __m128i v = _mm_cvtepi32_epi64(p);
            if (op==HLE_VMADH || op==HLE_VMUDH) v = _mm_slli_epi64(v,16);
            if (op==HLE_VMULF || op==HLE_VMACF) v = _mm_slli_epi64(v,1);
            if (op==HLE_VMULF) v = _mm_add_epi64(v,_mm_set1_epi64x(0x8000));
            if (op!=HLE_VMUDL && op!=HLE_VMUDM && op!=HLE_VMULF && op!=HLE_VMUDN && op!=HLE_VMUDH)
                v = _mm_add_epi64(v,_mm_loadu_si128((const __m128i *)(acc+at)));
            v = hle_sra64(_mm_slli_epi64(v,16),16);
            _mm_storeu_si128((__m128i *)(acc+at),v);
            /* Wrapped ACC[47:16] fits signed 32 bits. Defer its signed
             * saturation to PACKSSDW instead of four 64-bit clamps. */
            out[pair] = _mm_srli_epi64(v,16);low[pair]=v;
            p = _mm_srli_si128(p,8);
        }
        output[half] = hle_join64_low32(out[0],out[1]);
        if(op==HLE_VMUDL || op==HLE_VMADN || op==HLE_VMADL || op==HLE_VMUDN) {
            __m128i bits=hle_join64_low32(low[0],low[1]);
            if(op!=HLE_VMUDL) {
                bits=hle_select(_mm_cmpgt_epi32(output[half],_mm_set1_epi32(32767)),_mm_set1_epi32(65535),bits);
                bits=_mm_andnot_si128(_mm_cmpgt_epi32(_mm_set1_epi32(-32768),output[half]),bits);
            }
            /* ACC_L reads are signed bit patterns, not signed saturation. */
            output[half]=_mm_srai_epi32(_mm_slli_epi32(bits,16),16);
        }
        x = _mm_srli_si128(x,8); y = _mm_srli_si128(y,8);
        if(e>=0) y = _mm_set1_epi16(vt[e]);
    }
    _mm_storeu_si128((__m128i *)dst,_mm_packs_epi32(output[0],output[1]));
#else
    unsigned i;
    for(i=0;i<8;i++) {
        int32_t x=vs[i],y=vt[e<0?(int)i:e];int64_t v;
        if(op==HLE_VMUDL || op==HLE_VMADL) v=((uint32_t)(uint16_t)x*(uint16_t)y)>>16;
        else if(op==HLE_VMUDM || op==HLE_VMADM) v=(int64_t)x*(uint16_t)y;
        else if(op==HLE_VMADN || op==HLE_VMUDN) v=(int64_t)(uint16_t)x*y;
        else v=(int64_t)x*y;
        if(op==HLE_VMADH || op==HLE_VMUDH) v*=65536;
        if(op==HLE_VMULF || op==HLE_VMACF) v*=2;
        if(op==HLE_VMULF) v+=0x8000;
        if(op!=HLE_VMUDL && op!=HLE_VMUDM && op!=HLE_VMULF && op!=HLE_VMUDN && op!=HLE_VMUDH) v+=acc[i];
        acc[i]=(int64_t)((uint64_t)v<<16)>>16;
        if(op==HLE_VMADN || op==HLE_VMADL || op==HLE_VMUDN) dst[i]=(v=acc[i]>>16)>32767?(int16_t)0xffff:v< -32768?0:(int16_t)acc[i];
        else if(op==HLE_VMUDL) dst[i]=(int16_t)acc[i];
        else if(op==HLE_VMUDM) dst[i]=(int16_t)(acc[i]>>16);
        else dst[i]=hle_sat16((int32_t)(acc[i]>>16));
    }
#endif
}

static inline int64_t hle_dot_s16(const int16_t *x,const int16_t *y,size_t n,int reverse)
{
    int64_t sum=0;size_t i=0;
#if HLE_SIMD_ENABLED
    __m128i acc=_mm_setzero_si128();
    for(;i+8<=n;i+=8) {
        __m128i a=_mm_loadu_si128((const __m128i *)(x+i));
        __m128i b=_mm_loadu_si128((const __m128i *)(y+(reverse?n-i-8:i)));
        __m128i p,q;
        if(reverse) b=_mm_shuffle_epi8(b,_mm_setr_epi8(14,15,12,13,10,11,8,9,6,7,4,5,2,3,0,1));
        p=hle_mul_s16_lo(a,b);q=hle_mul_s16_lo(_mm_srli_si128(a,8),_mm_srli_si128(b,8));
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(p));
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(_mm_srli_si128(p,8)));
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(q));
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(_mm_srli_si128(q,8)));
    }
    if(i+4<=n) {
        __m128i a=_mm_loadl_epi64((const __m128i *)(x+i));
        __m128i b=_mm_loadl_epi64((const __m128i *)(y+(reverse?n-i-4:i))),p;
        if(reverse)b=_mm_shufflelo_epi16(b,_MM_SHUFFLE(0,1,2,3));
        p=hle_mul_s16_lo(a,b);
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(p));
        acc=_mm_add_epi64(acc,_mm_cvtepi32_epi64(_mm_srli_si128(p,8)));i+=4;
    }
    { int64_t a[2];_mm_storeu_si128((__m128i *)a,acc);sum=a[0]+a[1]; }
#endif
    for(;i<n;i++) sum+=(int64_t)x[i]*y[reverse?n-1-i:i];
    return sum;
}

/* Mixer forms differ: RSP uses a 0x7fff gain on the destination before
 * adding the source product; MusyX preserves the destination exactly. */
static inline void hle_mix8(int16_t *out,const int16_t *dst,const int16_t *src,int16_t gain,int rsp)
{
#if HLE_SIMD_ENABLED
    __m128i d=_mm_loadu_si128((const __m128i *)dst),s=_mm_loadu_si128((const __m128i *)src),lo,hi;
    if(rsp) {
        __m128i g=_mm_set1_epi32(((uint32_t)(uint16_t)gain<<16)|32767u);
        lo=_mm_madd_epi16(_mm_unpacklo_epi16(d,s),g);
        hi=_mm_madd_epi16(_mm_unpackhi_epi16(d,s),g);
        lo=_mm_srai_epi32(_mm_add_epi32(lo,_mm_set1_epi32(0x4000)),15);
        hi=_mm_srai_epi32(_mm_add_epi32(hi,_mm_set1_epi32(0x4000)),15);
    } else {
        __m128i g=_mm_set1_epi16(gain);
        lo=hle_mul_s16_lo(s,g);hi=hle_mul_s16_lo(_mm_srli_si128(s,8),g);
        lo=_mm_add_epi32(_mm_cvtepi16_epi32(d),_mm_srai_epi32(_mm_add_epi32(lo,_mm_set1_epi32(0x4000)),15));
        hi=_mm_add_epi32(_mm_cvtepi16_epi32(_mm_srli_si128(d,8)),_mm_srai_epi32(_mm_add_epi32(hi,_mm_set1_epi32(0x4000)),15));
    }
    _mm_storeu_si128((__m128i *)out,_mm_packs_epi32(lo,hi));
#else
    unsigned i;for(i=0;i<8;i++) out[i]=hle_sat16(rsp?
        ((0x8000+2*(int64_t)dst[i]*32767+2*(int64_t)src[i]*gain)>>16):
        (dst[i]+(((int32_t)src[i]*gain+0x4000)>>15)));
#endif
}

static inline void hle_adpcm8(int16_t *dst,const int16_t *src,const int16_t *book,const int16_t *last)
{
#if HLE_SIMD_ENABLED
    __m128i a=_mm_loadu_si128((const __m128i *)book),b=_mm_loadu_si128((const __m128i *)(book+8));
    __m128i s=_mm_loadu_si128((const __m128i *)src),g1=_mm_set1_epi16(last[0]),g2=_mm_set1_epi16(last[1]);
    __m128i lo=_mm_add_epi32(hle_mul_s16_lo(a,g1),hle_mul_s16_lo(b,g2));
    __m128i hi=_mm_add_epi32(hle_mul_s16_lo(_mm_srli_si128(a,8),g1),hle_mul_s16_lo(_mm_srli_si128(b,8),g2));
    lo=_mm_add_epi32(lo,_mm_slli_epi32(_mm_cvtepi16_epi32(s),11));
    hi=_mm_add_epi32(hi,_mm_slli_epi32(_mm_cvtepi16_epi32(_mm_srli_si128(s,8)),11));
#define HLE_ADPCM_TAP(j) do { \
    __m128i c=_mm_slli_si128(b,2*((j)+1)),g=_mm_set1_epi16(src[j]); \
    lo=_mm_add_epi32(lo,hle_mul_s16_lo(c,g)); \
    hi=_mm_add_epi32(hi,hle_mul_s16_lo(_mm_srli_si128(c,8),g)); \
} while(0)
    HLE_ADPCM_TAP(0);HLE_ADPCM_TAP(1);HLE_ADPCM_TAP(2);HLE_ADPCM_TAP(3);
    HLE_ADPCM_TAP(4);HLE_ADPCM_TAP(5);HLE_ADPCM_TAP(6);
#undef HLE_ADPCM_TAP
    _mm_storeu_si128((__m128i *)dst,_mm_packs_epi32(_mm_srai_epi32(lo,11),_mm_srai_epi32(hi,11)));
#else
    int16_t l1=last[0],l2=last[1];
    unsigned i,j;for(i=0;i<8;i++) {
        int64_t a=(int64_t)src[i]*2048+(int64_t)book[i]*l1+(int64_t)book[8+i]*l2;
        for(j=0;j<i;j++)a+=(int64_t)book[8+j]*src[i-1-j];
        dst[i]=hle_sat16((int32_t)(uint32_t)a>>11);
    }
#endif
}

static inline void hle_addsub8(int16_t *dst,const int16_t *a,const int16_t *b,int sub)
{
#if HLE_SIMD_ENABLED
    __m128i x=_mm_loadu_si128((const __m128i *)a),y=_mm_loadu_si128((const __m128i *)b);
    _mm_storeu_si128((__m128i *)dst,sub?_mm_subs_epi16(x,y):_mm_adds_epi16(x,y));
#else
    unsigned i;for(i=0;i<8;i++)dst[i]=hle_sat16((int32_t)a[i]+(sub?-(int32_t)b[i]:b[i]));
#endif
}

/* Saturating signed product, followed by the caller's 16-bit bit shift. */
static inline void hle_scale8(int16_t *dst,const int16_t *src,const int16_t *gain,int broadcast,unsigned shift)
{
#if HLE_SIMD_ENABLED
    __m128i a=_mm_loadu_si128((const __m128i *)src);
    __m128i b=broadcast?_mm_set1_epi16(*gain):_mm_loadu_si128((const __m128i *)gain);
    __m128i lo=hle_mul_s16_lo(a,b),hi=hle_mul_s16_lo(_mm_srli_si128(a,8),_mm_srli_si128(b,8));
    _mm_storeu_si128((__m128i *)dst,_mm_sll_epi16(_mm_packs_epi32(lo,hi),_mm_cvtsi32_si128(shift)));
#else
    unsigned i;for(i=0;i<8;i++)dst[i]=(uint16_t)hle_sat16((int32_t)src[i]*gain[broadcast?0:i])<<shift;
#endif
}
static inline void hle_transpose8(int16_t *dst,const int16_t *src)
{
#if HLE_SIMD_ENABLED
    __m128i a[8],b[8],c[8];unsigned i;
    for(i=0;i<8;i++)a[i]=_mm_loadu_si128((const __m128i *)(src+8*i));
    for(i=0;i<4;i++){b[2*i]=_mm_unpacklo_epi16(a[2*i],a[2*i+1]);b[2*i+1]=_mm_unpackhi_epi16(a[2*i],a[2*i+1]);}
    for(i=0;i<2;i++){
        c[4*i]=_mm_unpacklo_epi32(b[4*i],b[4*i+2]);c[4*i+1]=_mm_unpackhi_epi32(b[4*i],b[4*i+2]);
        c[4*i+2]=_mm_unpacklo_epi32(b[4*i+1],b[4*i+3]);c[4*i+3]=_mm_unpackhi_epi32(b[4*i+1],b[4*i+3]);
    }
    for(i=0;i<4;i++){
        _mm_storeu_si128((__m128i *)(dst+16*i),_mm_unpacklo_epi64(c[i],c[i+4]));
        _mm_storeu_si128((__m128i *)(dst+16*i+8),_mm_unpackhi_epi64(c[i],c[i+4]));
    }
#else
    unsigned i,j;int16_t a[64];for(i=0;i<8;i++)for(j=0;j<8;j++)a[8*i+j]=src[8*j+i];memcpy(dst,a,sizeof(a));
#endif
}

/* Exact SDK matrix product: truncate each unsigned fractional product
 * before summing the signed cross terms in 64-bit lanes. */
static inline void hle_matrix_rows(int32_t *dst,const int32_t *a,const int32_t *b,unsigned rows,int partial,int wrap48)
{
    int32_t result[16];unsigned i,j,k;
#if HLE_SIMD_ENABLED
    for(i=0;i<rows;i++) {
        __m128i acc[2]={_mm_setzero_si128(),_mm_setzero_si128()};
        __m128i fraction[2];
        for(k=0;k<4;k++) {
            __m128i v=_mm_loadu_si128((const __m128i *)(b+4*k));
            __m128i scalar=_mm_set1_epi32(a[4*i+k]);
            for(j=0;j<2;j++) {
                __m128i row=_mm_shuffle_epi32(v,_MM_SHUFFLE(1,1,0,0));
                /* Shifting EACH full product is exactly the truncated
                 * frac*frac + signed cross-term chain, without delaying
                 * its truncation until after the dot-product sum. */
                __m128i x=hle_sra64(_mm_mul_epi32(row,scalar),16);
                acc[j]=_mm_add_epi64(acc[j],x);
                if(k==3 && partial) {
                    __m128i ii=_mm_mul_epi32(_mm_srai_epi32(row,16),_mm_set1_epi32(a[4*i+k]>>16));
                    fraction[j]=_mm_sub_epi64(acc[j],_mm_slli_epi64(ii,16));
                }
                v=_mm_srli_si128(v,8);
            }
        }
        for(j=0;j<2;j++) {
            __m128i hi,v;
            if(wrap48)acc[j]=hle_sra64(_mm_slli_epi64(acc[j],16),16);
            if(!partial)fraction[j]=acc[j];
            else if(wrap48)fraction[j]=hle_sra64(_mm_slli_epi64(fraction[j],16),16);
            hi=hle_clamp64_s16(hle_sra64(acc[j],16));
            v=_mm_or_si128(_mm_slli_epi64(hi,16),hle_rsp_low64(fraction[j]));
            acc[j]=v;
        }
        _mm_storeu_si128((__m128i *)(result+4*i),hle_join64_low32(acc[0],acc[1]));
    }
#else
    for(i=0;i<rows;i++)for(j=0;j<4;j++) {
        int64_t acc=0,hi,fraction=0,low;
        for(k=0;k<4;k++){
            int32_t ai=a[4*i+k]>>16,af=a[4*i+k]&65535,bi=b[4*k+j]>>16,bf=b[4*k+j]&65535;
            acc+=((uint32_t)af*(uint32_t)bf)>>16;
            acc+=(int64_t)ai*bf+(int64_t)af*bi;
            if(k==3)fraction=acc;
            acc+=(int64_t)ai*bi*65536;
        }
        if(wrap48)acc=(int64_t)((uint64_t)acc<<16)>>16;
        if(!partial)fraction=acc;
        else if(wrap48)fraction=(int64_t)((uint64_t)fraction<<16)>>16;
        hi=acc>>16;
        low=(fraction>>16)< -32768?0:(fraction>>16)>32767?65535:(fraction&65535);
        result[4*i+j]=(int32_t)(((uint32_t)(uint16_t)hle_sat16(hi)<<16)|(uint32_t)low);
    }
#endif
    memcpy(dst,result,rows*4*sizeof(*result));
}
static inline void hle_matrix4(int32_t *dst,const int32_t *a,const int32_t *b)
{hle_matrix_rows(dst,a,b,4,0,0);}

/* Four independent clip components; readback/clipping remains with the
 * microcode-specific caller, including its last-integer-term latch. */
static inline void hle_transform4(int64_t *dst,const int32_t *m,int32_t x,int32_t y,int32_t z)
{
#if HLE_SIMD_ENABLED
    __m128i v[4];unsigned i;int32_t xyz[3]={x,y,z};
    for(i=0;i<4;i++)v[i]=_mm_loadu_si128((const __m128i *)(m+4*i));
    for(i=0;i<2;i++) {
        __m128i acc=_mm_cvtepi32_epi64(v[3]);unsigned k;
        for(k=0;k<3;k++) {
            /* Signed 32x32 -> 64 takes the even lanes. */
            __m128i row=_mm_shuffle_epi32(v[k],_MM_SHUFFLE(1,1,0,0));
            acc=_mm_add_epi64(acc,_mm_mul_epi32(row,_mm_set1_epi32(xyz[k])));
        }
        _mm_storeu_si128((__m128i *)(dst+2*i),acc);
        for(k=0;k<4;k++)v[k]=_mm_srli_si128(v[k],8);
    }
#else
    unsigned i;for(i=0;i<4;i++)dst[i]=(int64_t)x*m[i]+(int64_t)y*m[4+i]+(int64_t)z*m[8+i]+m[12+i];
#endif
}

static inline void hle_butterfly4(int32_t *sum,int32_t *diff,const int32_t *a,const int32_t *b,const uint16_t *coeff,int round)
{
#if HLE_SIMD_ENABLED
    __m128i x=_mm_loadu_si128((const __m128i *)a),y=_mm_loadu_si128((const __m128i *)b);
    __m128i c=_mm_cvtepu16_epi32(_mm_loadl_epi64((const __m128i *)coeff));
    __m128i d=_mm_add_epi32(_mm_mullo_epi32(_mm_sub_epi32(x,y),c),_mm_set1_epi32(round?32768:0));
    _mm_storeu_si128((__m128i *)sum,_mm_add_epi32(x,y));
    _mm_storeu_si128((__m128i *)diff,_mm_srai_epi32(d,16));
#else
    unsigned i;for(i=0;i<4;i++){
        uint32_t x=a[i],y=b[i];sum[i]=(int32_t)(x+y);
        diff[i]=(int32_t)((x-y)*coeff[i]+(round?32768u:0))>>16;
    }
#endif
}

/* RE2 keeps each term's truncation and the intermediate green saturation.
 * RGB are independent SIMD lanes; the caller's pixel store order is intact. */
static inline uint32_t hle_re2_rgba(uint8_t y,uint8_t cb,uint8_t cr)
{
#if HLE_SIMD_ENABLED
    __m128i c=_mm_srai_epi32(_mm_mullo_epi32(_mm_set1_epi32(((int)cr-128)*128),_mm_setr_epi32(45941,23401,0,0)),16);
    __m128i b=_mm_srai_epi32(_mm_mullo_epi32(_mm_set1_epi32(((int)cb-128)*128),_mm_setr_epi32(0,11277,58065,0)),16);
    __m128i green=_mm_setr_epi32(0,-1,0,0),lo=_mm_set1_epi32(-32768),hi=_mm_set1_epi32(32767);
    __m128i v=_mm_set1_epi32(((int)y*128*38155)>>16);int32_t out[4];
    c=_mm_sub_epi32(_mm_xor_si128(c,green),green);b=_mm_sub_epi32(_mm_xor_si128(b,green),green);
    v=_mm_max_epi32(lo,_mm_min_epi32(hi,_mm_add_epi32(v,c)));
    v=_mm_max_epi32(lo,_mm_min_epi32(hi,_mm_add_epi32(v,b)));
    v=_mm_srli_epi32(_mm_max_epi32(_mm_setzero_si128(),_mm_min_epi32(_mm_set1_epi32(0x7f80),v)),7);
    _mm_storeu_si128((__m128i *)out,v);
    return ((uint32_t)out[0]<<24)|((uint32_t)out[1]<<16)|((uint32_t)out[2]<<8);
#else
    int32_t yy=((int)y*128*38155)>>16,c=((int)cr-128)*128,b=((int)cb-128)*128;
    int32_t r=hle_sat16(yy+((c*45941)>>16));
    int32_t g=hle_sat16(hle_sat16(yy-((c*23401)>>16))-((b*11277)>>16));
    int32_t blue=hle_sat16(yy+((b*58065)>>16));
    r=r<0?0:r>0x7f80?0x7f80:r;g=g<0?0:g>0x7f80?0x7f80:g;blue=blue<0?0:blue>0x7f80?0x7f80:blue;
    return ((uint32_t)(r>>7)<<24)|((uint32_t)(g>>7)<<16)|((uint32_t)(blue>>7)<<8);
#endif
}

static inline void hle_hvqm_rgba4(uint8_t *out,const int16_t *y,const int16_t *cb,const int16_t *cr,uint8_t alpha)
{
#if HLE_SIMD_ENABLED
    __m128i yy=_mm_add_epi32(_mm_slli_epi32(_mm_cvtepi16_epi32(_mm_loadl_epi64((const __m128i *)y)),6),_mm_set1_epi32(32));
    __m128i u=_mm_sub_epi32(_mm_cvtepi16_epi32(_mm_loadl_epi64((const __m128i *)cb)),_mm_set1_epi32(128));
    __m128i v=_mm_sub_epi32(_mm_cvtepi16_epi32(_mm_loadl_epi64((const __m128i *)cr)),_mm_set1_epi32(128));
    __m128i r=_mm_srai_epi32(_mm_add_epi32(yy,_mm_mullo_epi32(v,_mm_set1_epi32(113))),6);
    __m128i g=_mm_srai_epi32(_mm_sub_epi32(_mm_sub_epi32(yy,_mm_mullo_epi32(v,_mm_set1_epi32(22))),_mm_mullo_epi32(u,_mm_set1_epi32(46))),6);
    __m128i b=_mm_srai_epi32(_mm_add_epi32(yy,_mm_mullo_epi32(u,_mm_set1_epi32(90))),6);
    __m128i z=_mm_setzero_si128(),max=_mm_set1_epi32(255);
    r=_mm_min_epi32(max,_mm_max_epi32(z,r));g=_mm_min_epi32(max,_mm_max_epi32(z,g));b=_mm_min_epi32(max,_mm_max_epi32(z,b));
    _mm_storeu_si128((__m128i *)out,_mm_or_si128(r,_mm_or_si128(_mm_slli_epi32(g,8),_mm_or_si128(_mm_slli_epi32(b,16),_mm_set1_epi32((uint32_t)alpha<<24)))));
#else
    unsigned i,j;for(i=0;i<4;i++) {
        int c=cr[i]-128,u=cb[i]-128,yy=64*y[i]+32;
        int rgb[3]={(yy+113*c)>>6,(yy-22*c-46*u)>>6,(yy+90*u)>>6};
        for(j=0;j<3;j++)out[4*i+j]=rgb[j]<0?0:rgb[j]>255?255:rgb[j];out[4*i+3]=alpha;
    }
#endif
}

/* Eight FIR outputs over an already latched window. Recursive filters and
 * inter-block DMA dependencies stay ordered in the caller. */
static inline void hle_fir8(int16_t *out,const int16_t *win,const int16_t *coeff,unsigned taps,int reverse,const int16_t *dst)
{
#if HLE_SIMD_ENABLED
    __m128i acc[4];unsigned i,k;
    for(i=0;i<4;i++)acc[i]=_mm_set1_epi64x(0x4000);
    for(k=0;k<taps;k++) {
        __m128i a=_mm_loadu_si128((const __m128i *)(win+(reverse?taps-1-k:k)));
        __m128i c=_mm_set1_epi16(coeff[k]);
        __m128i p[2]={hle_mul_s16_lo(a,c),hle_mul_s16_lo(_mm_srli_si128(a,8),c)};
        for(i=0;i<4;i++) {
            __m128i v=(i&1)?_mm_srli_si128(p[i/2],8):p[i/2];
            acc[i]=_mm_add_epi64(acc[i],_mm_cvtepi32_epi64(v));
        }
    }
    /* Up to 64 signed taps fit 22 bits after >>15, including the
     * destination. Narrow to 32 before the final signed-16 clamp. */
    for(i=0;i<4;i++)acc[i]=_mm_srli_epi64(acc[i],15);
    {
        __m128i lo=hle_join64_low32(acc[0],acc[1]),hi=hle_join64_low32(acc[2],acc[3]);
        if(dst){__m128i d=_mm_loadu_si128((const __m128i *)dst);lo=_mm_add_epi32(lo,_mm_cvtepi16_epi32(d));hi=_mm_add_epi32(hi,_mm_cvtepi16_epi32(_mm_srli_si128(d,8)));}
        _mm_storeu_si128((__m128i *)out,_mm_packs_epi32(lo,hi));
    }
#else
    unsigned i,k;for(i=0;i<8;i++){
        int64_t sum=0x4000;for(k=0;k<taps;k++)sum+=(int64_t)win[i+(reverse?taps-1-k:k)]*coeff[k];
        out[i]=hle_sat16((sum>>15)+(dst?dst[i]:0));
    }
#endif
}

static inline void hle_hvqm_basis16(int16_t *out,const int16_t *vec,int32_t scale,int32_t recip)
{
#if HLE_SIMD_ENABLED
    unsigned i,j;for(i=0;i<16;i+=4) {
        __m128i p=_mm_mullo_epi32(_mm_cvtepi16_epi32(_mm_loadl_epi64((const __m128i *)(vec+i))),_mm_set1_epi32(scale)),part[2];
        for(j=0;j<2;j++){
            __m128i v=_mm_shuffle_epi32(p,_MM_SHUFFLE(1,1,0,0));
            part[j]=hle_sra64(_mm_add_epi64(_mm_mul_epi32(v,_mm_set1_epi32(recip)),_mm_set1_epi64x(1<<14)),15);
            p=_mm_srli_si128(p,8);
        }
        p=hle_join64_low32(part[0],part[1]);
        p=_mm_srai_epi32(_mm_slli_epi32(p,16),16);
        p=_mm_add_epi16(_mm_packs_epi32(p,p),_mm_loadl_epi64((const __m128i *)(out+i)));
        _mm_storel_epi64((__m128i *)(out+i),p);
    }
#else
    unsigned i;for(i=0;i<16;i++)out[i]+=(int16_t)(((int64_t)vec[i]*scale*recip+(1<<14))>>15);
#endif
}

static inline void hle_musyx_fir8(int16_t *out,const int16_t *x,const int16_t *coeff,const int16_t *dst)
{
#if HLE_SIMD_ENABLED
    __m128i t[4],a,b,d=_mm_loadu_si128((const __m128i *)dst),lo,hi;unsigned k;
    for(k=0;k<4;k++)t[k]=hle_vmulf16(_mm_loadu_si128((const __m128i *)(x+k)),_mm_set1_epi16(coeff[k]));
    a=_mm_adds_epi16(t[0],t[1]);b=_mm_adds_epi16(t[2],t[3]);
    lo=_mm_add_epi32(_mm_cvtepi16_epi32(d),_mm_add_epi32(_mm_cvtepi16_epi32(a),_mm_cvtepi16_epi32(b)));
    hi=_mm_add_epi32(_mm_cvtepi16_epi32(_mm_srli_si128(d,8)),_mm_add_epi32(_mm_cvtepi16_epi32(_mm_srli_si128(a,8)),_mm_cvtepi16_epi32(_mm_srli_si128(b,8))));
    _mm_storeu_si128((__m128i *)out,_mm_packs_epi32(lo,hi));
#else
    unsigned i,k;for(i=0;i<8;i++){
        int16_t t[4];for(k=0;k<4;k++)t[k]=hle_sat16(((int32_t)x[i+k]*coeff[k]+0x4000)>>15);
        out[i]=hle_sat16((int32_t)dst[i]+hle_sat16((int32_t)t[0]+t[1])+hle_sat16((int32_t)t[2]+t[3]));
    }
#endif
}

static inline void hle_jpeg_rgba8(uint16_t *out,const int16_t *y,const int16_t *u,const int16_t *v)
{
#if HLE_SIMD_ENABLED
    __m128i yy=_mm_adds_epi16(_mm_loadu_si128((const __m128i *)y),_mm_set1_epi16(2048));
    __m128i uu=_mm_loadu_si128((const __m128i *)u),vv=_mm_loadu_si128((const __m128i *)v),r,g,b;
#define HLE_MULHI_SU(a,c) _mm_sub_epi16(_mm_mulhi_epu16((a),_mm_set1_epi16((int16_t)(c))),_mm_and_si128(_mm_srai_epi16((a),15),_mm_set1_epi16((int16_t)(c))))
    r=_mm_adds_epi16(HLE_MULHI_SU(vv,26378),vv);
    g=_mm_adds_epi16(HLE_MULHI_SU(uu,22564),HLE_MULHI_SU(vv,46819));
    b=_mm_adds_epi16(HLE_MULHI_SU(uu,50659),uu);
#undef HLE_MULHI_SU
    r=_mm_adds_epi16(yy,r);g=_mm_subs_epi16(yy,g);b=_mm_adds_epi16(yy,b);
    /* Above-range values keep 0xff0, including its low bits; the in-range
     * mask is 0xf80. This distinction is observable in packed RGBA5551. */
#define HLE_JPEG_CLAMP(c) hle_select(_mm_cmpgt_epi16((c),_mm_set1_epi16(0xff0)),_mm_set1_epi16(0xff0),_mm_and_si128(_mm_max_epi16((c),_mm_setzero_si128()),_mm_set1_epi16(0xf80)))
    r=HLE_JPEG_CLAMP(r);g=HLE_JPEG_CLAMP(g);b=HLE_JPEG_CLAMP(b);
#undef HLE_JPEG_CLAMP
    _mm_storeu_si128((__m128i *)out,_mm_or_si128(_mm_set1_epi16(1),_mm_or_si128(_mm_slli_epi16(r,4),_mm_or_si128(_mm_srli_epi16(g,1),_mm_srli_epi16(b,6)))));
#else
    unsigned i;for(i=0;i<8;i++){
        int32_t yy=hle_sat16((int32_t)y[i]+2048);
        int32_t r=hle_sat16(yy+hle_sat16(((int32_t)v[i]*26378>>16)+v[i]));
        int32_t g=hle_sat16(yy-hle_sat16(((int32_t)u[i]*22564>>16)+((int32_t)v[i]*46819>>16)));
        int32_t b=hle_sat16(yy+hle_sat16(((int32_t)u[i]*50659>>16)+u[i]));
        r=r>0xff0?0xff0:r<0?0:r&0xf80;g=g>0xff0?0xff0:g<0?0:g&0xf80;b=b>0xff0?0xff0:b<0?0:b&0xf80;
        out[i]=(r<<4)|(g>>1)|(b>>6)|1;
    }
#endif
}
#endif
