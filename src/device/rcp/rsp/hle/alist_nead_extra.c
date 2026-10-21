#include "hle_simd.h"
/* NEAD commands recovered from the complete cartridge microcode images. */
#include "alist.h"
#include "hle_internal.h"
#include "hle_timing.h"
#include "memory.h"

static uint8_t get_byte(struct hle_t* hle, uint32_t at)
{
    return hle->alist_buffer[(at & 0xfffu) ^ S8];
}
static void put_byte(struct hle_t* hle, uint32_t at, uint8_t value)
{
    hle->alist_buffer[(at & 0xfffu) ^ S8] = value;
}
static int16_t get_half(struct hle_t* hle, uint32_t at)
{
    return (int16_t)((uint16_t)get_byte(hle, at) << 8 | get_byte(hle, at + 1));
}
static void put_half(struct hle_t* hle, uint32_t at, uint16_t value)
{
    put_byte(hle, at, (uint8_t)(value >> 8));put_byte(hle, at + 1, (uint8_t)value);
}

/* MK SEGMENT stores the low 24 address bits in the real DMEM table.
 * The complete selector byte is used, so writes can alias command state. */
void alist_nead_segment(struct hle_t* hle, uint32_t w2)
{
    uint32_t at = 0x320u + (w2 >> 24)*4u, value = w2 & 0xffffffu;
    unsigned byte;
    for (byte = 0; byte < 4; ++byte) put_byte(hle, at+byte, (uint8_t)(value >> (24u-byte*8u)));
}
uint32_t alist_nead_segment_address(struct hle_t* hle, uint32_t w2)
{
    uint32_t at = 0x320u + (w2 >> 24)*4u, base = 0;unsigned byte;
    for (byte = 0; byte < 4; ++byte) base = base << 8 | get_byte(hle, at+byte);
    return base + (w2 & 0xffffffu);
}

/* SF opcode 19 / MM and Stadium 2 opcode 00. The 32 coefficients are
 * latched once; four vectors of signed products saturate independently.
 * All four inputs are fetched before any output is stored. */
void alist_nead_multiply(struct hle_t* hle, uint32_t w1, uint32_t w2)
{
    uint32_t at = w2 >> 16, table = (w2 & 0xffffu) + ((w1 >> 16) & 255u);
    int32_t count = w1 & 0xffffu;
    int16_t coefficients[32], samples[32];
    unsigned i;
    for (i = 0; i < 32; ++i) coefficients[i] = get_half(hle, table + 2*i);
    do {
        for (i = 0; i < 32; ++i) samples[i] = get_half(hle, at + 2*i);
        for (i = 0; i < 32; i+=8)
            hle_scale8(samples+i,samples+i,coefficients+i,0,0);
        for (i = 0; i < 32; ++i)
            put_half(hle,at+2*i,(uint16_t)samples[i]);
        hle_timing_add(HLE_WORK_MIX, 4);
        at += 64;count -= 64;
    } while (count > 0);
}

/* Animal Forest's index reduction uses a strict-positive subtraction
 * test. Exact multiples retain the divisor instead of becoming zero. */
static uint32_t indexed_address(struct hle_t* hle, uint32_t *indices,
    uint32_t base, uint32_t period)
{
    uint32_t value = (uint16_t)get_half(hle, *indices) & 0x7fffu;
    unsigned bit;
    *indices += 2;
    for (bit = 0; bit < 8; ++bit) {
        uint32_t reductions = period && value ? (value - 1u) / period + 1u : 1u;
        hle_timing_add(HLE_WORK_INDEX, reductions);
        if (period && value) value = ((value - 1u) % period) + 1u;
        period >>= 1;
    }
    return base + value*2u;
}

/* AC opcode 00: add paired indexed samples in place with 16-bit wrap,
 * then exchange paired eight-byte windows. Indices share the sample
 * buffer, so each pair is resolved from the current DMEM image. */
