/* Scratchpad RAM (0x70000000, 16 KB): not part of the EE's 8 KB data
 * cache, never misses. Layout:
 *   0x0000  Registers, ICPU, CPU, Timings, OpenBus (linker symbols, see
 *           the Makefile; zeroed in ps2_main.cpp)
 *   0x0100  the 65816's five opcode tables, 0x400 each (cpuexec.h)
 *   0x1500  its four opcode length tables, 0x100 each
 *   0x1900  free */

#define EE_SPAD_TABLES_ORIGINALS
#include <string.h>
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"

#include "ps2_spad.h"

void ps2_spad_init(void)
{
#ifdef _MIPS_ARCH_R5900
	static_assert(sizeof(S9xOpcodesE1) == 0x400, "opcode table size");
	uint8 *base = (uint8 *)EE_SPAD_TABLES;
	memcpy(base + 0x0000, S9xOpcodesE1, 0x400);
	memcpy(base + 0x0400, S9xOpcodesM1X1, 0x400);
	memcpy(base + 0x0800, S9xOpcodesM1X0, 0x400);
	memcpy(base + 0x0c00, S9xOpcodesM0X1, 0x400);
	memcpy(base + 0x1000, S9xOpcodesM0X0, 0x400);
	memcpy(base + 0x1400, S9xOpLengthsM1X1, 0x100);
	memcpy(base + 0x1500, S9xOpLengthsM1X0, 0x100);
	memcpy(base + 0x1600, S9xOpLengthsM0X1, 0x100);
	memcpy(base + 0x1700, S9xOpLengthsM0X0, 0x100);
#endif
}
