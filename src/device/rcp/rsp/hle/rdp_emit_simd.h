/* Integer SIMD shared by the angrylion HLE microcode emitters.
 * SSE4.2 / NEON selection and exact accumulator primitives are common
 * with audio HLE. Display-list and FIFO state remain ordered by callers. */
#ifndef PARALLAX_RDP_EMIT_SIMD_H
#define PARALLAX_RDP_EMIT_SIMD_H
#include "hle_simd.h"

#if HLE_SIMD_ENABLED
static inline unsigned rdp_simd_mask8(__m128i a)
{return (unsigned)_mm_movemask_epi8(_mm_packs_epi16(a,_mm_setzero_si128()))&255u;}
static inline __m128i rdp_simd_lt_u16(__m128i a,__m128i b)
{return _mm_cmpgt_epi16(_mm_xor_si128(b,_mm_set1_epi16((int16_t)0x8000)),_mm_xor_si128(a,_mm_set1_epi16((int16_t)0x8000)));}
static inline unsigned rdp_simd_vchvcl8(const int16_t *si,const int16_t *sf,const int16_t *ti,const int16_t *tf)
{
    __m128i z=_mm_setzero_si128(),ones=_mm_cmpeq_epi16(z,z);
    __m128i a=_mm_loadu_si128((const __m128i *)si),b=_mm_loadu_si128((const __m128i *)ti);
    __m128i sign=_mm_cmpgt_epi16(z,_mm_xor_si128(a,b)),cch=_mm_cmpeq_epi16(b,_mm_set1_epi16(-32768));
    __m128i vc=_mm_xor_si128(b,sign),vce=_mm_and_si128(_mm_cmpeq_epi16(a,vc),sign),eq,le,ge,uz,lz,len;
    vc=_mm_add_epi16(vc,_mm_and_si128(_mm_andnot_si128(cch,sign),_mm_set1_epi16(1)));
    eq=_mm_or_si128(_mm_andnot_si128(cch,_mm_cmpeq_epi16(a,vc)),vce);
    ge=_mm_xor_si128(_mm_cmpgt_epi16(b,_mm_or_si128(sign,a)),ones);
    le=hle_select(sign,_mm_xor_si128(_mm_cmpgt_epi16(z,_mm_sub_epi16(vc,a)),ones),_mm_cmpgt_epi16(z,b));
    a=_mm_loadu_si128((const __m128i *)sf);b=_mm_loadu_si128((const __m128i *)tf);
    vc=_mm_sub_epi16(_mm_xor_si128(b,sign),sign);
    uz=_mm_xor_si128(rdp_simd_lt_u16(_mm_add_epi16(a,b),a),ones);
    lz=_mm_cmpeq_epi16(_mm_sub_epi16(a,vc),z);
    len=_mm_or_si128(_mm_and_si128(_mm_or_si128(lz,uz),vce),_mm_andnot_si128(vce,_mm_and_si128(lz,uz)));
    le=hle_select(_mm_and_si128(eq,sign),len,le);
    ge=hle_select(_mm_andnot_si128(sign,eq),_mm_xor_si128(rdp_simd_lt_u16(a,vc),ones),ge);
    return rdp_simd_mask8(le)|(rdp_simd_mask8(ge)<<8);
}

/* Four stored int:frac clip comparisons, retaining the frontend's exact
 * boundary rules and bit layout (LE at 4..7, GE at 12..15). */
static inline unsigned rdp_simd_clip4(const int32_t *integer,const int32_t *fraction,int32_t wi,int32_t wf)
{
    __m128i z=_mm_setzero_si128(),ones=_mm_cmpeq_epi32(z,z);
    __m128i a=_mm_srai_epi32(_mm_slli_epi32(_mm_loadu_si128((const __m128i *)integer),16),16);
    __m128i b=_mm_set1_epi32((int16_t)wi),f=_mm_loadu_si128((const __m128i *)fraction);
    __m128i sign=_mm_cmpgt_epi32(z,_mm_xor_si128(a,b)),sum=_mm_add_epi32(a,b),fs=_mm_add_epi32(f,_mm_set1_epi32(wf));
    __m128i negative=_mm_cmpgt_epi32(z,b),diff=_mm_sub_epi32(a,b),le,ge;
    le=_mm_cmpgt_epi32(z,sum);
    le=hle_select(_mm_cmpeq_epi32(sum,_mm_set1_epi32(-1)),_mm_xor_si128(_mm_cmpgt_epi32(fs,_mm_set1_epi32(65536)),ones),le);
    le=hle_select(_mm_cmpeq_epi32(sum,z),_mm_or_si128(_mm_cmpeq_epi32(fs,z),_mm_cmpeq_epi32(fs,_mm_set1_epi32(65536))),le);
    le=hle_select(sign,le,negative);
    ge=hle_select(_mm_cmpeq_epi32(diff,z),_mm_xor_si128(_mm_cmpgt_epi32(_mm_set1_epi32(wf),f),ones),_mm_cmpgt_epi32(diff,z));
    ge=hle_select(sign,negative,ge);
    return ((unsigned)_mm_movemask_ps(_mm_castsi128_ps(le))<<4)|((unsigned)_mm_movemask_ps(_mm_castsi128_ps(ge))<<12);
}
#endif

