#include "../state.hpp"
#include "../dpc_bridge.h"

#ifdef PARALLEL_INTEGRATION
#include "../rsp_1.1.h"
#include "m64p_plugin.h"
namespace RSP
{
extern RSP_INFO rsp;
extern short MFC0_count[32];
extern int SP_STATUS_TIMEOUT;
} // namespace RSP
#endif

using namespace RSP;

extern "C"
{

	static void* dpc_opaque;
	static void (*dpc_read)(void*, uint32_t, uint32_t*);
	static void (*dpc_write)(void*, uint32_t, uint32_t, uint32_t);

	void parallelRSPSetDpcCallbacks(void* opaque,
	    void (*read)(void*, uint32_t, uint32_t*),
	    void (*write)(void*, uint32_t, uint32_t, uint32_t))
	{
		dpc_opaque = opaque;
		dpc_read = read;
		dpc_write = write;
	}

#ifdef INTENSE_DEBUG
	void log_rsp_mem_parallel(void);
#endif

	int RSP_MFC0(RSP::CPUState *rsp, unsigned rt, unsigned rd)
	{
		rd &= 15;
		uint32_t res = *rsp->cp0.cr[rd];
		if (rd >= CP0_REGISTER_CMD_START && dpc_read)
			dpc_read(dpc_opaque, (rd - CP0_REGISTER_CMD_START) * 4, &res);
		if (rt)
			rsp->sr[rt] = res;

		if (rd == CP0_REGISTER_SP_RESERVED)
			*rsp->cp0.cr[rd] = 1; // Atomic semaphore test-and-set, even for r0.

#ifdef PARALLEL_INTEGRATION
		if (rd == CP0_REGISTER_SP_RESERVED)
		{
			// Synchronize after the atomic access. The task scheduler gives
			// the CPU a turn before this RSP can acquire the semaphore again.
			*RSP::rsp.SP_STATUS_REG |= SP_STATUS_HALT;
			return MODE_CHECK_FLAGS;
		}
		// WAIT_FOR_CPU_HOST. From CXD4. DPC polling also needs to yield:
		// the CPU can unfreeze the RDP only after this synchronous slice ends.
		if (rd == CP0_REGISTER_SP_STATUS || rd >= CP0_REGISTER_CMD_START)
		{
			RSP::MFC0_count[rt] += 1;
			if (RSP::MFC0_count[rt] >= RSP::SP_STATUS_TIMEOUT)
			{
				*RSP::rsp.SP_STATUS_REG |= SP_STATUS_HALT;
				return MODE_CHECK_FLAGS;
			}
		}
#endif

		//if (rd == 4) // SP_STATUS_REG
		//   fprintf(stderr, "READING STATUS REG!\n");

		return MODE_CONTINUE;
	}

	static inline int rsp_status_write(RSP::CPUState *rsp, uint32_t rt)
	{
		//fprintf(stderr, "Writing 0x%x to status reg!\n", rt);

		uint32_t status = *rsp->cp0.cr[CP0_REGISTER_SP_STATUS];

		if ((rt & (SP_CLR_HALT | SP_SET_HALT)) == SP_CLR_HALT)
			status &= ~SP_STATUS_HALT;
		else if ((rt & (SP_CLR_HALT | SP_SET_HALT)) == SP_SET_HALT)
			status |= SP_STATUS_HALT;

		if (rt & SP_CLR_BROKE)
			status &= ~SP_STATUS_BROKE;

		if ((rt & (SP_CLR_INTR | SP_SET_INTR)) == SP_CLR_INTR)
			*rsp->cp0.irq &= ~1;
		else if ((rt & (SP_CLR_INTR | SP_SET_INTR)) == SP_SET_INTR)
			*rsp->cp0.irq |= 1;

		if ((rt & (SP_CLR_SSTEP | SP_SET_SSTEP)) == SP_CLR_SSTEP)
			status &= ~SP_STATUS_SSTEP;
		else if ((rt & (SP_CLR_SSTEP | SP_SET_SSTEP)) == SP_SET_SSTEP)
			status |= SP_STATUS_SSTEP;

		if ((rt & (SP_CLR_INTR_BREAK | SP_SET_INTR_BREAK)) == SP_CLR_INTR_BREAK)
			status &= ~SP_STATUS_INTR_BREAK;
		else if ((rt & (SP_CLR_INTR_BREAK | SP_SET_INTR_BREAK)) == SP_SET_INTR_BREAK)
			status |= SP_STATUS_INTR_BREAK;

		if ((rt & (SP_CLR_SIG0 | SP_SET_SIG0)) == SP_CLR_SIG0)
			status &= ~SP_STATUS_SIG0;
		else if ((rt & (SP_CLR_SIG0 | SP_SET_SIG0)) == SP_SET_SIG0)
			status |= SP_STATUS_SIG0;

		if ((rt & (SP_CLR_SIG1 | SP_SET_SIG1)) == SP_CLR_SIG1)
			status &= ~SP_STATUS_SIG1;
		else if ((rt & (SP_CLR_SIG1 | SP_SET_SIG1)) == SP_SET_SIG1)
			status |= SP_STATUS_SIG1;

		if ((rt & (SP_CLR_SIG2 | SP_SET_SIG2)) == SP_CLR_SIG2)
			status &= ~SP_STATUS_SIG2;
		else if ((rt & (SP_CLR_SIG2 | SP_SET_SIG2)) == SP_SET_SIG2)
			status |= SP_STATUS_SIG2;

		if ((rt & (SP_CLR_SIG3 | SP_SET_SIG3)) == SP_CLR_SIG3)
			status &= ~SP_STATUS_SIG3;
		else if ((rt & (SP_CLR_SIG3 | SP_SET_SIG3)) == SP_SET_SIG3)
			status |= SP_STATUS_SIG3;

		if ((rt & (SP_CLR_SIG4 | SP_SET_SIG4)) == SP_CLR_SIG4)
			status &= ~SP_STATUS_SIG4;
		else if ((rt & (SP_CLR_SIG4 | SP_SET_SIG4)) == SP_SET_SIG4)
			status |= SP_STATUS_SIG4;

		if ((rt & (SP_CLR_SIG5 | SP_SET_SIG5)) == SP_CLR_SIG5)
			status &= ~SP_STATUS_SIG5;
		else if ((rt & (SP_CLR_SIG5 | SP_SET_SIG5)) == SP_SET_SIG5)
			status |= SP_STATUS_SIG5;

		if ((rt & (SP_CLR_SIG6 | SP_SET_SIG6)) == SP_CLR_SIG6)
			status &= ~SP_STATUS_SIG6;
		else if ((rt & (SP_CLR_SIG6 | SP_SET_SIG6)) == SP_SET_SIG6)
			status |= SP_STATUS_SIG6;

		if ((rt & (SP_CLR_SIG7 | SP_SET_SIG7)) == SP_CLR_SIG7)
			status &= ~SP_STATUS_SIG7;
		else if ((rt & (SP_CLR_SIG7 | SP_SET_SIG7)) == SP_SET_SIG7)
			status |= SP_STATUS_SIG7;

		*rsp->cp0.cr[CP0_REGISTER_SP_STATUS] = status;
		return ((*rsp->cp0.irq & 1) || (status & SP_STATUS_HALT)) ? MODE_CHECK_FLAGS : MODE_CONTINUE;
	}

#ifdef PARALLEL_INTEGRATION
	static int rsp_dma_read(RSP::CPUState *rsp)
	{
		uint32_t length_reg = *rsp->cp0.cr[CP0_REGISTER_DMA_READ_LENGTH];
		uint32_t length = (length_reg & 0xFFF) + 1;
		uint32_t skip = (length_reg >> 20) & 0xFF8;
		unsigned count = (length_reg >> 12) & 0xFF;

		// Force alignment.
		length = (length + 0x7) & ~0x7;
		rsp->cycles += (length / 8) * (count + 1); // Ares DMA: one RSP clock per 8 bytes.
		*rsp->cp0.cr[CP0_REGISTER_DMA_CACHE] &= ~0x7;
		*rsp->cp0.cr[CP0_REGISTER_DMA_DRAM] &= ~0x7;

		unsigned i = 0;
		uint32_t source = *rsp->cp0.cr[CP0_REGISTER_DMA_DRAM];
		uint32_t dest = *rsp->cp0.cr[CP0_REGISTER_DMA_CACHE];

#ifdef INTENSE_DEBUG
		fprintf(stderr, "DMA READ: (0x%x <- 0x%x) len %u, count %u, skip %u\n", dest & 0x1ffc, source & 0x7ffffc,
		        length, count + 1, skip);
#endif

		do
		{
			unsigned j = 0;
			do
			{
				uint32_t source_addr = (source + j) & 0x7FFFFC;
				uint32_t dest_addr = (dest & 0x1000) | ((dest + j) & 0xFFC);
				uint32_t word = rsp->rdram[source_addr >> 2];

				if (dest_addr & 0x1000)
				{
					// Invalidate IMEM.
					unsigned block = (dest_addr & 0xfff) / CODE_BLOCK_SIZE;
					rsp->dirty_blocks |= (0x3 << block) >> 1;
					//rsp->dirty_blocks = ~0u;
					rsp->imem[(dest_addr & 0xfff) >> 2] = word;
				}
				else
					rsp->dmem[dest_addr >> 2] = word;

				j += 4;
			} while (j < length);

			source += length + (i < count ? skip : 0);
			dest = (dest & 0x1000) | ((dest + length) & 0xFFF);
		} while (++i <= count);

		*rsp->cp0.cr[CP0_REGISTER_DMA_DRAM] = source & 0xffffff;
		*rsp->cp0.cr[CP0_REGISTER_DMA_CACHE] = dest;
		*rsp->cp0.cr[CP0_REGISTER_DMA_READ_LENGTH] =
			*rsp->cp0.cr[CP0_REGISTER_DMA_WRITE_LENGTH] = (length_reg & 0xff800000) | 0xff8;

#ifdef INTENSE_DEBUG
		log_rsp_mem_parallel();
#endif
		return rsp->dirty_blocks ? MODE_CHECK_FLAGS : MODE_CONTINUE;
	}

	static void rsp_dma_write(RSP::CPUState *rsp)
	{
		uint32_t length_reg = *rsp->cp0.cr[CP0_REGISTER_DMA_WRITE_LENGTH];
		uint32_t length = (length_reg & 0xFFF) + 1;
		uint32_t skip = (length_reg >> 20) & 0xFF8;
		unsigned count = (length_reg >> 12) & 0xFF;

		// Force alignment.
		length = (length + 0x7) & ~0x7;
		rsp->cycles += (length / 8) * (count + 1);
		*rsp->cp0.cr[CP0_REGISTER_DMA_CACHE] &= ~0x7;
		*rsp->cp0.cr[CP0_REGISTER_DMA_DRAM] &= ~0x7;

		uint32_t dest = *rsp->cp0.cr[CP0_REGISTER_DMA_DRAM];
		uint32_t source = *rsp->cp0.cr[CP0_REGISTER_DMA_CACHE];

#ifdef INTENSE_DEBUG
		fprintf(stderr, "DMA WRITE: (0x%x <- 0x%x) len %u, count %u, skip %u\n", dest & 0x7ffffc, source & 0x1ffc,
		        length, count + 1, skip);
#endif

		unsigned i = 0;
		do
		{
			unsigned j = 0;

			do
			{
				uint32_t source_addr = (source & 0x1000) | ((source + j) & 0xFFC);
				uint32_t dest_addr = (dest + j) & 0x7FFFFC;

				rsp->rdram[dest_addr >> 2] =
				    (source_addr & 0x1000) ? rsp->imem[(source_addr & 0xfff) >> 2] : rsp->dmem[source_addr >> 2];

				j += 4;
			} while (j < length);

			source = (source & 0x1000) | ((source + length) & 0xFFF);
			dest += length + (i < count ? skip : 0);
		} while (++i <= count);

		*rsp->cp0.cr[CP0_REGISTER_DMA_CACHE] = source;
		*rsp->cp0.cr[CP0_REGISTER_DMA_DRAM] = dest & 0xffffff;
		*rsp->cp0.cr[CP0_REGISTER_DMA_READ_LENGTH] =
			*rsp->cp0.cr[CP0_REGISTER_DMA_WRITE_LENGTH] = (length_reg & 0xff800000) | 0xff8;
#ifdef INTENSE_DEBUG
		log_rsp_mem_parallel();
#endif
	}
#endif

	int RSP_MTC0(RSP::CPUState *rsp, unsigned rd, unsigned rt)
	{
		uint32_t val = rsp->sr[rt];
		rd &= 15;
		if (rd >= CP0_REGISTER_CMD_START && dpc_write)
		{
			dpc_write(dpc_opaque, (rd - CP0_REGISTER_CMD_START) * 4, val, ~0u);
			return MODE_CONTINUE;
		}

		switch (static_cast<CP0Registers>(rd & 15))
		{
		case CP0_REGISTER_DMA_CACHE:
			*rsp->cp0.cr[CP0_REGISTER_DMA_CACHE] = val & 0x1ff8;
			break;

		case CP0_REGISTER_DMA_DRAM:
			*rsp->cp0.cr[CP0_REGISTER_DMA_DRAM] = val & 0xfffff8;
			break;

		case CP0_REGISTER_DMA_READ_LENGTH:
			*rsp->cp0.cr[CP0_REGISTER_DMA_READ_LENGTH] = val;
#ifdef PARALLEL_INTEGRATION
			return rsp_dma_read(rsp);
#else
			return MODE_DMA_READ;
#endif

		case CP0_REGISTER_DMA_WRITE_LENGTH:
			*rsp->cp0.cr[CP0_REGISTER_DMA_WRITE_LENGTH] = val;
#ifdef PARALLEL_INTEGRATION
			rsp_dma_write(rsp);
#endif
			break;

		case CP0_REGISTER_SP_STATUS:
			return rsp_status_write(rsp, val);
		case CP0_REGISTER_DMA_FULL:
		case CP0_REGISTER_DMA_BUSY:
			break; // Read-only from either processor.

		case CP0_REGISTER_SP_RESERVED:
			// CXD4 forces this to 0.
			*rsp->cp0.cr[CP0_REGISTER_SP_RESERVED] = 0;
			break;

		case CP0_REGISTER_CMD_START:
#ifdef INTENSE_DEBUG
			fprintf(stderr, "CMD_START 0x%x\n", val & 0xfffffff8u);
#endif
			*rsp->cp0.cr[CP0_REGISTER_CMD_START] = *rsp->cp0.cr[CP0_REGISTER_CMD_CURRENT] =
			    *rsp->cp0.cr[CP0_REGISTER_CMD_END] = val & 0xfffffff8u;
			break;

		case CP0_REGISTER_CMD_END:
#ifdef INTENSE_DEBUG
			fprintf(stderr, "CMD_END 0x%x\n", val & 0xfffffff8u);
#endif
			*rsp->cp0.cr[CP0_REGISTER_CMD_END] = val & 0xfffffff8u;

#ifdef PARALLEL_INTEGRATION
			RSP::rsp.ProcessRdpList();
#endif
			break;

		case CP0_REGISTER_CMD_CLOCK:
			*rsp->cp0.cr[CP0_REGISTER_CMD_CLOCK] = val;
			break;

		case CP0_REGISTER_CMD_STATUS:
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] &= ~(!!(val & 0x1) << 0);
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] |= (!!(val & 0x2) << 0);
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] &= ~(!!(val & 0x4) << 1);
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] |= (!!(val & 0x8) << 1);
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] &= ~(!!(val & 0x10) << 2);
			*rsp->cp0.cr[CP0_REGISTER_CMD_STATUS] |= (!!(val & 0x20) << 2);
			*rsp->cp0.cr[CP0_REGISTER_CMD_TMEM_BUSY] &= !(val & 0x40) * -1;
			*rsp->cp0.cr[CP0_REGISTER_CMD_CLOCK] &= !(val & 0x200) * -1;
			break;

		case CP0_REGISTER_CMD_CURRENT:
		case CP0_REGISTER_CMD_BUSY:
		case CP0_REGISTER_CMD_PIPE_BUSY:
		case CP0_REGISTER_CMD_TMEM_BUSY:
			break;

		default:
			*rsp->cp0.cr[rd & 15] = val;
			break;
		}

		return MODE_CONTINUE;
	}
}
