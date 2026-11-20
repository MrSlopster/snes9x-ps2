/* Exact idle-loop skipping for the 65816 (PS2 port speed-up).
 *
 * SNES games spin in tiny polling loops (LDA $xx / BEQ -4, BRA -2, ...)
 * waiting for the NMI/IRQ handler to change RAM. Emulating every
 * iteration is pure EE time. Snes9x 1.53 had an approximate "shutdown"
 * hack for this; this version is exact:
 *
 *  - Called on a taken, short, backward branch (hook in bOP, cpumacro.h).
 *  - The loop body may only contain loads/compares/BIT/logic ops whose
 *    operands are immediate or plain memory (Memory.Map pointers: RAM /
 *    ROM), no writes, no I/O registers (so no APU ports, no $4212 ...),
 *    and no SA-1 / SuperFX (they share RAM with the CPU).
 *  - Two consecutive passes with identical registers, flags and OpenBus,
 *    inside the same event window, give the exact iteration cost c.
 *  - Whole iterations are then skipped: Cycles += k * c, stopping at least
 *    one iteration before the next H-event, the IRQ timer and NMI. Until
 *    then nothing can change what the loop reads, and every iteration
 *    leaves the same state, so the result is identical to executing them.
 */

#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "apu.h"

#include "ps2_idle.h"

int ps2_idle_enabled = 1;
uint32 ps2_idle_skipped_cycles = 0; /* master cycles, reset by the profiler */
uint32 ps2_idle_plain_skips = 0;     /* fast-forwards of plain polling loops */
uint32 ps2_idle_handshake_skips = 0; /* fast-forwards of APU-port loops */
uint32 ps2_idle_handshake_passes = 0; /* loop passes skipped by those */

static bool peek(uint32 address, uint8 *out)
{
	uint8 *p = Memory.Map[(address & 0xffffff) >> MEMMAP_SHIFT];
	if (p < (uint8 *)CMemory::MAP_LAST)
		return false; /* I/O, SRAM handlers, coprocessors: not plain memory */
	*out = p[address & 0xffff];
	return true;
}

static bool direct(uint32 address)
{
	return Memory.Map[(address & 0xffffff) >> MEMMAP_SHIFT] >= (uint8 *)CMemory::MAP_LAST;
}

/* $2140-$217f in a bank that maps the PPU/APU registers: an APU port. */
static bool apu_port(uint32 address)
{
	return Memory.Map[(address & 0xffffff) >> MEMMAP_SHIFT] == (uint8 *)CMemory::MAP_PPU &&
	       (address & 0xffc0) == 0x2140;
}

/* Operand is plain memory, or (counted) an APU port read. */
static bool operand_ok(uint32 address, int *port_reads)
{
	if (direct(address))
		return true;
	if (apu_port(address))
	{
		(*port_reads)++;
		return true;
	}
	return false;
}

enum Mode
{
	IMM,
	DP,
	ABS,
	LONG,
	IMPL
};

/* Side-effect-free opcodes allowed in a loop body. */
static int body_mode(uint8 op)
{
	switch (op)
	{
	case 0xA9: case 0xA2: case 0xA0: case 0xC9: case 0xE0: case 0xC0:
	case 0x89: case 0x29: case 0x09: case 0x49:
		return IMM;
	case 0xA5: case 0xA6: case 0xA4: case 0xC5: case 0xE4: case 0xC4:
	case 0x24: case 0x25: case 0x05: case 0x45:
		return DP;
	case 0xAD: case 0xAE: case 0xAC: case 0xCD: case 0xEC: case 0xCC:
	case 0x2C: case 0x2D: case 0x0D: case 0x4D:
		return ABS;
	case 0xAF: case 0xCF: case 0x2F: case 0x0F: case 0x4F:
		return LONG;
	case 0xEA: /* NOP */
		return IMPL;
	default:
		return -1;
	}
}

enum
{
	LOOP_IMPURE = 2,
	LOOP_PURE = 1,      /* reads only plain memory */
	LOOP_HANDSHAKE = 3  /* pure, plus exactly one APU port read per pass */
};

/* Body = [target, branch_start) in bank PB. */
static int classify_body(uint16 target, uint16 branch_start)
{
	int port_reads = 0;
	uint32 pb = ICPU.ShiftedPB;
	uint16 pos = target;

	while (pos != branch_start)
	{
		uint8 op, b1, b2, b3;
		if (!peek(pb | pos, &op))
			return LOOP_IMPURE;
		int mode = body_mode(op);
		if (mode < 0)
			return LOOP_IMPURE;
		uint8 len = ICPU.S9xOpLengths[op];
		if ((uint16)(branch_start - pos) < len)
			return LOOP_IMPURE;

		switch (mode)
		{
		case DP:
			if (!peek(pb | (uint16)(pos + 1), &b1) || !direct((Registers.D.W + b1) & 0xffff))
				return LOOP_IMPURE;
			break;
		case ABS:
			if (!peek(pb | (uint16)(pos + 1), &b1) || !peek(pb | (uint16)(pos + 2), &b2) ||
			    !operand_ok(ICPU.ShiftedDB | (b1 | (b2 << 8)), &port_reads))
				return LOOP_IMPURE;
			break;
		case LONG:
			if (!peek(pb | (uint16)(pos + 1), &b1) || !peek(pb | (uint16)(pos + 2), &b2) ||
			    !peek(pb | (uint16)(pos + 3), &b3) || !operand_ok(b1 | (b2 << 8) | (b3 << 16), &port_reads))
				return LOOP_IMPURE;
			break;
		default:
			break;
		}
		pos += len;
	}
	if (port_reads == 0)
		return LOOP_PURE;
	return port_reads == 1 ? LOOP_HANDSHAKE : LOOP_IMPURE;
}

