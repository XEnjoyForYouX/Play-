#include <stdio.h>
#include "MIPSAnalysis.h"
#include "MIPS.h"

CMIPSAnalysis::CMIPSAnalysis(CMIPS* ctx)
    : m_ctx(ctx)
{
}

void CMIPSAnalysis::Clear()
{
	m_subroutines.clear();
}

void CMIPSAnalysis::Analyse(uint32 start, uint32 end, uint32 entryPoint)
{
	AnalyseSubroutines(start, end, entryPoint);
	AnalyseStringReferences();
}

void CMIPSAnalysis::InsertSubroutine(uint32 start, uint32 end, uint32 stackAllocStart, uint32 stackAllocEnd, uint32 stackSize, uint32 returnAddrPos)
{
	assert(FindSubroutine(start) == nullptr);
	assert(FindSubroutine(end) == nullptr);

	SUBROUTINE subroutine;
	subroutine.start = start;
	subroutine.end = end;
	subroutine.stackAllocStart = stackAllocStart;
	subroutine.stackAllocEnd = stackAllocEnd;
	subroutine.stackSize = stackSize;
	subroutine.returnAddrPos = returnAddrPos;

	m_subroutines.insert(std::make_pair(start, subroutine));
}

const CMIPSAnalysis::SUBROUTINE* CMIPSAnalysis::FindSubroutine(uint32 address) const
{
	auto subroutineIterator = m_subroutines.lower_bound(address);
	if(subroutineIterator == std::end(m_subroutines)) return nullptr;

	auto& subroutine = subroutineIterator->second;
	if(address >= subroutine.start && address <= subroutine.end)
	{
		return &subroutine;
	}
	else
	{
		return nullptr;
	}
}

void CMIPSAnalysis::ChangeSubroutineStart(uint32 currStart, uint32 newStart)
{
	auto subroutineIterator = m_subroutines.find(currStart);
	assert(subroutineIterator != std::end(m_subroutines));

	SUBROUTINE subroutine(subroutineIterator->second);
	subroutine.start = newStart;

	m_subroutines.erase(subroutineIterator);
	m_subroutines.insert(SubroutineList::value_type(newStart, subroutine));
}

void CMIPSAnalysis::ChangeSubroutineEnd(uint32 start, uint32 newEnd)
{
	assert(start < newEnd);

	auto subroutineIterator = m_subroutines.find(start);
	assert(subroutineIterator != std::end(m_subroutines));

	auto& subroutine(subroutineIterator->second);
	subroutine.end = newEnd;
}

uint32 CMIPSAnalysis::GetInstruction(uint32 address) const
{
	uint32 physAddr = m_ctx->m_pAddrTranslator(m_ctx, address);
	return m_ctx->m_pMemoryMap->GetInstruction(physAddr);
}

void CMIPSAnalysis::AnalyseSubroutines(uint32 start, uint32 end, uint32 entryPoint)
{
	start &= ~0x3;
	end &= ~0x3;

	auto subroutinesBefore = m_subroutines.size();

	FindSubroutinesByStackAllocation(start, end);
	FindSubroutinesByJumpTargets(start, end, entryPoint);
	ExpandSubroutines(start, end);

	uint32 subroutinesAdded = static_cast<uint32>(m_subroutines.size() - subroutinesBefore);
	printf("CMIPSAnalysis: Found %d subroutines in the range [0x%08X, 0x%08X].\r\n", subroutinesAdded, start, end);
}

static bool IsStackFreeingInstruction(uint32 opcode)
{
	return (opcode & 0xFFFF0000) == 0x27BD0000;
}

