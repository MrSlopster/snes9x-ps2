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
 *    "Consecutive" is not implied by the two snapshots being equal (the
 *    last pass of one frame and the first of the next look the same, with
 *    an NMI and a frame of work between them), so it is established
 *    separately: the body is decoded (straight-line code, no writes) and
 *    the hit before must have been reached through that same code - which
 *    is given for ROM, and for RAM means it was decoded there as well -
 *    without the chain being broken in between. The chain
 *    (S9xIdleLoopLive) is cut by the two ways out of such a body: its
 *    branch not being taken (cpumacro.h) and anything the main loop does
 *    between instructions - NMI, IRQ, ... (cpuexec.cpp).
 *  - Whole iterations are then skipped: Cycles += k * c, every one of them
 *    ending before the next H-event and the IRQ timer (NMI is raised by an
 *    H-event). Until then nothing can change what the loop reads, and
 *    every iteration leaves the same state, so the result is identical to
 *    executing them.
 *  - A loop proven this way is remembered with the registers, code and
 *    memory bytes the proof rests on; arriving at its branch again with
 *    all of those unchanged (after each H-event, typically) skips at once.
 */

#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "apu.h"
#include "ppu.h"

#include "ps2_idle.h"

int ps2_idle_enabled = 1;
uint32 ps2_idle_skipped_cycles = 0; /* master cycles, reset by the profiler */
uint32 ps2_idle_plain_skips = 0;     /* fast-forwards of plain polling loops */
uint32 ps2_idle_handshake_skips = 0; /* fast-forwards of APU-port loops */
uint32 ps2_idle_handshake_passes = 0; /* loop passes skipped by those */
uint32 ps2_idle_crossings = 0;        /* passes taken across an H-event without the interpreter */

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

/* Verdicts, direct-mapped by loop address. A verdict follows from the
 * code bytes, D and DB (operand addresses), the flags (operand sizes) and
 * the memory map, so the first three are part of the entry and the cache
 * is emptied when the map may have changed (S9xIdleLoopReset; BS-X
 * cartridges, which remap while running, are not cached). An "impure"
 * verdict is not checked against the code: a stale one only loses a skip,
 * and polling loops on I/O registers ask on every pass. */
struct KindEntry
{
	uint32 pbpc;
	uint16 d, p;
	uint8 db;
	uint8 kind; /* 0 empty, else LOOP_* */
	uint8 len;
	uint8 code[16];
};
static KindEntry kind_cache[256];

/* code: the len bytes from the branch target to the end of the branch.
 * (Code in ROM is what it was when the entry was made; only code in RAM
 * is compared.) */
static int loop_kind(uint16 target, uint16 after_branch, const uint8 *code, unsigned len, bool in_rom)
{
	uint32 pbpc = Registers.PBPC;
	KindEntry &e = kind_cache[(pbpc ^ (pbpc >> 8)) & 255];
	if (e.kind && e.pbpc == pbpc && e.d == Registers.D.W && e.db == Registers.DB &&
	    (e.kind == LOOP_IMPURE ||
	     (e.p == Registers.P.W && e.len == len && (in_rom || !memcmp(e.code, code, len)))))
		return e.kind;

	int kind = classify_body(target, (uint16)(after_branch - 2));
	if (!Settings.BS)
	{
		e.pbpc = pbpc;
		e.d = Registers.D.W;
		e.p = Registers.P.W;
		e.db = Registers.DB;
		e.kind = (uint8)kind;
		e.len = (uint8)len;
		memcpy(e.code, code, len);
	}
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
};

/* The previous hit of the hook: its state, and, if the loop body was
 * decoded there, the verdict and (for code in RAM) the code itself. */
static LoopState last;
static uint8 last_kind; /* 0: not decoded */
static uint8 last_len;
static uint8 last_code[16];

/* Non-zero while nothing but straight-line code can have run since that
 * hit. Cleared from the core: see the top of this file. */
