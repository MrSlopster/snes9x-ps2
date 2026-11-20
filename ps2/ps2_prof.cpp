/* On-device frame profiler (see ps2_prof.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes9x.h"
#include "gfx.h"
#include "ppu.h"
#include <zlib.h>

#include "ps2_prof.h"
#include "ps2_menu.h"
#include "ps2_net.h"
#include <stdarg.h>

extern "C" {
#include <kernel.h>
#include <timer.h>
}

extern "C" char _ftext[], _etext[];

int ps2_prof_enabled = 0;

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

static inline int current(void)
{
	return depth ? stack[depth - 1] : PROF_CPU;
}

static inline void charge(void)
{
	unsigned now = ee_count();
	acc[current()] += now - last_mark; /* wraps safely (unsigned) */
	last_mark = now;
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
	snprintf(lines[3], sizeof(lines[3]), "APU port calls %u/frame",
		 port_calls / window_frames);
	out("PROF %s | %s | %s | %s", lines[0], lines[1], lines[2], lines[3]);

	memset(acc, 0, sizeof(acc));
	port_calls = 0;
	window_start = last_mark = now;
	window_frames = 0;
}

/* Output fingerprint for A/B correctness checks of optimizations: CRC of
 * the core's rendered frame (before any overlay) every 60th rendered
 * frame. PCSX2 runs are deterministic, so equal CRCs = identical output. */
static uLong audio_crc = 0;

void ps2_prof_audio(const void *samples, int bytes)
{
	if (ps2_prof_enabled)
		audio_crc = crc32(audio_crc, (const Bytef *)samples, bytes);
}

void ps2_prof_fingerprint(int width, int height)
{
	static unsigned frame_no = 0;
	if (!ps2_prof_enabled || ++frame_no % 60)
		return;
	uLong crc = crc32(0L, Z_NULL, 0);
	for (int y = 0; y < height; y++)
		crc = crc32(crc, (const Bytef *)(GFX.Screen + y * GFX.RealPPL), width * 2);
	out("PFRAME %u %08lx audio %08lx", frame_no, crc, audio_crc);
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
