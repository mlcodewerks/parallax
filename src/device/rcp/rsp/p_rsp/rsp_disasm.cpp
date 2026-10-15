#include "rsp_disasm.hpp"
#include <assert.h>

namespace RSP
{
#define DISASM(asmfmt, ...) do { \
    char buf[1024]; \
    sprintf(buf, "0x%03x   " asmfmt, pc, __VA_ARGS__); \
    return buf; \
} while(0)

#define DISASM_NOP() do { \
    char buf[1024]; \
    sprintf(buf, "0x%03x   nop", pc); \
    return buf; \
} while(0)

static const char *reg_names[32] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};
#define NAME(reg) reg_names[reg]

const char *register_name(unsigned reg_index)
{
	assert(reg_index < 32);
	return reg_names[reg_index];
}

std::string disassemble(uint32_t pc, uint32_t instr)
{
	// VU
	if ((instr >> 25) == 0x25)
	{
		// VU instruction. COP2, and high bit of opcode is set.
		uint32_t op = instr & 63;
		uint32_t vd = (instr >> 6) & 31;
		uint32_t vs = (instr >> 11) & 31;
		uint32_t vt = (instr >> 16) & 31;
		uint32_t e = (instr >> 21) & 15;

		static const char *ops_str[64] = {
			"VMULF", "VMULU", nullptr, nullptr, "VMUDL", "VMUDM", "VMUDN", "VMUDH", "VMACF", "VMACU", nullptr,
			nullptr, "VMADL", "VMADM", "VMADN", "VMADH", "VADD",  "VSUB",  nullptr, "VABS",  "VADDC", "VSUBC",
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "VSAR",  nullptr, nullptr, "VLT",
			"VEQ",   "VNE",   "VGE",   "VCL",   "VCH",   "VCR",   "VMRG",  "VAND",  "VNAND", "VOR",   "VNOR",
			"VXOR",  "VNXOR", nullptr, nullptr, "VRCP",  "VRCPL", "VRCPH", "VMOV",  "VRSQ",  "VRSQL", "VRSQH",
			"VNOP",
		};

		if (ops_str[op])
		{
			if (op >= 0x30 && op <= 0x36)
				DISASM("%s v%u[%u], v%u[%u]", ops_str[op], vd, vs & 7, vt, e & 7);
			DISASM("%s v%u, v%u, v%u[%u]", ops_str[op], vd, vs, vt, e);
		}
		else
			DISASM("cop2 %u reserved", op);
	}

	// TODO: Meaningful register allocation.
	// For now, always flush register state to memory after an instruction for simplicity.
	// Should be red-hot in L1 cache, so probably won't be that bad.
	// On x86 and x64, we unfortunately have an anemic register bank to work with in Lightning.

	uint32_t type = instr >> 26;

#define NOP_IF_RD_ZERO() if (instr == 0) { DISASM_NOP(); break; }
#define NOP_IF_RT_ZERO() ((void)0)

	switch (type)
	{
	case 000:
	{
		auto rd = (instr >> 11) & 31;
		auto rt = (instr >> 16) & 31;
		auto shift = (instr >> 6) & 31;
		auto rs = (instr >> 21) & 31;

		switch (instr & 63)
		{
#define FIXED_SHIFT_OP(asmop) \
	NOP_IF_RD_ZERO(); \
	DISASM(#asmop " %s, %s, %u", NAME(rd), NAME(rt), shift)

		case 000: // SLL
		{
			FIXED_SHIFT_OP(sll);
			break;
		}

		case 002: // SRL
		{
			FIXED_SHIFT_OP(srl);
			break;
		}

		case 003: // SRA
		{
			FIXED_SHIFT_OP(sra);
			break;
		}

#define VARIABLE_SHIFT_OP(asmop) \
	NOP_IF_RD_ZERO(); \
	DISASM(#asmop " %s, %s, %s", NAME(rd), NAME(rt), NAME(rs))

		case 004: // SLLV
		{
			VARIABLE_SHIFT_OP(sllv);
			break;
		}

		case 006: // SRLV
		{
			VARIABLE_SHIFT_OP(srlv);
			break;
		}

		case 007: // SRAV
		{
			VARIABLE_SHIFT_OP(srav);
			break;
		}

		// If the last instruction is also a branch instruction, we will need to do some funky handling
		// so make sure we save the old branch taken register.
		case 010: // JR
		{
			DISASM("jr %s", NAME(rs));
			break;
		}

		case 011: // JALR
		{
			DISASM("jalr %s, %s", NAME(rd), NAME(rs));
			break;
		}

		case 015: // BREAK
		{
			DISASM("break %u", (instr >> 6) & 0xfffff);
			break;
		}

#define THREE_REG_OP(asmop) \
	NOP_IF_RD_ZERO(); \
	DISASM(#asmop " %s, %s, %s", NAME(rd), NAME(rs), NAME(rt))

		case 040: // ADD
		{
			THREE_REG_OP(add);
			break;
		}
		case 041: // ADDU
		{
			THREE_REG_OP(addu);
			break;
		}

		case 042: // SUB
		{
			THREE_REG_OP(sub);
			break;
		}
		case 043: // SUBU
		{
			THREE_REG_OP(subu);
			break;
		}

		case 044: // AND
		{
			THREE_REG_OP(and);
			break;
		}

		case 045: // OR
		{
			THREE_REG_OP(or);
			break;
		}

		case 046: // XOR
		{
			THREE_REG_OP(xor);
			break;
		}

		case 047: // NOR
		{
			NOP_IF_RD_ZERO();
			DISASM("nor %s, %s, %s", NAME(rd), NAME(rs), NAME(rt));
			break;
		}

		case 052: // SLT
		{
			THREE_REG_OP(slt);
			break;
		}

		case 053: // SLTU
		{
			THREE_REG_OP(sltu);
			break;
		}

		default:
			break;
		}
		break;
	}

	case 001: // REGIMM
	{
		unsigned rt = (instr >> 16) & 31;

		switch (rt)
		{
		case 020: // BLTZAL
		{
			unsigned rs = (instr >> 21) & 31;
			uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
			DISASM("bltzal %s, 0x%03x", NAME(rs), target_pc);
			break;
		}

		case 000: // BLTZ
		{
			unsigned rs = (instr >> 21) & 31;
			uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
			DISASM("bltz %s, 0x%03x", NAME(rs), target_pc);
			break;
		}

		case 021: // BGEZAL
		{
			unsigned rs = (instr >> 21) & 31;
			uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
			DISASM("bgezal %s, 0x%03x", NAME(rs), target_pc);
			break;
		}

		case 001: // BGEZ
		{
			unsigned rs = (instr >> 21) & 31;
			uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
			DISASM("bgez %s, 0x%03x", NAME(rs), target_pc);
			break;
		}

		default:
			break;
		}
		break;
	}

	case 003: // JAL
	{
		uint32_t target_pc = (instr & 0x3ffu) << 2;
		DISASM("jal 0x%03x", target_pc);
		break;
	}

	case 002: // J
	{
		uint32_t target_pc = (instr & 0x3ffu) << 2;
		DISASM("j 0x%03x", target_pc);
		break;
	}

	case 004: // BEQ
	{
		unsigned rs = (instr >> 21) & 31;
		unsigned rt = (instr >> 16) & 31;
		uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
		DISASM("beq %s, %s, 0x%03x", NAME(rs), NAME(rt), target_pc);
		break;
	}

	case 005: // BNE
	{
		unsigned rs = (instr >> 21) & 31;
		unsigned rt = (instr >> 16) & 31;
		uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
		DISASM("bne %s, %s, 0x%03x", NAME(rs), NAME(rt), target_pc);
		break;
	}

	case 006: // BLEZ
	{
		unsigned rs = (instr >> 21) & 31;
		uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
		DISASM("blez %s, 0x%03x", NAME(rs), target_pc);
		break;
	}

	case 007: // BGTZ
	{
		unsigned rs = (instr >> 21) & 31;
		uint32_t target_pc = (pc + 4 + (instr << 2)) & 0xffc;
		DISASM("bgtz %s, 0x%03x", NAME(rs), target_pc);
		break;
	}

#define TWO_REG_IMM_OP(asmop, immtype) \
	unsigned rt = (instr >> 16) & 31; \
	NOP_IF_RT_ZERO(); \
	unsigned rs = (instr >> 21) & 31; \
	DISASM(#asmop " %s, %s, %d", NAME(rt), NAME(rs), immtype(instr))

	case 010: // ADDI
	{
		TWO_REG_IMM_OP(addi, int16_t);
		break;
	}
	case 011:
	{
		TWO_REG_IMM_OP(addiu, int16_t);
		break;
	}

	case 012: // SLTI
	{
		TWO_REG_IMM_OP(slti, int16_t);
		break;
	}

	case 013: // SLTIU
	{
		TWO_REG_IMM_OP(sltiu, int16_t);
		break;
	}

	case 014: // ANDI
	{
		TWO_REG_IMM_OP(andi, uint16_t);
		break;
	}

	case 015: // ORI
	{
		TWO_REG_IMM_OP(ori, uint16_t);
		break;
	}

	case 016: // XORI
	{
		TWO_REG_IMM_OP(xori, uint16_t);
		break;
	}

	case 017: // LUI
	{
		unsigned rt = (instr >> 16) & 31;
		NOP_IF_RT_ZERO();
		int16_t imm = int16_t(instr);
		DISASM("lui %s, %d", NAME(rt), imm);
		break;
	}

	case 020: // COP0
	{
		unsigned rd = (instr >> 11) & 31;
		unsigned rs = (instr >> 21) & 31;
		unsigned rt = (instr >> 16) & 31;
		static const char *cp0_names[16] = {
			"SP_MEM_ADDR", "SP_DRAM_ADDR", "SP_RD_LEN", "SP_WR_LEN",
			"SP_STATUS", "SP_DMA_FULL", "SP_DMA_BUSY", "SP_SEMAPHORE",
			"DPC_START", "DPC_END", "DPC_CURRENT", "DPC_STATUS",
			"DPC_CLOCK", "DPC_BUFBUSY", "DPC_PIPEBUSY", "DPC_TMEM"
		};

		switch (rs)
		{
		case 000: // MFC0
		{
			DISASM("mfc0 %s, %s", NAME(rt), cp0_names[rd & 15]);
			break;
		}

		case 004: // MTC0
		{
			DISASM("mtc0 %s, %s", NAME(rt), cp0_names[rd & 15]);
			break;
		}

		default:
			break;
		}
		break;
	}

	case 022: // COP2
	{
		unsigned rd = (instr >> 11) & 31;
		unsigned rs = (instr >> 21) & 31;
		unsigned rt = (instr >> 16) & 31;
		unsigned imm = (instr >> 7) & 15;

		switch (rs)
		{
		case 000: // MFC2
		{
			DISASM("mfc2 %s, v%u[%u]", NAME(rt), rd, imm);
			break;
		}

		case 002: // CFC2
		{
			DISASM("cfc2 %s, $%u", NAME(rt), rd);
			break;
		}

		case 004: // MTC2
		{
			DISASM("mtc2 %s, v%u[%u]", NAME(rt), rd, imm);
			break;
		}

		case 006: // CTC2
		{
			DISASM("ctc2 %s, $%u", NAME(rt), rd);
			break;
		}

		default:
			break;
		}
		break;
	}

	case 040: // LB
	{
		unsigned rt = (instr >> 16) & 31;
		if (rt == 0)
		{
			DISASM_NOP();
			break;
		}

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("lb %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 041: // LH
	{
		unsigned rt = (instr >> 16) & 31;
		if (rt == 0)
		{
			DISASM_NOP();
			break;
		}

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("lh %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 043: // LW
	{
		unsigned rt = (instr >> 16) & 31;
		if (rt == 0)
		{
			DISASM_NOP();
			break;
		}

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("lw %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 044: // LBU
	{
		unsigned rt = (instr >> 16) & 31;
		if (rt == 0)
		{
			DISASM_NOP();
			break;
		}

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("lbu %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 045: // LHU
	{
		unsigned rt = (instr >> 16) & 31;
		if (rt == 0)
		{
			DISASM_NOP();
			break;
		}

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("lhu %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 050: // SB
	{
		unsigned rt = (instr >> 16) & 31;

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("sb %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 051: // SH
	{
		unsigned rt = (instr >> 16) & 31;

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("sh %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 053: // SW
	{
		unsigned rt = (instr >> 16) & 31;

		int16_t simm = int16_t(instr);
		unsigned rs = (instr >> 21) & 31;
		DISASM("sw %s, %d(%s)", NAME(rt), simm, NAME(rs));
		break;
	}

	case 062: // LWC2
	{
		unsigned rt = (instr >> 16) & 31;
		int simm = int(instr & 127);
		if (simm & 64) simm -= 128;
		unsigned rs = (instr >> 21) & 31;
		unsigned rd = (instr >> 11) & 31;
		unsigned imm = (instr >> 7) & 15;

		static const char *lwc2_ops[32] = {
			"LBV", "LSV", "LLV", "LDV", "LQV", "LRV", "LPV", "LUV", "LHV", nullptr, nullptr, "LTV",
		};

		const char *op = lwc2_ops[rd];
		static const unsigned shift[12] = {0,1,2,3,4,4,3,3,4,4,4,4};
		if (op)
			DISASM("%s v%u, %u, %d(%s)", op, rt, imm, simm * (1 << shift[rd]), NAME(rs));
		else
			DISASM_NOP();
		break;
	}

	case 072: // SWC2
	{
		unsigned rt = (instr >> 16) & 31;
		int simm = int(instr & 127);
		if (simm & 64) simm -= 128;
		unsigned rs = (instr >> 21) & 31;
		unsigned rd = (instr >> 11) & 31;
		unsigned imm = (instr >> 7) & 15;

		static const char *swc2_ops[32] = {
			"SBV", "SSV", "SLV", "SDV", "SQV", "SRV", "SPV", "SUV", "SHV", "SFV", nullptr, "STV",
		};

		const char *op = swc2_ops[rd];
		static const unsigned shift[12] = {0,1,2,3,4,4,3,3,4,4,4,4};
		if (op)
			DISASM("%s v%u, %u, %d(%s)", op, rt, imm, simm * (1 << shift[rd]), NAME(rs));
		else
			DISASM_NOP();

		break;
	}

	default:
		break;
	}

	return "???";
}
}