uint8 S9xIdleLoopLive = 0;

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

/* Everything but the position in time. */
static bool same_regs(const LoopState &a, const LoopState &b)
{
	return a.pbpc == b.pbpc &&
	       a.a == b.a && a.x == b.x && a.y == b.y && a.d == b.d && a.s == b.s && a.p == b.p &&
	       a.db == b.db && a.carry == b.carry && a.zero == b.zero && a.negative == b.negative &&
	       a.overflow == b.overflow && a.open_bus == b.open_bus;
}

/* ... and no H-event in between. */
static bool same_state(const LoopState &a, const LoopState &b)
{
	return a.next_event == b.next_event && a.which_event == b.which_event && same_regs(a, b);
}

/* The last plain idle loop that was proven (two identical passes in one
 * event window), with what the proof rests on: the registers at the
 * branch, the code, and the memory bytes the body reads. H-events end a
 * skip ten times per scanline, and proving the loop again each time costs
 * two real passes; with this, one pass that arrives at the branch in the
 * same state, with the same code and the same bytes behind its operands,
 * is enough: the next pass can only repeat the proven one. */
struct KnownLoop
{
	bool valid;
	LoopState s;       /* (the event fields are not compared) */
	int32 c;           /* master cycles per pass */
	uint32 recip;      /* floor(65536 / c): passes in n cycles >= (n * recip) >> 16 */
	bool in_rom;       /* the code cannot have changed */
	int32 fast_rom_speed;
	uint8 code_len;
	const uint8 *code_ptr;
	uint8 code[16];
	uint8 nbytes;
	const uint8 *byte_ptr[12];
	uint8 byte_val[12];
};
static KnownLoop known;

static const uint8 *direct_ptr(uint32 address)
{
	uint8 *p = Memory.Map[(address & 0xffffff) >> MEMMAP_SHIFT];
	return p >= (uint8 *)CMemory::MAP_LAST ? p + (address & 0xffff) : NULL;
}

/* Record a proven LOOP_PURE body. Anything unusual simply leaves no
 * record (the loop is then proven again each time, as before). */
static void remember_loop(uint16 target, uint16 after_branch, const LoopState &state, int32 c)
{
	known.valid = false;
	if (CheckEmulation())
		return; /* direct page wraps differently */
	if (Settings.BS)
		return; /* remaps memory while running */

	uint32 pb = ICPU.ShiftedPB;
	unsigned len = (uint16)(after_branch - target);
	if (len < 2 || len > sizeof(known.code) || (target & MEMMAP_MASK) + len > MEMMAP_BLOCK_SIZE)
		return;
	const uint8 *code = direct_ptr(pb | target);
	if (!code)
		return;

	unsigned n = 0;
	for (unsigned pos = 0; pos < len - 2;)
	{
		uint8 op = code[pos];
		uint32 a0, a1;
		switch (body_mode(op))
		{
		case DP:
			a0 = (Registers.D.W + code[pos + 1]) & 0xffff;
			a1 = (a0 + 1) & 0xffff;
			break;
		case ABS:
			a0 = ICPU.ShiftedDB | code[pos + 1] | (code[pos + 2] << 8);
			a1 = ICPU.ShiftedDB | ((a0 + 1) & 0xffff);
			break;
		case LONG:
			a0 = code[pos + 1] | (code[pos + 2] << 8) | (code[pos + 3] << 16);
			a1 = (a0 + 1) & 0xffffff;
			break;
		case IMM:
		case IMPL:
			pos += ICPU.S9xOpLengths[op];
			continue;
		default:
			return;
		}
		const uint8 *p0 = direct_ptr(a0), *p1 = direct_ptr(a1);
		if (!p0 || !p1 || n + 2 > sizeof(known.byte_val))
			return;
		known.byte_ptr[n] = p0;
		known.byte_val[n++] = *p0;
		known.byte_ptr[n] = p1;
		known.byte_val[n++] = *p1;
		pos += ICPU.S9xOpLengths[op];
	}

	known.s = state;
	known.c = c;
	known.recip = 65536u / (uint32)c;
	known.in_rom = code >= Memory.ROM && code < Memory.ROM + CMemory::MAX_ROM_SIZE;
	known.fast_rom_speed = CPU.FastROMSpeed;
	known.code_len = (uint8)len;
	known.code_ptr = code;
	memcpy(known.code, code, len);
	known.nbytes = (uint8)n;
	known.valid = true;
}