void alist_nead_indexed(struct hle_t* hle, uint32_t w1, uint32_t w2)
{
    uint32_t base = w2 & 0xffffu, indices = base;
    uint32_t period = (w1 >> 16) & 255u, count = w1 & 0xffffu;
    uint32_t swaps = w2 >> 16;
    while (count--) {
        uint32_t first = indexed_address(hle, &indices, base, period << 7);
        uint32_t second = indexed_address(hle, &indices, base, period << 7);
        uint16_t sum = (uint16_t)((uint16_t)get_half(hle, first) + (uint16_t)get_half(hle, second));
        put_half(hle, first, sum);put_half(hle, second, sum);
        hle_timing_add(HLE_WORK_MIX, 1);
    }
    period -= 8u;
    while (swaps--) {
        uint32_t first = indexed_address(hle, &indices, base, period << 7);
        uint32_t second = indexed_address(hle, &indices, base, period << 7);
        uint8_t a[8], b[8];unsigned i;
        for (i = 0; i < 8; ++i) {a[i] = get_byte(hle, first+i);b[i] = get_byte(hle, second+i);}
        for (i = 0; i < 8; ++i) put_byte(hle, second+i, a[i]);
        for (i = 0; i < 8; ++i) put_byte(hle, first+i, b[i]);
        hle_timing_add(HLE_WORK_COPY, 1);
    }
}

static void packed_samples(struct hle_t* hle, uint32_t at, uint8_t samples[32])
{
    unsigned i;
    for (i = 0; i < 16; ++i) {samples[2*i] = get_byte(hle, at+i);samples[2*i+1] = 0;}
}
static void packed_half(struct hle_t* hle, uint32_t at, uint8_t samples[16])
{
    unsigned i;
    for (i = 0; i < 8; ++i) {samples[2*i] = get_byte(hle, at+i);samples[2*i+1] = 0;}
}
static void store_half(struct hle_t* hle, uint32_t at, const uint8_t samples[16])
{
    unsigned i, bytes = 16u - (at & 15u);
    for (i = 0; i < bytes; ++i) put_byte(hle, at+i, samples[i]);
}
static void store_samples(struct hle_t* hle, uint32_t at, const uint8_t samples[32])
{
    unsigned i, bytes = 16u - (at & 15u);
    for (i = 0; i < bytes; ++i) put_byte(hle, at+i, samples[i]);
    for (i = 0; i < bytes; ++i) put_byte(hle, at+16u+i, samples[16u+i]);
}

/* Signed 8-bit decoder. The two alternating batches prefetch the next
 * sixteen source bytes before storing, which matters for overlapping
 * input/output. SQV leaves the remainder of an unaligned row intact. */
void alist_nead_s8(struct hle_t* hle, uint32_t w1, uint32_t w2, uint16_t scratch, uint16_t loop_offset, int pipelined)
{
    uint32_t input = (uint16_t)get_half(hle, scratch), output = (uint16_t)get_half(hle, scratch+2u);
    uint32_t address = w2 & 0xffffffu;
    uint32_t count = (uint16_t)get_half(hle, scratch+4u);
    int32_t remaining = (int32_t)count;
    uint8_t current[32] = {0}, next[32];
    store_samples(hle, output, current);
    if (!(w1 & 0x10000u)) {
        uint32_t source = address;
        if (w1 & 0x20000u) {
            unsigned k;source = 0;
            for (k = 0; k < 4; ++k) source = source << 8 | get_byte(hle, scratch+loop_offset+k);
        }
        alist_dmem_dma(hle, output, source, 31u, 0);
    }
    output += 32;
    if (remaining && !pipelined) {
        do {
            packed_samples(hle, input, current);
            store_samples(hle, output, current);
            input += 16;output += 32;remaining -= 32;
        } while (remaining > 0);
    } else if (remaining) {
        packed_samples(hle, input, current);input += 16;
        for (;;) {
            remaining -= 32;
            packed_half(hle, input, next);
            store_half(hle, output, current);
            packed_half(hle, input+8, next+16);
            store_half(hle, output+16, current+16);output += 32;
            if (remaining <= 0) break;
            remaining -= 32;
            packed_half(hle, input+16, current);
            store_half(hle, output, next);
            packed_half(hle, input+24, current+16);
            store_half(hle, output+16, next+16);output += 32;input += 32;
            if (remaining <= 0) break;
        }
    }
    hle_timing_add(HLE_WORK_COPY, (count + 31u) / 32u * 2u);
    alist_dmem_dma(hle, output-32u, address, 31u, 1);
}
