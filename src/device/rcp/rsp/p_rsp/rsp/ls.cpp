#include "../state.hpp"

#ifdef TRACE_COP2
#include <stdio.h>
#define TRACE_LS(op) printf(#op " v%u, %u, %d(r%u)\n", rt, e, offset, base)
#else
#define TRACE_LS(op) ((void)0)
#endif


// Loads truncate at vector byte 16; stores wrap their vector source bytes.
// Keep halfword transfers for aligned accesses used by common microcode.
static inline void load_vector_bytes(RSP::CPUState* rsp, unsigned rt,
                                     unsigned e, unsigned addr, unsigned count)
{
    if (count > 16 - e) count = 16 - e;
    auto* reg = rsp->cp2.regs[rt].e;
    if (((addr | e | count) & 1) == 0)
        for (unsigned i = 0; i < count; i += 2)
            reg[(e + i) >> 1] = READ_MEM_U16(rsp->dmem, (addr + i) & 0xfff);
    else
        for (unsigned i = 0; i < count; ++i)
            reinterpret_cast<uint8_t*>(reg)[MES(e + i)] = READ_MEM_U8(rsp->dmem, (addr + i) & 0xfff);
}

static inline void store_vector_bytes(RSP::CPUState* rsp, unsigned rt,
                                      unsigned e, unsigned addr, unsigned count)
{
    const auto* reg = rsp->cp2.regs[rt].e;
    if (((addr | e | count) & 1) == 0)
        for (unsigned i = 0; i < count; i += 2)
            WRITE_MEM_U16(rsp->dmem, (addr + i) & 0xfff, reg[((e + i) & 15) >> 1]);
    else
        for (unsigned i = 0; i < count; ++i)
            WRITE_MEM_U8(rsp->dmem, (addr + i) & 0xfff,
                         reinterpret_cast<const uint8_t*>(reg)[MES((e + i) & 15)]);
}

static inline void load_packed_vector(RSP::CPUState* rsp, unsigned rt,
                                      unsigned e, unsigned addr, unsigned stride, unsigned shift)
{
    unsigned index = (addr & 7) - e;
    addr &= 0xff8;
    for (unsigned i = 0; i < 8; ++i)
        rsp->cp2.regs[rt].e[i] = READ_MEM_U8(rsp->dmem, (addr + ((index + i * stride) & 15)) & 0xfff) << shift;
}

static inline void store_packed_vector(RSP::CPUState* rsp, unsigned rt,
                                       unsigned e, unsigned addr, unsigned shift)
{
    for (unsigned i = 0; i < 8; ++i)
    {
        unsigned element = e + i;
        unsigned bits = (element & 8) ? 15 - shift : shift;
        WRITE_MEM_U8(rsp->dmem, (addr + i) & 0xfff, rsp->cp2.regs[rt].e[element & 7] >> bits);
    }
}

extern "C"
{
	// DMEM accesses wrap within 4 KiB. Vector loads truncate while stores
	// wrap their source bytes; packed accesses use rotating 16-byte windows.

	// Load 8-bit
	void RSP_LBV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LBV);
		unsigned addr = (rsp->sr[base] + offset * 1) & 0xfff;
		reinterpret_cast<uint8_t *>(rsp->cp2.regs[rt].e)[MES(e)] = READ_MEM_U8(rsp->dmem, addr);
	}

	// Store 8-bit
	void RSP_SBV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SBV);
		unsigned addr = (rsp->sr[base] + offset * 1) & 0xfff;
		uint8_t v = reinterpret_cast<uint8_t *>(rsp->cp2.regs[rt].e)[MES(e)];

#ifdef INTENSE_DEBUG
		fprintf(stderr, "SBV: 0x%x (0x%x)\n", addr, v);