/* Position blend with independently truncated frac*weight terms and a
 * single signed/unsigned clamp at the final read (RS/F3DEX clipping). */
static inline void rdp_simd_blend4(int32_t *out,const int32_t *a,const int32_t *b,int wa,int wb)
{
#if HLE_SIMD_ENABLED
    __m128i x=_mm_loadu_si128((const __m128i *)a),y=_mm_loadu_si128((const __m128i *)b),v[2];unsigned i;
    __m128i ga=_mm_set1_epi32((uint16_t)wa),gb=_mm_set1_epi32((uint16_t)wb);
    if((uint16_t)wa+(unsigned)(uint16_t)wb<=65536) {
        /* Convex weights bound the final sum to signed 32 bits. Vector
         * additions may wrap internally; their final bits are exact. */
        __m128i mask=_mm_set1_epi32(65535);
        __m128i f=_mm_add_epi32(_mm_srli_epi32(_mm_mullo_epi32(_mm_and_si128(x,mask),ga),16),_mm_srli_epi32(_mm_mullo_epi32(_mm_and_si128(y,mask),gb),16));
        __m128i integer=_mm_add_epi32(_mm_mullo_epi32(_mm_srai_epi32(x,16),ga),_mm_mullo_epi32(_mm_srai_epi32(y,16),gb));
        _mm_storeu_si128((__m128i *)out,_mm_add_epi32(integer,f));
        return;
    }
    for(i=0;i<2;i++) {
        __m128i xx=_mm_shuffle_epi32(x,_MM_SHUFFLE(1,1,0,0)),yy=_mm_shuffle_epi32(y,_MM_SHUFFLE(1,1,0,0));
        __m128i acc=_mm_add_epi64(hle_sra64(_mm_mul_epi32(xx,ga),16),hle_sra64(_mm_mul_epi32(yy,gb),16));
        v[i]=_mm_or_si128(_mm_slli_epi64(hle_clamp64_s16(hle_sra64(acc,16)),16),hle_rsp_low64(acc));
        x=_mm_srli_si128(x,8);y=_mm_srli_si128(y,8);
    }
    _mm_storeu_si128((__m128i *)out,hle_join64_low32(v[0],v[1]));
#else
    unsigned i;for(i=0;i<4;i++) {
        int64_t acc=(((uint32_t)(a[i]&65535)*(uint16_t)wa)>>16)+(int64_t)(a[i]>>16)*(uint16_t)wa+
                    (((uint32_t)(b[i]&65535)*(uint16_t)wb)>>16)+(int64_t)(b[i]>>16)*(uint16_t)wb;
        int64_t hi=acc>>16;out[i]=hi< -32768?(int32_t)0x80000000:hi>32767?0x7fffffff:(int32_t)acc;
    }
#endif
}

static inline void rdp_simd_blend8(int16_t *out,const int16_t *a,const int16_t *b,int wa,int wb)
{
#if HLE_SIMD_ENABLED
    __m128i x=_mm_loadu_si128((const __m128i *)a),y=_mm_loadu_si128((const __m128i *)b),v[2];unsigned i;
    for(i=0;i<2;i++) {
        __m128i p=_mm_mullo_epi32(_mm_cvtepi16_epi32(x),_mm_set1_epi32((uint16_t)wa));
        __m128i q=_mm_mullo_epi32(_mm_cvtepi16_epi32(y),_mm_set1_epi32((uint16_t)wb));
        if((uint16_t)wa+(unsigned)(uint16_t)wb<=65536) {
            v[i]=_mm_srai_epi32(_mm_add_epi32(p,q),16);
            x=_mm_srli_si128(x,8);y=_mm_srli_si128(y,8);continue;
        }
        __m128i lo=_mm_add_epi64(_mm_cvtepi32_epi64(p),_mm_cvtepi32_epi64(q));
        __m128i hi=_mm_add_epi64(_mm_cvtepi32_epi64(_mm_srli_si128(p,8)),_mm_cvtepi32_epi64(_mm_srli_si128(q,8)));
        v[i]=hle_join64_low32(hle_clamp64_s16(hle_sra64(lo,16)),hle_clamp64_s16(hle_sra64(hi,16)));
        x=_mm_srli_si128(x,8);y=_mm_srli_si128(y,8);
    }
    _mm_storeu_si128((__m128i *)out,_mm_packs_epi32(v[0],v[1]));
#else
    unsigned i;for(i=0;i<8;i++)out[i]=hle_sat16(((int64_t)a[i]*(uint16_t)wa+(int64_t)b[i]*(uint16_t)wb)>>16);
#endif
}

