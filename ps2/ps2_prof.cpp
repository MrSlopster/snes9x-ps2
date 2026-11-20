/* On-device frame profiler (see ps2_prof.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes9x.h"
#include "gfx.h"
#include "ppu.h"
#include <zlib.h>
#include "apu.h"
#include "apu/bapu/snes/snes.hpp"

#include "ps2_prof.h"
#include "ps2_menu.h"
#include "ps2_net.h"
#include "ps2_idle.h"
#include "ps2_config.h"
#include "ps2_dirs.h"
#include <stdarg.h>

extern "C" {
#include <kernel.h>
#include <timer.h>
}

extern "C" char _ftext[], _etext[];

int ps2_prof_enabled = 0;
int ps2_prof_pmc = 0;

/* Profiler output: console (PCSX2 log / ps2link) and the UDP network log. */
static void out(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s\n", line);
	ps2_net_log(line);
}

#define EE_HZ 294912000u
#define AVG_FRAMES 30
#define STACK_MAX 16

static unsigned acc[PROF_COUNT];       /* cycles this window */

/* R5900 performance counters (COP0 PCCR/PCR0/PCR1), charged per bucket
 * like the cycle counts. Each window uses one event pair; the pair
 * rotates so a minute of play covers caches, branches and issue width.
 * Event numbers: EE Core User's Manual, performance counter chapter.
 * PCSX2 does not implement these (reads 0): hardware-only data. */
struct PmcSet
{
	const char *name0, *name1;
	unsigned ev0, ev1;
};
static const PmcSet pmc_sets[] = {
	{ "I$miss", "D$miss", 6, 6 },
	{ "BTACmiss", "BRmispred", 4, 3 },
	{ "1issue", "2issue", 2, 2 },
};
#define PMC_SETS ((int)(sizeof(pmc_sets) / sizeof(pmc_sets[0])))
static int pmc_set = 0;
static unsigned pmc0[PROF_COUNT], pmc1[PROF_COUNT];
static unsigned last_pc0, last_pc1;
static unsigned port_calls;            /* APU port accesses this window */
static int stack[STACK_MAX];
static int depth = 0;
static unsigned last_mark;             /* Count at the last bucket switch */
static unsigned window_start;
static int window_frames = 0;

static char lines[4][40];              /* latest report */

static inline unsigned ee_count(void)
{
	unsigned c;
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(c));
	return c;
}

static inline unsigned read_pcr0(void)
{
	unsigned v;
	__asm__ __volatile__("mfpc %0, 0" : "=r"(v));
	return v & 0x7fffffff;
}

static inline unsigned read_pcr1(void)
{
	unsigned v;
	__asm__ __volatile__("mfpc %0, 1" : "=r"(v));
	return v & 0x7fffffff;
}

/* Program PCCR for an event pair, counting in all CPU modes, and zero
 * both counters. Layout: CTE bit 31; EVENT1 19:15, U1 14, S1 13, K1 12,
 * EXL1 11; EVENT0 9:5, U0 4, S0 3, K0 2, EXL0 1. */
static void pmc_program(const PmcSet &p)
{
	unsigned pccr = (1u << 31) | (p.ev1 << 15) | (0xFu << 11) | (p.ev0 << 5) | (0xFu << 1);
	__asm__ __volatile__(
		"mtps %0, 0\n\t"
		"mtpc $zero, 0\n\t"
		"mtpc $zero, 1\n\t"
		"sync.p" ::"r"(pccr));
	last_pc0 = last_pc1 = 0;
}

static inline int current(void)
{
	return depth ? stack[depth - 1] : PROF_CPU;
}

static inline void charge(void)
{
	unsigned now = ee_count();
	int b = current();
	acc[b] += now - last_mark; /* wraps safely (unsigned) */
	last_mark = now;
	if (ps2_prof_pmc)
	{
		unsigned p0 = read_pcr0(), p1 = read_pcr1();
		pmc0[b] += (p0 - last_pc0) & 0x7fffffff;
		pmc1[b] += (p1 - last_pc1) & 0x7fffffff;
		last_pc0 = p0;
		last_pc1 = p1;
	}
}