void CMIPSAnalysis::FindSubroutinesByStackAllocation(uint32 start, uint32 end)
{
	uint32 candidate = start;
	while(candidate != end)
	{
		uint32 returnAddr = 0;
		uint32 opcode = GetInstruction(candidate);
		if((opcode & 0xFFFF0000) == 0x27BD0000)
		{
			//Found the head of a routine (stack allocation)
			uint32 stackAmount = 0 - (int16)(opcode & 0xFFFF);
			//Look for a JR RA
			uint32 tempAddr = candidate;
			while(tempAddr != end)
			{
				opcode = GetInstruction(tempAddr);

				//Check SW/SD/SQ RA, 0x????(SP)
				if(
				    ((opcode & 0xFFFF0000) == 0xAFBF0000) || //SW
				    ((opcode & 0xFFFF0000) == 0xFFBF0000) || //SD
				    ((opcode & 0xFFFF0000) == 0x7FBF0000))   //SQ
				{
					returnAddr = (opcode & 0xFFFF);
				}

				//Check for JR RA or J
				if((opcode == 0x03E00008) || ((opcode & 0xFC000000) == 0x08000000))
				{
					//Check if there's a stack unwinding instruction above or below

					//Check above
					//ADDIU SP, SP, 0x????
					//JR RA

					opcode = GetInstruction(tempAddr - 4);
					if(IsStackFreeingInstruction(opcode))
					{
						if(stackAmount == (int16)(opcode & 0xFFFF))
						{
							//That's good...
							InsertSubroutine(candidate, tempAddr + 4, candidate, tempAddr - 4, stackAmount, returnAddr);
							candidate = tempAddr + 4;
							break;
						}
					}

					//Check below
					//JR RA
					//ADDIU SP, SP, 0x????

					opcode = GetInstruction(tempAddr + 4);
					if(IsStackFreeingInstruction(opcode))
					{
						if(stackAmount == (int16)(opcode & 0xFFFF))
						{
							//That's good
							InsertSubroutine(candidate, tempAddr + 4, candidate, tempAddr + 4, stackAmount, returnAddr);
							candidate = tempAddr + 4;
						}
						break;
					}
					//No stack unwinding was found... just forget about this one
					//break;
				}
				tempAddr += 4;
			}
		}
		candidate += 4;
	}
}

void CMIPSAnalysis::FindSubroutinesByJumpTargets(uint32 start, uint32 end, uint32 entryPoint)
{
	//Second pass : Search for all JAL targets then scan for functions
	std::set<uint32> subroutineAddresses;
	for(uint32 address = start; address <= end; address += 4)
	{
		uint32 opcode = GetInstruction(address);
		if(
		    (opcode & 0xFC000000) == 0x0C000000 ||
		    (opcode & 0xFC000000) == 0x08000000)
		{
			uint32 addrTopBits = address & 0xF0000000;
			uint32 jumpTarget = addrTopBits | ((opcode & 0x03FFFFFF) * 4);
			if(jumpTarget < start) continue;
			if(jumpTarget >= end) continue;
			subroutineAddresses.insert(jumpTarget);
		}
	}

	if(entryPoint != -1)
	{
		subroutineAddresses.insert(entryPoint);
	}

	static const uint32 searchLimit = 0x1000;

	for(const auto& subroutineAddress : subroutineAddresses)
	{
		if(subroutineAddress == 0) continue;

		//Don't bother if we already found it
		if(FindSubroutine(subroutineAddress)) continue;

		//Otherwise, try to find a function that already exists
		for(uint32 address = subroutineAddress; address <= end; address += 4)
		{
			if(address >= (subroutineAddress + searchLimit)) break;

			uint32 opcode = GetInstruction(address);

			//Check for JR RA or J
			if((opcode == 0x03E00008) || ((opcode & 0xFC000000) == 0x08000000))
			{
				InsertSubroutine(subroutineAddress, address + 4, 0, 0, 0, 0);
				break;
			}

			auto subroutine = FindSubroutine(address);
			if(subroutine)
			{
				//Function already exists, merge.
				ChangeSubroutineStart(subroutine->start, subroutineAddress);
				break;
			}
		}
	}
}

