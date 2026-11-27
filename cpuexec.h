/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _CPUEXEC_H_
#define _CPUEXEC_H_

#include "ppu.h"
#ifdef DEBUGGER
#include "debug.h"
#endif

struct SOpcodes
{
	void (*S9xOpcode) (void);
};

struct SICPU
{
	struct SOpcodes	*S9xOpcodes;
	uint8	*S9xOpLengths;
	uint8	_Carry;
	uint8	_Zero;
	uint8	_Negative;
	uint8	_Overflow;
	uint32	ShiftedPB;
	uint32	ShiftedDB;
	uint32	Frame;
	uint32	FrameAdvanceCount;
};

extern struct SICPU		ICPU;

extern struct SOpcodes	S9xOpcodesE1[256];
extern struct SOpcodes	S9xOpcodesM1X1[256];
extern struct SOpcodes	S9xOpcodesM1X0[256];
extern struct SOpcodes	S9xOpcodesM0X1[256];
extern struct SOpcodes	S9xOpcodesM0X0[256];
extern struct SOpcodes	S9xOpcodesSlow[256];
extern uint8			S9xOpLengthsM1X1[256];
extern uint8			S9xOpLengthsM1X0[256];
extern uint8			S9xOpLengthsM0X1[256];
extern uint8			S9xOpLengthsM0X0[256];

#ifdef _MIPS_ARCH_R5900
/* PS2: every emulated instruction goes through the current opcode table
 * and length table. Copies of them live in the EE's scratchpad RAM, which
 * is not part of the 8 KB data cache and never misses (ps2/ps2_spad.cpp
 * fills them in at startup; layout there). The names below then mean the
 * copies everywhere except where the tables are defined. */
#define EE_SPAD_TABLES 0x70000100
#ifndef EE_SPAD_TABLES_ORIGINALS
#define S9xOpcodesE1		((struct SOpcodes *) (EE_SPAD_TABLES + 0x0000))
#define S9xOpcodesM1X1		((struct SOpcodes *) (EE_SPAD_TABLES + 0x0400))
#define S9xOpcodesM1X0		((struct SOpcodes *) (EE_SPAD_TABLES + 0x0800))
#define S9xOpcodesM0X1		((struct SOpcodes *) (EE_SPAD_TABLES + 0x0c00))
#define S9xOpcodesM0X0		((struct SOpcodes *) (EE_SPAD_TABLES + 0x1000))
#define S9xOpLengthsM1X1	((uint8 *) (EE_SPAD_TABLES + 0x1400))
#define S9xOpLengthsM1X0	((uint8 *) (EE_SPAD_TABLES + 0x1500))
#define S9xOpLengthsM0X1	((uint8 *) (EE_SPAD_TABLES + 0x1600))
#define S9xOpLengthsM0X0	((uint8 *) (EE_SPAD_TABLES + 0x1700))
#endif
#endif

void S9xMainLoop (void);
void S9xReset (void);
void S9xSoftReset (void);
void S9xDoHEventProcessing (void);

static inline void S9xUnpackStatus (void)
{
	ICPU._Zero = (Registers.PL & Zero) == 0;
	ICPU._Negative = (Registers.PL & Negative);
	ICPU._Carry = (Registers.PL & Carry);
	ICPU._Overflow = (Registers.PL & Overflow) >> 6;
}

static inline void S9xPackStatus (void)
{
	Registers.PL &= ~(Zero | Negative | Carry | Overflow);
	Registers.PL |= ICPU._Carry | ((ICPU._Zero == 0) << 1) | (ICPU._Negative & 0x80) | (ICPU._Overflow << 6);
}

static inline void S9xFixCycles (void)
{
	if (CheckEmulation())
	{
		ICPU.S9xOpcodes = S9xOpcodesE1;
		ICPU.S9xOpLengths = S9xOpLengthsM1X1;
	}
	else
	if (CheckMemory())
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X1;
			ICPU.S9xOpLengths = S9xOpLengthsM1X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X0;
			ICPU.S9xOpLengths = S9xOpLengthsM1X0;
		}
	}
	else
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X1;
			ICPU.S9xOpLengths = S9xOpLengthsM0X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X0;
			ICPU.S9xOpLengths = S9xOpLengthsM0X0;
		}
	}
}

#endif