/* The CPU is in this recorded state (but for the position in time)? The
 * fields most likely to differ come first. */
static inline bool at_state(const LoopState &k)
{
	return Registers.PBPC == k.pbpc && Registers.X.W == k.x && Registers.Y.W == k.y && Registers.A.W == k.a &&
	       Registers.P.W == k.p && Registers.S.W == k.s && Registers.D.W == k.d && Registers.DB == k.db &&
	       ICPU._Carry == k.carry && ICPU._Zero == k.zero && ICPU._Negative == k.negative &&
	       ICPU._Overflow == k.overflow && OpenBus == k.open_bus;
}

/* At the branch of the remembered loop, in the proven state? */
static inline bool at_known_loop(void)
{
	if (!known.valid || !at_state(known.s) || CPU.FastROMSpeed != known.fast_rom_speed)
		return false;
	/* (The pointers are as good as the addresses while the memory map
	 * stands: see S9xIdleLoopReset and remember_loop. Looking each one up
	 * again here, 800 times a frame, cost 5% of the 65816's time.) */
	for (unsigned i = 0; i < known.nbytes; i++)
		if (*known.byte_ptr[i] != known.byte_val[i])
			return false;
	return known.in_rom || memcmp(known.code_ptr, known.code, known.code_len) == 0;
}

/* Reset, ROM load, state load: nothing remembered applies any more. */
void S9xIdleLoopReset(void)
{
	S9xIdleLoopLive = 0;
	last_kind = 0;
	known.valid = false;
	memset(kind_cache, 0, sizeof(kind_cache));
#ifdef ORACLE_IDLE_STATS
	extern void ps2_idle_shadow_reset(void);
	ps2_idle_shadow_reset();
#endif
}

#ifdef ORACLE_IDLE_STATS
/* Host oracle, ps2_idle_enabled == 2: skip nothing, and check every skip
 * that would have been taken. A skip of k passes at c cycles claims that
 * the next k hits of the hook are this same branch, in the same state,
 * c cycles apart. */
uint32 ps2_idle_shadow_checked = 0, ps2_idle_shadow_wrong = 0;
static struct
{
	LoopState s;
	int32 c, left;
	bool cross; /* the pass runs across H-events: only the registers and the cycle count are predicted */
} shadow;

void ps2_idle_shadow_reset(void)
{
	shadow.left = 0;
}

static void shadow_expect(int32 k, int32 c)
{
	snapshot(shadow.s);
	shadow.c = c;
	shadow.left = k;
	shadow.cross = false;
	ps2_idle_shadow_checked++;
}

/* The next pass, across H-events, is predicted to end at this cycle count. */
static void shadow_expect_cross(int32 end)
{
	shadow_expect(1, end - CPU.Cycles);
	shadow.cross = true;
}

/* True when this hit was one of the predicted passes. */
static bool shadow_verify(void)
{
	if (!shadow.left)
		return false;
	LoopState now;
	snapshot(now);
	shadow.s.cycles += shadow.c;
	if (!(shadow.cross ? same_regs(shadow.s, now) : same_state(shadow.s, now)) || now.cycles != shadow.s.cycles)
	{
		if (ps2_idle_shadow_wrong++ < 5)
			fprintf(stderr, "idle shadow: wrong skip at %06x, c %d: expected cycles %d, got %06x at %d\n",
				(unsigned)shadow.s.pbpc, (int)shadow.c, (int)shadow.s.cycles, (unsigned)now.pbpc, (int)now.cycles);
		shadow.left = 0;
		return false;
	}
	shadow.left--;
	return true;
}
#define SHADOW (ps2_idle_enabled == 2)
#else
#define SHADOW 0
#define shadow_expect(k, c)
#define shadow_expect_cross(end)
#endif