#endif

		WRITE_MEM_U8(rsp->dmem, addr, v);
	}

	// Load 16-bit
	void RSP_LSV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LSV);
		load_vector_bytes(rsp, rt, e, (rsp->sr[base] + offset * 2) & 0xfff, 2);
	}

	// Store 16-bit
	void RSP_SSV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SSV);
		unsigned addr = (rsp->sr[base] + offset * 2) & 0xfff;
		uint8_t v0 = reinterpret_cast<uint8_t *>(rsp->cp2.regs[rt].e)[MES(e)];
		uint8_t v1 = reinterpret_cast<uint8_t *>(rsp->cp2.regs[rt].e)[MES((e + 1) & 0xf)];

#ifdef INTENSE_DEBUG
		fprintf(stderr, "SSV: 0x%x (0x%x, 0x%x)\n", addr, v0, v1);
#endif

		WRITE_MEM_U8(rsp->dmem, addr, v0);
		WRITE_MEM_U8(rsp->dmem, (addr + 1) & 0xfff, v1);
	}

	// Load 32-bit
	void RSP_LLV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LLV);
		load_vector_bytes(rsp, rt, e, (rsp->sr[base] + offset * 4) & 0xfff, 4);
	}

	// Store 32-bit
	void RSP_SLV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SLV);
		store_vector_bytes(rsp, rt, e, (rsp->sr[base] + offset * 4) & 0xfff, 4);
	}

	// Load 64-bit
	void RSP_LDV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LDV);
		load_vector_bytes(rsp, rt, e, (rsp->sr[base] + offset * 8) & 0xfff, 8);
	}

	// Store 64-bit
	void RSP_SDV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SDV);
		unsigned addr = (rsp->sr[base] + offset * 8) & 0xfff;

#ifdef INTENSE_DEBUG
		fprintf(stderr, "SDV 0x%x, e = %u\n", addr, e);
