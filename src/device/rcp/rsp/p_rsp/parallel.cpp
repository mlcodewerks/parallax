#ifdef DEBUG_JIT
#include "debug_rsp.hpp"
#else
#include "rsp_jit.hpp"
#endif
#include <stdint.h>
#include "dpc_bridge.h"
#include "hle_boot_probe.hpp"

#include "m64p_plugin.h"
#include "rsp_1.1.h"
#ifdef __LIBRETRO__
#include "renderer_options.h"
extern "C" {
#include "api/callbacks.h"
}
#endif

#define RSP_PARALLEL_VERSION 0x0101
#define RSP_PLUGIN_API_VERSION 0x020000

namespace RSP
{
RSP_INFO rsp;
#ifdef DEBUG_JIT
RSP::CPU cpu;
#else
RSP::JIT::CPU cpu;
#endif
short MFC0_count[32];
int SP_STATUS_TIMEOUT;
} // namespace RSP

extern "C"
{
    uint32_t parallelRSPHleBootProbe(const uint32_t *imem,const uint32_t *dmem,
        uint32_t *state,uint16_t *vco)
    { return rsp_hle_boot_probe(imem,dmem,state,vco); }
    uint16_t parallelRSPGetVCO(void)
    {
        return (uint16_t)rsp_get_flags(RSP::cpu.get_state().cp2.flags[RSP::RSP_VCO].e);
    }
    void parallelRSPSetVCO(uint16_t value)
    {
        rsp_set_flags(RSP::cpu.get_state().cp2.flags[RSP::RSP_VCO].e,value);
    }
    uint32_t parallelRSPGetT6(void){return RSP::cpu.get_state().sr[14];}
    void parallelRSPSetT6(uint32_t value){RSP::cpu.get_state().sr[14]=value;}
    uint32_t parallelRSPGetPacketCursor(void){return RSP::cpu.get_state().sr[16];}
    void parallelRSPSetPacketCursor(uint32_t value){RSP::cpu.get_state().sr[16]=value;}
    void parallelRSPGetGraphicsLatches(uint16_t values[32][8])
    {auto& v=RSP::cpu.get_state().cp2.regs;for(unsigned r=0;r<32;r++)for(unsigned k=0;k<8;k++)values[r][k]=v[r].e[k];}
    void parallelRSPSetGraphicsLatches(const uint16_t values[32][8],uint32_t mask)
    {auto& v=RSP::cpu.get_state().cp2.regs;for(unsigned r=0;r<32;r++)if(mask&(1u<<r))for(unsigned k=0;k<8;k++)v[r].e[k]=values[r][k];}
    void parallelRSPGetF5Context(uint32_t values[7])
    {
        auto& v=RSP::cpu.get_state().cp2.regs;
        values[0]=v[12].e[0]|(uint32_t(v[12].e[1])<<16);
        const unsigned regs[3]={2,11,13};
        for(unsigned r=0;r<3;r++)for(unsigned p=0;p<2;p++)
            values[1+r*2+p]=v[regs[r]].e[4+p*2]|(uint32_t(v[regs[r]].e[5+p*2])<<16);
    }
    void parallelRSPSetF5Context(const uint32_t values[7],uint32_t mask)
    {
        auto& v=RSP::cpu.get_state().cp2.regs;
        const unsigned regs[7]={12,2,2,11,11,13,13},lanes[7]={0,4,6,4,6,4,6};
        for(unsigned p=0;p<7;p++)if(mask&(1u<<p)){
            v[regs[p]].e[lanes[p]]=uint16_t(values[p]);
            v[regs[p]].e[lanes[p]+1]=uint16_t(values[p]>>16);
        }
    }
    void parallelRSPSaveState(uint32_t* words)
    {
        auto& s = RSP::cpu.get_state();
        *words++ = s.pc; *words++ = s.has_delay_slot;
        *words++ = s.branch_target; *words++ = s.cycles;
        *words++ = RSP::SP_STATUS_TIMEOUT;
        for (auto value : s.sr) *words++ = value;
        for (auto& reg : s.cp2.regs) for (auto value : reg.e) *words++ = value;
        for (auto& flag : s.cp2.flags) for (auto value : flag.e) *words++ = value;
        for (auto value : s.cp2.acc.e) *words++ = value;
        *words++ = uint16_t(s.cp2.div_out); *words++ = uint16_t(s.cp2.div_in);
        *words++ = uint8_t(s.cp2.dp_flag);
    }

    void parallelRSPLoadState(const uint32_t* words)
    {
        auto& s = RSP::cpu.get_state();
        s.pc = *words++; s.has_delay_slot = *words++;
        s.branch_target = *words++; s.cycles = *words++;
        RSP::SP_STATUS_TIMEOUT = *words++;
        for (auto& value : s.sr) value = *words++;
        for (auto& reg : s.cp2.regs) for (auto& value : reg.e) value = uint16_t(*words++);
        for (auto& flag : s.cp2.flags) for (auto& value : flag.e) value = uint16_t(*words++);
        for (auto& value : s.cp2.acc.e) value = uint16_t(*words++);
        s.cp2.div_out = int16_t(*words++); s.cp2.div_in = int16_t(*words++);
        s.cp2.dp_flag = int8_t(*words++);
        s.dirty_blocks = ~0u;
        RSP::cpu.invalidate_imem();
    }

	// Hack entry point to use when loading savestates when we're tracing.
	void rsp_clear_registers()
	{
		memset(RSP::cpu.get_state().sr, 0, sizeof(uint32_t) * 32);
		memset(&RSP::cpu.get_state().cp2, 0, sizeof(RSP::cpu.get_state().cp2));
	}

#ifdef INTENSE_DEBUG
	// Need super-fast hash here.
	static uint64_t hash_imem(const uint8_t *data, size_t size)
	{
		uint64_t h = 0xcbf29ce484222325ull;
		size_t i;
		for (i = 0; i < size; i++)
			h = (h * 0x100000001b3ull) ^ data[i];
		return h;
	}

	void log_rsp_mem_parallel(void)
	{
		fprintf(stderr, "IMEM HASH: 0x%016llx\n", hash_imem(RSP::rsp.IMEM, 0x1000));
		fprintf(stderr, "DMEM HASH: 0x%016llx\n", hash_imem(RSP::rsp.DMEM, 0x1000));
	}
#endif

	EXPORT unsigned int CALL parallelRSPDoRspCycles(unsigned int cycles)
	{
		if (*RSP::rsp.SP_STATUS_REG & (SP_STATUS_HALT | SP_STATUS_BROKE))
			return 0;

		// We don't know if Mupen from the outside invalidated our IMEM.
		RSP::cpu.invalidate_imem();

		// Run CPU until we either break or we need to fire an IRQ.
		RSP::cpu.get_state().pc = *RSP::rsp.SP_PC_REG & 0xfff;
		RSP::cpu.get_state().cycles = 0;

#ifdef INTENSE_DEBUG
		fprintf(stderr, "RUN TASK: %u\n", RSP::cpu.get_state().pc);
		log_rsp_mem_parallel();
#endif

		for (auto &count : RSP::MFC0_count)
			count = 0;

		while (!(*RSP::rsp.SP_STATUS_REG & SP_STATUS_HALT))
		{
#ifdef DEBUG_JIT
			auto mode = RSP::cpu.run();
#else
			// Streaming microcode can compute indefinitely without a CP0 poll.
			// Return at a basic-block boundary so CPU events and DMA can progress.
			auto mode = RSP::cpu.run(RSP::cpu.cycle_timing_enabled() ? 32768 : UINT32_MAX);
			if (mode == RSP::MODE_TIMESLICE) break;
#endif
			if (mode == RSP::MODE_CHECK_FLAGS && (*RSP::cpu.get_state().cp0.irq & 1))
				break;
		}

		*RSP::rsp.SP_PC_REG = 0x04001000 | (RSP::cpu.get_state().pc & 0xffc);
		cycles = RSP::cpu.get_state().cycles;

		// From CXD4.
		if (*RSP::rsp.SP_STATUS_REG & SP_STATUS_BROKE)
			return cycles;
		else if (*RSP::cpu.get_state().cp0.irq & 1)
			RSP::rsp.CheckInterrupts();
		// Bound subsequent polls even while the semaphore is held: the CPU
		// cannot release it until this synchronous RSP invocation yields.
		RSP::SP_STATUS_TIMEOUT = 16;

		// CPU restarts with the correct SIGs.
		*RSP::rsp.SP_STATUS_REG &= ~SP_STATUS_HALT;

		return cycles;
	}

	unsigned int parallelRSPExecutedCycles(void)
	{
		return RSP::cpu.get_state().cycles;
	}
    void parallelRSPResetExecutedCycles(void) { RSP::cpu.get_state().cycles = 0; }

	EXPORT m64p_error CALL parallelRSPPluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion,
	                                                   int *APIVersion, const char **PluginNamePtr, int *Capabilities)
	{
		/* set version info */
		if (PluginType != NULL)
			*PluginType = M64PLUGIN_RSP;

		if (PluginVersion != NULL)
			*PluginVersion = RSP_PARALLEL_VERSION;

		if (APIVersion != NULL)
			*APIVersion = RSP_PLUGIN_API_VERSION;

		if (Capabilities != NULL)
			*Capabilities = 0;

		return M64ERR_SUCCESS;
	}

	EXPORT void CALL parallelRSPRomClosed(void)
	{
		*RSP::rsp.SP_PC_REG = 0x00000000;
	}

	EXPORT void CALL parallelRSPInitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount)
	{
		if (CycleCount)
			*CycleCount = 0;

		if (Rsp_Info.DMEM == Rsp_Info.IMEM) /* usually dummy RSP data for testing */
			return; /* DMA is not executed just because plugin initiates. */

		RSP::rsp = Rsp_Info;
		*RSP::rsp.SP_PC_REG = 0x04001000 & 0x00000FFF; /* task init bug on Mupen64 */

		auto **cr = RSP::cpu.get_state().cp0.cr;
		cr[0x0] = RSP::rsp.SP_MEM_ADDR_REG;
		cr[0x1] = RSP::rsp.SP_DRAM_ADDR_REG;
		cr[0x2] = RSP::rsp.SP_RD_LEN_REG;
		cr[0x3] = RSP::rsp.SP_WR_LEN_REG;
		cr[0x4] = RSP::rsp.SP_STATUS_REG;
		cr[0x5] = RSP::rsp.SP_DMA_FULL_REG;
		cr[0x6] = RSP::rsp.SP_DMA_BUSY_REG;
		cr[0x7] = RSP::rsp.SP_SEMAPHORE_REG;
		cr[0x8] = RSP::rsp.DPC_START_REG;
		cr[0x9] = RSP::rsp.DPC_END_REG;
		cr[0xA] = RSP::rsp.DPC_CURRENT_REG;
		cr[0xB] = RSP::rsp.DPC_STATUS_REG;
		cr[0xC] = RSP::rsp.DPC_CLOCK_REG;
		cr[0xD] = RSP::rsp.DPC_BUFBUSY_REG;
		cr[0xE] = RSP::rsp.DPC_PIPEBUSY_REG;
		cr[0xF] = RSP::rsp.DPC_TMEM_REG;

		*cr[RSP::CP0_REGISTER_SP_STATUS] = SP_STATUS_HALT;
		RSP::cpu.get_state().cp0.irq = RSP::rsp.MI_INTR_REG;

		// From CXD4.
		RSP::SP_STATUS_TIMEOUT = 0x7fff;

		RSP::cpu.set_dmem(reinterpret_cast<uint32_t *>(Rsp_Info.DMEM));
		RSP::cpu.set_imem(reinterpret_cast<uint32_t *>(Rsp_Info.IMEM));
		RSP::cpu.set_rdram(reinterpret_cast<uint32_t *>(Rsp_Info.RDRAM));
#if defined(__LIBRETRO__) && !defined(DEBUG_JIT)
        RSP::cpu.set_cycle_timing(renderer_component_timing());
        DebugMessage(M64MSG_INFO,"RSP backend: JIT LLE; per-cycle timing %s",
                     renderer_component_timing() ? "on" : "off");
#endif
	}
}