/* The pass that starts here, c cycles long, runs into one or more H-events.
 * Can it be taken without the interpreter? Yes if every event it meets
 * leaves the CPU alone - does not write memory, raise an interrupt, or
 * depend on the exact cycle it fires at - and the IRQ timer stays ahead:
 *   H-blank start, and HDMA start on a line without HDMA: nothing but the
 *     next event being scheduled;
 *   HDMA init on any line but 0: likewise;
 *   render: draws a scanline;
 *   WRAM refresh: 40 cycles pass.
 * Not the end of the scanline (APU, V counter, NMI, joypad, end of frame),
 * HDMA start with HDMA to do, HDMA init on line 0.
 * Follows S9xReschedule and S9xDoHEventProcessing (cpuexec.cpp).
 * *end: the cycle count when the pass has ended. */
static bool crossing_is_inert(int32 c, int32 *end)
{
	int32 at = CPU.Cycles + c, next = CPU.NextEvent;
	int which = CPU.WhichEvent;
	while (at >= next)
	{
		switch (which)
		{
		case HC_HBLANK_START_EVENT:
			which = HC_HDMA_START_EVENT;
			next = Timings.HDMAStart;
			break;
		case HC_HDMA_START_EVENT:
			if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
				return false;
			which = HC_HCOUNTER_MAX_EVENT;
			next = Timings.H_Max;
			break;
		case HC_HDMA_INIT_EVENT:
			if (CPU.V_Counter == 0)
				return false;
			which = HC_RENDER_EVENT;
			next = Timings.RenderPos;
			break;
		case HC_RENDER_EVENT:
			which = HC_WRAM_REFRESH_EVENT;
			next = Timings.WRAMRefreshPos;
			break;
		case HC_WRAM_REFRESH_EVENT:
			at += SNES_WRAM_REFRESH_CYCLES;
			which = HC_HBLANK_START_EVENT;
			next = Timings.HBlankStart;
			break;
		default: /* HC_HCOUNTER_MAX_EVENT */
			return false;
		}
	}
	/* (checked between instructions: none inside the pass may see it) */
	if (at >= Timings.NextIRQTimer)
		return false;
	*end = at;
	return true;
}