/* Purity verdicts, direct-mapped by loop address; operand addresses
 * depend on D and DB, so they are part of the key. */
struct PurityEntry
{
	uint32 pbpc;
	uint16 d;
	uint8 db;
	uint8 state; /* 0 empty, else LOOP_* */
};
static PurityEntry purity_cache[256];

static int loop_kind(uint16 target, uint16 after_branch)
{
	uint32 pbpc = Registers.PBPC;
	PurityEntry &e = purity_cache[(pbpc ^ (pbpc >> 8)) & 255];
	if (e.state && e.pbpc == pbpc && e.d == Registers.D.W && e.db == Registers.DB)
		return e.state;

	int kind = classify_body(target, (uint16)(after_branch - 2));
	e.pbpc = pbpc;
	e.d = Registers.D.W;
	e.db = Registers.DB;
	e.state = (uint8)kind;
	return kind;
}

struct LoopState
{
	uint32 pbpc;
	int32 cycles;
	int32 next_event;
	uint8 which_event;
	uint16 a, x, y, d, s, p;
	uint8 db, carry, zero, negative, overflow, open_bus;
	bool valid;
};

static LoopState last;

static void snapshot(LoopState &s)
{
	s.pbpc = Registers.PBPC;
	s.cycles = CPU.Cycles;
	s.next_event = CPU.NextEvent;
	s.which_event = CPU.WhichEvent;
	s.a = Registers.A.W;
	s.x = Registers.X.W;
	s.y = Registers.Y.W;
	s.d = Registers.D.W;
	s.s = Registers.S.W;
	s.p = Registers.P.W;
	s.db = Registers.DB;
	s.carry = ICPU._Carry;
	s.zero = ICPU._Zero;
	s.negative = ICPU._Negative;
	s.overflow = ICPU._Overflow;
	s.open_bus = OpenBus;
}

static bool same_state(const LoopState &a, const LoopState &b)
{
	return a.pbpc == b.pbpc && a.next_event == b.next_event && a.which_event == b.which_event &&
	       a.a == b.a && a.x == b.x && a.y == b.y && a.d == b.d && a.s == b.s && a.p == b.p &&
	       a.db == b.db && a.carry == b.carry && a.zero == b.zero && a.negative == b.negative &&
	       a.overflow == b.overflow && a.open_bus == b.open_bus;
}

void S9xIdleLoopCheck(uint16 target, uint16 after_branch)
{
	if (!ps2_idle_enabled || Settings.SA1 || Settings.SuperFX)
		return;

	/* Anything pending that the main loop would act on: don't touch. */
	if (CPU.NMIPending || CPU.IRQLine || CPU.IRQExternal || CPU.WaitingForInterrupt ||
	    Timings.IRQFlagChanging || (CPU.Flags & SCAN_KEYS_FLAG))
	{
		last.valid = false;
		return;
	}

	LoopState now;
	snapshot(now);

	/* Decode the body only after two identical passes (loops whose
	 * registers change, e.g. counters, never get this far), and only
	 * once per loop thanks to the purity cache. */
	if (last.valid && same_state(last, now))
	{
		int kind = loop_kind(target, after_branch);
		if (kind != LOOP_IMPURE)
		{
			int32 c = now.cycles - last.cycles;
			if (c > 0 && c <= 512)
			{
				int32 limit = CPU.NextEvent < Timings.NextIRQTimer ? CPU.NextEvent : Timings.NextIRQTimer;
				int32 k = (limit - CPU.Cycles) / c - 1;
				/* Handshake loops: only as far as the SPC700 leaves the
				 * port unchanged (runs it ahead; see apu.cpp). */
				if (k > 0 && kind == LOOP_HANDSHAKE)
					k = CPU.InDMAorHDMA ? 0 : S9xAPUHandshakeSkip(c, k);
				if (k > 0)
				{
					if (kind == LOOP_HANDSHAKE)
						ps2_idle_handshake_skips++, ps2_idle_handshake_passes += k;
					else
						ps2_idle_plain_skips++;
					CPU.Cycles += k * c;
					ps2_idle_skipped_cycles += (uint32)(k * c);
					now.cycles = CPU.Cycles;
				}
			}
		}
	}

	now.valid = true;
	last = now;
}