static inline void rdp_simd_light_fold3(int32_t *lt,const int32_t *rgb,const int32_t *rgb2,int d,int d2)
{
#if HLE_SIMD_ENABLED
    __m128i x=_mm_setr_epi32((int16_t)lt[0],(int16_t)lt[1],(int16_t)lt[2],0),parts[2];int32_t out[4];unsigned i;
    __m128i c=_mm_setr_epi32((rgb[0]&255)<<7,(rgb[1]&255)<<7,(rgb[2]&255)<<7,0);
    __m128i p=_mm_mullo_epi32(x,_mm_set1_epi32(32767)),q=_mm_mullo_epi32(c,_mm_set1_epi32(d)),r=_mm_setzero_si128();
    if(rgb2){c=_mm_setr_epi32((rgb2[0]&255)<<7,(rgb2[1]&255)<<7,(rgb2[2]&255)<<7,0);r=_mm_mullo_epi32(c,_mm_set1_epi32(d2));}
    for(i=0;i<2;i++) {
        __m128i a=_mm_add_epi64(_mm_cvtepi32_epi64(p),_mm_cvtepi32_epi64(q));
        a=_mm_add_epi64(a,_mm_add_epi64(_mm_cvtepi32_epi64(r),_mm_set1_epi64x(0x4000)));
        parts[i]=hle_clamp64_s16(hle_sra64(a,15));
        p=_mm_srli_si128(p,8);q=_mm_srli_si128(q,8);r=_mm_srli_si128(r,8);
    }
    _mm_storeu_si128((__m128i *)out,_mm_and_si128(hle_join64_low32(parts[0],parts[1]),_mm_set1_epi32(65535)));
    for(i=0;i<3;i++)lt[i]=out[i];
#else
    unsigned i;for(i=0;i<3;i++) {
        int64_t acc=(int64_t)(int16_t)lt[i]*32767+0x4000+(int64_t)((rgb[i]&255)<<7)*d;
        if(rgb2)acc+=(int64_t)((rgb2[i]&255)<<7)*d2;
        lt[i]=(uint16_t)hle_sat16(acc>>15);
    }
#endif
}

static inline void rdp_simd_rotate4(int64_t *out,const int32_t m[4][4],const int32_t dir[3])
{
#if HLE_SIMD_ENABLED
    __m128i a=_mm_loadu_si128((const __m128i *)m[0]),b=_mm_loadu_si128((const __m128i *)m[1]);
    __m128i c=_mm_loadu_si128((const __m128i *)m[2]),d=_mm_loadu_si128((const __m128i *)m[3]);
    __m128i ab0=_mm_unpacklo_epi32(a,b),ab1=_mm_unpackhi_epi32(a,b),cd0=_mm_unpacklo_epi32(c,d),cd1=_mm_unpackhi_epi32(c,d);
    int32_t tr[16];
    _mm_storeu_si128((__m128i *)tr,_mm_unpacklo_epi64(ab0,cd0));
    _mm_storeu_si128((__m128i *)(tr+4),_mm_unpackhi_epi64(ab0,cd0));
    _mm_storeu_si128((__m128i *)(tr+8),_mm_unpacklo_epi64(ab1,cd1));
    _mm_storeu_si128((__m128i *)(tr+12),_mm_setzero_si128());
    hle_transform4(out,tr,dir[0],dir[1],dir[2]);
#else
    unsigned i,k;for(i=0;i<4;i++){out[i]=0;for(k=0;k<3;k++)out[i]+=(int64_t)m[i][k]*dir[k];}
#endif
}

/* Two four-lane int/fraction attribute vectors -> native RDP packet
 * halfword order. Each vector occupies exactly eight int32_t fields. */