void ps2_prof_push(int bucket)
{
	charge();
	if (depth < STACK_MAX)
		stack[depth++] = bucket;
}

void ps2_prof_pop(void)
{
	charge();
	if (depth > 0)
		depth--;
}

static double ms(unsigned cycles)
{
	return cycles * 1000.0 / EE_HZ / window_frames;
}

/* ---- PC sampling profiler -------------------------------------------
 * EE timer 1 interrupts SAMPLE_HZ times a second; the handler bins the
 * interrupted PC into 64-byte buckets over .text. Every DUMP_FRAMES
 * frames the hottest buckets are printed ("PSAMP addr count") for
 * offline symbolization with addr2line. TIM2/TIM3 belong to ps2sdk and
 * the kernel; TIM1 is free. */

#define SAMPLE_HZ 2000
#define SAMPLE_SHIFT 6
#define DUMP_FRAMES 600
#define DUMP_TOP 60

static unsigned *hist = NULL;
static unsigned hist_n = 0;
static volatile unsigned samples_other = 0, samples_total = 0;
static int sampler_id = -1;
static unsigned text0, text1;

static s32 sample_handler(s32 cause, void *arg, void *addr)
{
	(void)cause;
	(void)arg;
	unsigned pc = (unsigned)addr;
	if (pc < text0 || pc >= text1)
	{
		unsigned epc;
		__asm__ __volatile__("mfc0 %0, $14" : "=r"(epc));
		pc = epc;
	}
	samples_total++;
	if (pc >= text0 && pc < text1)
		hist[(pc - text0) >> SAMPLE_SHIFT]++;
	else
		samples_other++;

	*T1_MODE |= 1 << 10; /* EQUF: write 1 to acknowledge */
	ExitHandler();
	return 0;
}

static void sampler_start(void)
{
	if (sampler_id >= 0)
		return;
	text0 = (unsigned)_ftext;
	text1 = (unsigned)_etext;
	hist_n = ((text1 - text0) >> SAMPLE_SHIFT) + 1;
	if (!hist)
		hist = (unsigned *)calloc(hist_n, sizeof(unsigned));
	if (!hist)
		return;

	*T1_MODE = 0;
	*T1_COUNT = 0;
	*T1_COMP = kBUSCLKBY256 / SAMPLE_HZ;
	sampler_id = AddIntcHandler2(INTC_TIM1, sample_handler, 0, NULL);
	EnableIntc(INTC_TIM1);
	/* BUSCLK/256, clear on compare, count, compare interrupt, ack. */
	*T1_MODE = Tn_MODE(2, 0, 0, 0, 1, 1, 1, 0, 1, 0);
}

static void sampler_stop(void)
{
	if (sampler_id < 0)
		return;
	*T1_MODE = 0;
	DisableIntc(INTC_TIM1);
	RemoveIntcHandler(INTC_TIM1, sampler_id);
	sampler_id = -1;
}

static void sampler_dump(void)
{
	if (!hist)
		return;
	out("PSAMP total %u other %u text %08x-%08x", samples_total, samples_other, text0, text1);
	for (int k = 0; k < DUMP_TOP; k++)
	{
		unsigned best = 0, bi = 0;
		for (unsigned i = 0; i < hist_n; i++)
			if (hist[i] > best)
				best = hist[i], bi = i;
		if (!best)
			break;
		out("PSAMP %08x %u", text0 + (bi << SAMPLE_SHIFT), best);
		hist[bi] = 0; /* consumed */
	}
	memset(hist, 0, hist_n * sizeof(unsigned));
	samples_total = samples_other = 0;
}