#endif

		// Byte accesses cover odd alignment and wrapping vector sources.
		if ((e > 8) || (e & 1) || (addr & 1))
		{
			for (unsigned i = 0; i < 8; i++)
			{
				WRITE_MEM_U8(rsp->dmem, (addr + i) & 0xfff,
				             reinterpret_cast<const uint8_t *>(rsp->cp2.regs[rt].e)[MES((e + i) & 0xf)]);
			}
		}
		else
		{
			e >>= 1;
			for (unsigned i = 0; i < 4; i++)
			{
				WRITE_MEM_U16(rsp->dmem, (addr + 2 * i) & 0xfff, rsp->cp2.regs[rt].e[e + i]);
			}
		}
	}

	// Load 8x8-bit into high bits.
	void RSP_LPV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LPV);
		load_packed_vector(rsp, rt, e, (rsp->sr[base] + offset * 8) & 0xfff, 1, 8);
	}

	void RSP_SPV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SPV);
		store_packed_vector(rsp, rt, e, (rsp->sr[base] + offset * 8) & 0xfff, 8);
	}

	// Load 8x8-bit into high bits, but shift by 7 instead of 8.
	// Was probably used for certain fixed point algorithms to get more headroom without
	// saturation, but weird nonetheless.
	void RSP_LUV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LUV);
		load_packed_vector(rsp, rt, e, (rsp->sr[base] + offset * 8) & 0xfff, 1, 7);
	}

	void RSP_SUV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SUV);
		store_packed_vector(rsp, rt, e, (rsp->sr[base] + offset * 8) & 0xfff, 7);
	}

	// Load 8x8-bits into high bits, but shift by 7 instead of 8.
	// Seems to differ from LUV in that it loads every other byte instead of packed bytes.
	void RSP_LHV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LHV);
		load_packed_vector(rsp, rt, e, (rsp->sr[base] + offset * 16) & 0xfff, 2, 7);
	}

	void RSP_SHV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SHV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		unsigned index = addr & 7;
		addr &= 0xff8;
		const auto* bytes = reinterpret_cast<const uint8_t*>(rsp->cp2.regs[rt].e);
		for (unsigned i = 0; i < 8; ++i)
		{
		    unsigned byte = e + i * 2;
		    unsigned value = (bytes[MES(byte & 15)] << 1) | (bytes[MES((byte + 1) & 15)] >> 7);
		    WRITE_MEM_U8(rsp->dmem, (addr + ((index + i * 2) & 15)) & 0xfff, value);
		}
	}

	// No idea what the purpose of this is.
	void RSP_SFV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SFV);
		static const uint8_t lanes[16][4] = {
		    {0,1,2,3}, {6,7,4,5}, {0,0,0,0}, {0,0,0,0},
		    {1,2,3,0}, {7,4,5,6}, {0,0,0,0}, {0,0,0,0},
		    {4,5,6,7}, {0,0,0,0}, {0,0,0,0}, {3,0,1,2},
		    {5,6,7,4}, {0,0,0,0}, {0,0,0,0}, {0,1,2,3}
		};
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		unsigned index = addr & 7;
		addr &= 0xff8;
		const unsigned valid = (0x9933u >> e) & 1;
		for (unsigned i = 0; i < 4; ++i)
		{
		    unsigned value = valid ? rsp->cp2.regs[rt].e[lanes[e][i]] >> 7 : 0;
		    WRITE_MEM_U8(rsp->dmem, (addr + ((index + i * 4) & 15)) & 0xfff, value);
		}
	}

	// Loads full 128-bit register, however, it seems to handle unaligned addresses in a very
	// strange way.
	void RSP_LQV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LQV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		load_vector_bytes(rsp, rt, e, addr, 16 - (addr & 15));
	}

	void RSP_SQV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SQV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		store_vector_bytes(rsp, rt, e, addr, 16 - (addr & 15));
	}

	// Complements LQV?
	void RSP_LRV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LRV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		unsigned tail = addr & 15;
		if (tail > e)
		    load_vector_bytes(rsp, rt, 16 - tail + e, addr & 0xff0, tail - e);
	}

	void RSP_SRV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(SRV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		unsigned tail = addr & 15;
		store_vector_bytes(rsp, rt, (e + 16 - tail) & 15, addr & 0xff0, tail);
	}

	// Distribute an eight-byte-aligned memory window over VT's register group.
	void RSP_LTV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(LTV);
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		unsigned window = addr & 0xff8;
		unsigned index = e + (addr & 8);
		unsigned group = rt & ~7u;
		for (unsigned i = 0; i < 8; ++i)
		{
		    unsigned hi = READ_MEM_U8(rsp->dmem, (window + ((index + i * 2) & 15)) & 0xfff);
		    unsigned lo = READ_MEM_U8(rsp->dmem, (window + ((index + i * 2 + 1) & 15)) & 0xfff);
		    rsp->cp2.regs[group + (((e >> 1) + i) & 7)].e[i] = (hi << 8) | lo;
		}
	}

	void RSP_STV(RSP::CPUState *rsp, unsigned rt, unsigned e, int offset, unsigned base)
	{
		TRACE_LS(STV);
		if (e & 1)
			return;
		if (rt & 7)
			return;
		unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
		if (addr & 0xf)
			return;

		for (unsigned i = 0; i < 8; i++)
		{
			WRITE_MEM_U16(rsp->dmem, addr + 2 * i, rsp->cp2.regs[rt + ((e / 2 + i) & 7)].e[i]);
		}
	}

    // Store a whole vector into a rotating 16-byte, eight-byte-aligned window.
    void RSP_SWV(RSP::CPUState* rsp, unsigned rt, unsigned e, int offset, unsigned base)
    {
        TRACE_LS(SWV);
        unsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;
        unsigned index = addr & 7;
        addr &= 0xff8;
        const auto* bytes = reinterpret_cast<const uint8_t*>(rsp->cp2.regs[rt].e);
        for (unsigned i = 0; i < 16; ++i)
            WRITE_MEM_U8(rsp->dmem, (addr + ((index + i) & 15)) & 0xfff, bytes[MES((e + i) & 15)]);
    }
}