void CMIPSAnalysis::ExpandSubroutines(uint32 executableStart, uint32 executableEnd)
{
	static const uint32 searchLimit = 0x1000;

	const auto& findFreeSubroutineEnd =
	    [this](uint32 begin, uint32 end) -> uint32 {
		for(uint32 address = begin; address <= begin + searchLimit; address += 4)
		{
			if(FindSubroutine(address) != nullptr) return MIPS_INVALID_PC;

			uint32 opcode = GetInstruction(address);

			//Check for JR RA or J
			if((opcode == 0x03E00008) || ((opcode & 0xFC000000) == 0x08000000))
			{
				//+4 for delay slot
				return address + 4;
			}

			//Check for BEQ R0, R0, $label
			if((opcode & 0xFFFF0000) == 0x10000000)
			{
				//+4 for delay slot
				return address + 4;
			}
		}

		return MIPS_INVALID_PC;
	};

	for(auto& subroutinePair : m_subroutines)
	{
		auto& subroutine = subroutinePair.second;

		//Don't bother if subroutine is not in our range
		if(subroutine.start < executableStart) continue;
		if(subroutine.end > executableEnd) continue;

		//Search for branch targets that fall in space not allocated for a subroutine
		for(uint32 address = subroutine.start; address <= subroutine.end; address += 4)
		{
			uint32 opcode = GetInstruction(address);

			auto branchType = m_ctx->m_pArch->IsInstructionBranch(m_ctx, address, opcode);
			if(branchType != MIPS_BRANCH_NORMAL) continue;

			uint32 branchTarget = m_ctx->m_pArch->GetInstructionEffectiveAddress(m_ctx, address, opcode);
			if(branchTarget == MIPS_INVALID_PC) continue;

			//Check if pointing inside our subroutine. If so, don't bother
			if(branchTarget >= subroutine.start && branchTarget <= subroutine.end) continue;

			//Branch could be out of subroutine range, but that would be weird and we don't want to handle that
			if(branchTarget < subroutine.start) continue;

			//Check if branch is outside our search limit
			if(branchTarget > (subroutine.end + searchLimit)) continue;

			//Doesn't make sense if target is outside range
			if(branchTarget >= executableEnd) continue;

			//If there's already a subroutine there, don't bother
			if(FindSubroutine(branchTarget) != nullptr) continue;

			uint32 routineEnd = findFreeSubroutineEnd(branchTarget, executableEnd);
			if(routineEnd == MIPS_INVALID_PC)
			{
				continue;
			}

			//Check invariant
			assert(FindSubroutine(routineEnd) == nullptr);

			//Check if we need to update stackAllocEnd
			uint32 endOpcode = GetInstruction(routineEnd);

			if(IsStackFreeingInstruction(endOpcode))
			{
				uint16 stackAmount = static_cast<int16>(endOpcode & 0xFFFF);
				if(stackAmount == subroutine.stackSize)
				{
					subroutine.stackAllocEnd = std::max<uint32>(subroutine.stackAllocEnd, routineEnd);
				}
			}

			subroutine.end = std::max<uint32>(subroutine.end, routineEnd);
		}
	}
}

bool CMIPSAnalysis::TryGetStringAtAddress(CMIPS* context, uint32 address, std::string& result)
{
	result.clear();
	uint8 byteBefore = context->m_pMemoryMap->GetByte(address - 1);
	if(byteBefore != 0) return false;
	while(1)
	{
		uint8 byte = context->m_pMemoryMap->GetByte(address);
		if(byte == 0) break;
		if(byte > 0x7F) return false;
		if((byte < 0x20) &&
		   (byte != '\t') &&
		   (byte != '\n') &&
		   (byte != '\r'))
		{
			return false;
		}
		result += byte;
		address++;
	}
	return (result.length() > 1);
}

bool CMIPSAnalysis::TryGetSJISLatinStringAtAddress(CMIPS* context, uint32 address, std::string& result)
{
	enum DECODE_STATE
	{
		DECODE_STATE_NORMAL,
		DECODE_STATE_81,
		DECODE_STATE_82,
	};

	DECODE_STATE state = DECODE_STATE_NORMAL;

	result.clear();

	while(1)
	{
		uint8 byte = context->m_pMemoryMap->GetByte(address++);
		if(byte == 0) break;
		switch(state)
		{
		case DECODE_STATE_NORMAL:
			if(byte == 0x81)
			{
				state = DECODE_STATE_81;
			}
			else if(byte == 0x82)
			{
				state = DECODE_STATE_82;
			}
			else if(
			    ((byte >= 0x20) && (byte < 0x80)) ||
			    (byte == '\t') ||
			    (byte == '\n') ||
			    (byte == '\r'))
			{
				result += byte;
				state = DECODE_STATE_NORMAL;
			}
			else
			{
				return false;
			}
			break;
		case DECODE_STATE_81:
			if(byte == 0x40)
			{
				result += ' ';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte == 0x44)
			{
				result += '.';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte == 0x46)
			{
				result += ':';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte == 0x5E)
			{
				result += '/';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte == 0x69)
			{
				result += '(';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte == 0x6A)
			{
				result += ')';
				state = DECODE_STATE_NORMAL;
			}
			else
			{
				return false;
			}
			break;
		case DECODE_STATE_82:
			if(byte >= 0x4F && byte < 0x59)
			{
				result += (byte - 0x4F) + '0';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte >= 0x60 && byte < 0x7A)
			{
				result += (byte - 0x60) + 'A';
				state = DECODE_STATE_NORMAL;
			}
			else if(byte >= 0x81 && byte < 0x9B)
			{
				result += (byte - 0x81) + 'a';
				state = DECODE_STATE_NORMAL;
			}
			else
			{
				return false;
			}
			break;
		}
	}
	return (result.length() > 1);
}