static inline void rdp_simd_pack_attrs(int32_t *out,const int32_t *a,const int32_t *b)
{
#if HLE_SIMD_ENABLED
    __m128i mask=_mm_setr_epi8(8,9,0,1,12,13,4,5,-1,-1,-1,-1,-1,-1,-1,-1);
    __m128i a0=_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)a),mask),a1=_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(a+4)),mask);
    __m128i b0=_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)b),mask),b1=_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(b+4)),mask);
    __m128i x=_mm_unpacklo_epi32(a0,a1),y=_mm_unpacklo_epi32(b0,b1);
    _mm_storeu_si128((__m128i *)out,_mm_unpacklo_epi64(x,y));
    _mm_storeu_si128((__m128i *)(out+4),_mm_unpackhi_epi64(x,y));
#else
    unsigned i;for(i=0;i<2;i++){
        out[i]=((uint32_t)(uint16_t)a[4*i]<<16)|(uint16_t)a[4*i+2];
        out[i+2]=((uint32_t)(uint16_t)b[4*i]<<16)|(uint16_t)b[4*i+2];
        out[i+4]=((uint32_t)(uint16_t)a[4*i+1]<<16)|(uint16_t)a[4*i+3];
        out[i+6]=((uint32_t)(uint16_t)b[4*i+1]<<16)|(uint16_t)b[4*i+3];
    }
#endif
}

/* Vertex VMADN latches the fraction before the final VMADH integer
 * product. Both reads interpret bits 47:16 as a signed 32-bit value. */
static inline void rdp_simd_readback4(int32_t *out,const int64_t *acc,const int32_t *last)
{
#if HLE_SIMD_ENABLED
    __m128i v[2],p[2],lo[2],integer,fraction,partial;unsigned i;
    for(i=0;i<2;i++){
        __m128i end=_mm_cvtepi32_epi64(_mm_loadl_epi64((const __m128i *)(last+2*i)));
        __m128i a=_mm_loadu_si128((const __m128i *)(acc+2*i));
        lo[i]=a;v[i]=_mm_srli_epi64(a,16);
        p[i]=_mm_srli_epi64(_mm_sub_epi64(a,_mm_slli_epi64(end,16)),16);
    }
    integer=hle_join64_low32(v[0],v[1]);partial=hle_join64_low32(p[0],p[1]);
    integer=_mm_min_epi32(_mm_set1_epi32(32767),_mm_max_epi32(_mm_set1_epi32(-32768),integer));
    fraction=_mm_and_si128(hle_join64_low32(lo[0],lo[1]),_mm_set1_epi32(65535));
    fraction=hle_select(_mm_cmpgt_epi32(partial,_mm_set1_epi32(32767)),_mm_set1_epi32(65535),fraction);
    fraction=_mm_andnot_si128(_mm_cmpgt_epi32(_mm_set1_epi32(-32768),partial),fraction);
    _mm_storeu_si128((__m128i *)out,_mm_or_si128(_mm_slli_epi32(integer,16),fraction));
#else
    unsigned i;for(i=0;i<4;i++){
        int32_t hi=(int32_t)((uint64_t)acc[i]>>16),p=(int32_t)(((uint64_t)acc[i]-(uint64_t)((int64_t)last[i]*65536))>>16);
        uint32_t f=p>32767?65535:p< -32768?0:(uint16_t)acc[i];
        out[i]=((uint32_t)(uint16_t)hle_sat16(hi)<<16)|f;
    }
#endif
}

/* ZSortp's stored matrix sum wraps to 32 bits instead of applying the
 * SDK's signed-int / unsigned-fraction saturation. */
static inline void rdp_simd_matrix_wrap4(int32_t *out,const int32_t *a,const int32_t *b)
{
    int32_t result[16];unsigned i,j,k;
#if HLE_SIMD_ENABLED
    for(i=0;i<4;i++){
        __m128i sum[2]={_mm_setzero_si128(),_mm_setzero_si128()};
        for(k=0;k<4;k++){
            __m128i row=_mm_loadu_si128((const __m128i *)(b+4*k)),gain=_mm_set1_epi32(a[4*i+k]);
            for(j=0;j<2;j++){
                __m128i v=_mm_shuffle_epi32(row,_MM_SHUFFLE(1,1,0,0));
                sum[j]=_mm_add_epi64(sum[j],hle_sra64(_mm_mul_epi32(v,gain),16));row=_mm_srli_si128(row,8);
            }
        }
        _mm_storeu_si128((__m128i *)(result+4*i),hle_join64_low32(sum[0],sum[1]));
    }
#else
    for(i=0;i<4;i++)for(j=0;j<4;j++){
        int64_t sum=0;for(k=0;k<4;k++)sum+=((int64_t)a[4*i+k]*b[4*k+j])>>16;result[4*i+j]=(int32_t)sum;
    }
#endif
    memcpy(out,result,sizeof(result));
}
#endif