void ps2_prof_frame(void)
{
	static bool armed = false;
	static int sample_frames = 0;

	if (!ps2_prof_enabled)
	{
		armed = false;
		sampler_stop();
		return;
	}

	sampler_start();
	if (++sample_frames >= DUMP_FRAMES)
	{
		sample_frames = 0;
		sampler_dump();
	}

	unsigned now = ee_count();
	if (!armed)
	{
		/* Start a fresh window at a frame boundary. */
		memset(acc, 0, sizeof(acc));
		memset(pmc0, 0, sizeof(pmc0));
		memset(pmc1, 0, sizeof(pmc1));
		pmc_program(pmc_sets[pmc_set]);
		port_calls = 0;
		depth = 0;
		last_mark = window_start = now;
		window_frames = 0;
		armed = true;
		return;
	}

	charge();
	if (++window_frames < AVG_FRAMES)
		return;

	unsigned total = now - window_start;
	double fps = window_frames * (double)EE_HZ / total;

	snprintf(lines[0], sizeof(lines[0]), "CPU %5.2f APU %5.2f PPU %5.2f",
		 ms(acc[PROF_CPU]), ms(acc[PROF_APU]), ms(acc[PROF_PPU]));
	snprintf(lines[1], sizeof(lines[1]), "VID %5.2f MIX %5.2f IOP %5.2f",
		 ms(acc[PROF_VIDEO]), ms(acc[PROF_MIX]), ms(acc[PROF_IOP]));
	snprintf(lines[2], sizeof(lines[2]), "WAIT%5.2f =%6.2fms %4.1ffps",
		 ms(acc[PROF_WAIT]), ms(total), fps);
	{
		/* Share of emulated 65816 time skipped as exact idle loops. */
		double frame_cycles = (double)Timings.H_Max * Timings.V_Max * window_frames;
		snprintf(lines[3], sizeof(lines[3]), "APUport %u/f idle %4.1f%%",
			 port_calls / window_frames, 100.0 * ps2_idle_skipped_cycles / frame_cycles);
		ps2_idle_skipped_cycles = 0;
	}

	if (ps2_prof_pmc)
	{
		/* Thousands of events per frame, per bucket. */
		const PmcSet &p = pmc_sets[pmc_set];
		static const char *bn[PROF_COUNT] = { "CPU", "APU", "PPU", "VID", "MIX", "IOP", "WAIT" };
		char line[256];
		int n = snprintf(line, sizeof(line), "PMC %s/%s k/frame:", p.name0, p.name1);
		unsigned t0 = 0, t1 = 0;
		for (int b = 0; b < PROF_COUNT; b++)
		{
			t0 += pmc0[b], t1 += pmc1[b];
			if (b <= PROF_VIDEO)
				n += snprintf(line + n, sizeof(line) - n, " %s %.1f/%.1f", bn[b],
					      pmc0[b] / 1000.0 / window_frames, pmc1[b] / 1000.0 / window_frames);
		}
		snprintf(line + n, sizeof(line) - n, " ALL %.1f/%.1f", t0 / 1000.0 / window_frames,
			 t1 / 1000.0 / window_frames);
		out("%s", line);
	}
	out("PROF %s | %s | %s | %s", lines[0], lines[1], lines[2], lines[3]);

	memset(acc, 0, sizeof(acc));
	memset(pmc0, 0, sizeof(pmc0));
	memset(pmc1, 0, sizeof(pmc1));
	port_calls = 0;
	window_frames = 0;
	pmc_set = (pmc_set + 1) % PMC_SETS;
	pmc_program(pmc_sets[pmc_set]);
	window_start = last_mark = ee_count();
}

/* Output fingerprint for A/B correctness checks of optimizations: CRC of
 * the core's rendered frame (before any overlay) every 60th rendered
 * frame. PCSX2 runs are deterministic, so equal CRCs = identical output. */
static uLong audio_crc = 0;

/* Integer fingerprint of the whole emulated sound unit: sync the APU to
 * the current CPU time (exact at any point), then hash its save state
 * (SPC700 RAM/registers/timers, DSP internals). Unlike a CRC of mixed
 * samples it does not depend on when samples are drained or on the
 * host's float rounding, so it is comparable across builds and machines. */
static uLong apu_state_crc(void)
{
	static uint8 state[SPC_SAVE_STATE_BLOCK_SIZE];
	S9xAPUExecute();
	SNES::dsp.synchronize();
	S9xAPUSaveState(state);
	return crc32(crc32(0L, Z_NULL, 0), state, sizeof(state));
}