void S9xIdleLoopCheck(uint16 target, uint16 after_branch)
{
	if (!ps2_idle_enabled || Settings.SA1 || Settings.SuperFX)
		return;
#ifdef ORACLE_IDLE_STATS
	if (SHADOW && shadow_verify())
		return;
#endif

	/* Anything pending that the main loop would act on: don't touch. */
	if (CPU.NMIPending || CPU.IRQLine || CPU.IRQExternal || CPU.WaitingForInterrupt ||
	    Timings.IRQFlagChanging || (CPU.Flags & SCAN_KEYS_FLAG))
	{
		S9xIdleLoopLive = 0;
		return;
	}

	if (at_known_loop())
	{
		const int32 c = known.c;
		for (;;)
		{
			int32 limit = CPU.NextEvent < Timings.NextIRQTimer ? CPU.NextEvent : Timings.NextIRQTimer;
			/* Every skipped pass must end before the limit (events fire
			 * at "cycles >= event"). This runs some 800 times a frame
			 * and the R5900 takes 37 cycles to divide: multiply by the
			 * reciprocal, rounded down - at worst one pass fewer is
			 * skipped here, and it is taken below. */
			int32 room = limit - CPU.Cycles - 1;
			int32 k = room > 0 ? (int32)(((uint32)room * known.recip) >> 16) : 0;
			if (k > 0 && SHADOW)
			{
				shadow_expect(k, c);
				break;
			}
			if (k > 0)
			{
				ps2_idle_plain_skips++;
				CPU.Cycles += k * c;
				ps2_idle_skipped_cycles += (uint32)(k * c);
			}

			/* The next pass meets the event. H-events come ten to a
			 * scanline, and most of them are nothing to the CPU: take
			 * the pass and the events here and carry on, instead of
			 * going back to the interpreter for one pass each time. The
			 * loop's state is the same afterwards by the same argument
			 * as for the passes above. */
			int32 end;
			if (!crossing_is_inert(c, &end))
				break;
			if (SHADOW)
			{
				shadow_expect_cross(end);
				break;
			}
			ps2_idle_crossings++;
			ps2_idle_skipped_cycles += (uint32)c;
			CPU.Cycles += c;
			while (CPU.Cycles >= CPU.NextEvent)
				S9xDoHEventProcessing();
		}
		/* (not a pass the proof below may measure from) */
		S9xIdleLoopLive = 0;
		return;
	}

	/* Most hits are loops whose registers change (counters): they only
	 * leave their state behind for the next hit to compare with. This is
	 * the hot path of the hook - thousands of hits per frame. */
	if (!S9xIdleLoopLive || !at_state(last))
	{
		snapshot(last);
		last_kind = 0;
		S9xIdleLoopLive = 1;
		return;
	}

	/* Two passes with the same registers: decode the body. */
	LoopState now;
	snapshot(now);
	uint8 kind, len = (uint8)(after_branch - target);
	const uint8 *code = direct_ptr(ICPU.ShiftedPB | target);
	bool in_rom = code >= Memory.ROM && code < Memory.ROM + CMemory::MAX_ROM_SIZE;
	if (!code || len > sizeof(last_code) || (target & MEMMAP_MASK) + len > MEMMAP_BLOCK_SIZE)
		kind = LOOP_IMPURE;
	else
		kind = (uint8)loop_kind(target, after_branch, code, len, in_rom);

	/* No H-event since the previous hit, and the code that ran from there
	 * was this body, so the pass between the two was the body and nothing
	 * else. For code in ROM that follows from the bytes being what they
	 * are now; code in RAM must have been decoded at the previous hit too
	 * (it could otherwise have been something that rewrote itself). */
	if (kind != LOOP_IMPURE && same_state(last, now) &&
	    (in_rom || (last_kind == kind && last_len == len && !memcmp(last_code, code, len))))
	{
		int32 c = now.cycles - last.cycles;
		if (c > 0 && c <= 512)
		{
			int32 limit = CPU.NextEvent < Timings.NextIRQTimer ? CPU.NextEvent : Timings.NextIRQTimer;
			/* Whole passes that end before the limit (events fire at
			 * "cycles >= event"). */
			int32 k = (limit - CPU.Cycles - 1) / c;
			/* Handshake loops: only as far as the SPC700 leaves the
			 * port unchanged (runs it ahead; see apu.cpp). */
			if (k > 0 && kind == LOOP_HANDSHAKE)
				k = CPU.InDMAorHDMA || SHADOW ? 0 : S9xAPUHandshakeSkip(c, k);
			if (k > 0 && SHADOW)
			{
				remember_loop(target, after_branch, now, c);
				shadow_expect(k, c);
			}
			else if (k > 0)
			{
				if (kind == LOOP_HANDSHAKE)
					ps2_idle_handshake_skips++, ps2_idle_handshake_passes += k;
				else
					ps2_idle_plain_skips++;
				if (kind == LOOP_PURE)
					remember_loop(target, after_branch, now, c);
				CPU.Cycles += k * c;
				ps2_idle_skipped_cycles += (uint32)(k * c);
				now.cycles = CPU.Cycles;
			}
		}
	}

	last = now;
	last_kind = kind;
	if (kind != LOOP_IMPURE && !in_rom)
	{
		last_len = len;
		memcpy(last_code, code, len);
	}
	S9xIdleLoopLive = 1;
}