void CMIPSAnalysis::AnalyseStringReferences()
{
	bool commentInserted = false;
	for(const auto& subroutinePair : m_subroutines)
	{
		const auto& subroutine = subroutinePair.second;
		uint32 registerValue[0x20] = {0};
		bool registerWritten[0x20] = {false};
		for(uint32 address = subroutine.start; address <= subroutine.end; address += 4)
		{
			uint32 op = GetInstruction(address);

			//LUI
			if((op & 0xFC000000) == 0x3C000000)
			{
				uint32 rt = (op >> 16) & 0x1F;
				uint32 imm = static_cast<int16>(op);
				registerWritten[rt] = true;
				registerValue[rt] = imm << 16;
			}
			//ADDIU
			else if((op & 0xFC000000) == 0x24000000)
			{
				uint32 rs = (op >> 21) & 0x1F;
				uint32 imm = static_cast<int16>(op);
				if(registerWritten[rs])
				{
					//Check string
					uint32 targetAddress = registerValue[rs] + imm;
					registerWritten[rs] = false;

					std::string stringConstant;
					if(TryGetStringAtAddress(m_ctx, targetAddress, stringConstant) ||
					   TryGetSJISLatinStringAtAddress(m_ctx, targetAddress, stringConstant))
					{
						if(!m_ctx->m_Comments.Find(address))
						{
							m_ctx->m_Comments.InsertTag(address, std::move(stringConstant));
							commentInserted = true;
						}
					}
				}
			}
		}
	}
	if(commentInserted)
	{
		m_ctx->m_Comments.OnTagListChange();
	}
}

static bool IsValidProgramAddress(uint32 address)
{
	return (address != 0) && ((address & 0x03) == 0);
}

CMIPSAnalysis::CallStackItemArray CMIPSAnalysis::GetCallStack(CMIPS* context, uint32 pc, uint32 sp, uint32 ra)
{
	uint32 physicalSp = context->m_pAddrTranslator(context, sp);

	CallStackItemArray result;

	{
		auto routine = context->m_analysis->FindSubroutine(pc);
		if(!routine)
		{
			if(IsValidProgramAddress(pc)) result.push_back(pc);
			if(pc != ra)
			{
				if(IsValidProgramAddress(ra)) result.push_back(ra);
			}
			return result;
		}

		//We need to get to a state where we're ready to dig into the previous function's
		//stack

		//Check if we need to check into the stack to get the RA
		if(context->m_analysis->FindSubroutine(ra) == routine)
		{
			ra = context->m_pMemoryMap->GetWord(physicalSp + routine->returnAddrPos);
			physicalSp += routine->stackSize;
		}
		else
		{
			//We haven't called a sub routine yet... The RA is good, but we
			//don't know wether stack memory has been allocated or not

			//ADDIU SP, SP, 0x????
			//If the PC is after this instruction, then, we've allocated stack

			if(pc > routine->stackAllocStart)
			{
				if(pc <= routine->stackAllocEnd)
				{
					physicalSp += routine->stackSize;
				}
			}
		}
	}

	while(1)
	{
		//Add the current function
		result.push_back(pc);

		//Go to previous routine
		pc = ra;

		//Check if we can go on...
		auto routine = context->m_analysis->FindSubroutine(pc);
		if(!routine)
		{
			if(IsValidProgramAddress(ra)) result.push_back(ra);
			break;
		}

		//Get the next RA
		ra = context->m_pMemoryMap->GetWord(physicalSp + routine->returnAddrPos);
		physicalSp += routine->stackSize;

		if((pc == ra) && (routine->stackSize == 0))
		{
			if(IsValidProgramAddress(ra)) result.push_back(ra);
			break;
		}
	}

	return result;
}