void ps2_prof_audio(const void *samples, int bytes)
{
	if (ps2_prof_enabled)
		audio_crc = crc32(audio_crc, (const Bytef *)samples, bytes);
}

void ps2_prof_fingerprint(int width, int height)
{
	static unsigned frame_no = 0;
	if (!ps2_prof_enabled)
		return;
	++frame_no;

	/* Debug: raw frame dumps for exact visual comparisons between builds
	 * (ps2.cfg dump_frames=1): "<base>/dump_NNNNN.raw" every 300th frame,
	 * header = width, height (uint16 LE), then rows of BGR555 pixels. */
	if (ps2_cfg.dump_frames && frame_no % 300 == 0)
	{
		char path[300];
		snprintf(path, sizeof(path), "%s/dump_%05u.raw", ps2_base_dir(), frame_no);
		FILE *f = fopen(path, "wb");
		if (f)
		{
			uint16 hdr[2] = { (uint16)width, (uint16)height };
			fwrite(hdr, sizeof(hdr), 1, f);
			for (int y = 0; y < height; y++)
				fwrite(GFX.Screen + y * GFX.RealPPL, 2, width, f);
			fclose(f);
		}
	}

	if (frame_no % 60)
		return;
	uLong crc = crc32(0L, Z_NULL, 0);
	for (int y = 0; y < height; y++)
		crc = crc32(crc, (const Bytef *)(GFX.Screen + y * GFX.RealPPL), width * 2);
	out("PFRAME %u %08lx apu %08lx", frame_no, crc, apu_state_crc());
}

void ps2_prof_overlay(unsigned short *screen, int pitch, int width, int height)
{
	(void)screen;
	(void)pitch;
	if (!ps2_prof_enabled || !lines[0][0])
		return;

	/* Below the top overscan, 8x10 font, 32 columns fit a 256-wide frame. */
	for (int i = 0; i < 4; i++)
		ps2_menu_overlay_text(4, 16 + i * 10, lines[i], width, height);
}

/* ---- core hooks via -Wl,--wrap (mangled C++ names) ------------------ */

extern "C" {

void __real__Z17S9xAPUEndScanlinev(void);
void __wrap__Z17S9xAPUEndScanlinev(void)
{
	if (!ps2_prof_enabled)
		return __real__Z17S9xAPUEndScanlinev();
	ps2_prof_push(PROF_APU);
	__real__Z17S9xAPUEndScanlinev();
	ps2_prof_pop();
}

uint8 __real__Z14S9xAPUReadPorti(int port);
uint8 __wrap__Z14S9xAPUReadPorti(int port)
{
	if (!ps2_prof_enabled)
		return __real__Z14S9xAPUReadPorti(port);
	port_calls++;
	ps2_prof_push(PROF_APU);
	uint8 v = __real__Z14S9xAPUReadPorti(port);
	ps2_prof_pop();
	return v;
}

void __real__Z15S9xAPUWritePortih(int port, uint8 byte);
void __wrap__Z15S9xAPUWritePortih(int port, uint8 byte)
{
	if (!ps2_prof_enabled)
		return __real__Z15S9xAPUWritePortih(port, byte);
	port_calls++;
	ps2_prof_push(PROF_APU);
	__real__Z15S9xAPUWritePortih(port, byte);
	ps2_prof_pop();
}

void __real__Z15S9xUpdateScreenv(void);
void __wrap__Z15S9xUpdateScreenv(void)
{
	if (!ps2_prof_enabled)
		return __real__Z15S9xUpdateScreenv();
	ps2_prof_push(PROF_PPU);
	__real__Z15S9xUpdateScreenv();
	ps2_prof_pop();
}

void __real__Z19S9xEndScreenRefreshv(void);
void __wrap__Z19S9xEndScreenRefreshv(void)
{
	if (!ps2_prof_enabled)
		return __real__Z19S9xEndScreenRefreshv();
	ps2_prof_push(PROF_PPU);
	__real__Z19S9xEndScreenRefreshv();
	ps2_prof_pop();
}

} /* extern "C" */
