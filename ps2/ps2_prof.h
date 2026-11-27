#pragma once
/* On-device frame profiler: splits each emulated frame's EE time into
 * buckets using the R5900 COP0 Count register (294.912 MHz). Self time
 * only: a nested bucket pauses its parent. Core functions are timed via
 * linker --wrap (ps2_prof.cpp), the port's own work via PROF_SCOPE. */

enum
{
	PROF_CPU = 0, /* everything not in another bucket: 65816, DMA, glue */
	PROF_APU,     /* SPC700 + S-DSP catch-up (scanline + port accesses) */
	PROF_PPU,     /* S9xUpdateScreen / S9xEndScreenRefresh rendering */
	PROF_VIDEO,   /* ps2_video_frame: cache flush, DMA, GS */
	PROF_MIX,     /* S9xMixSamples (resampler) */
	PROF_IOP,     /* audsrv RPCs (includes blocking for ring space) */
	PROF_WAIT,    /* S9xSyncSpeed wall-clock sleep */
	PROF_COUNT
};

extern int ps2_prof_enabled;
extern int ps2_prof_pmc; /* also read the R5900 performance counters */
extern int ps2_prof_sample_all; /* PC sampler dumps every bucket, not the top 60 (ps2.cfg: psamp_all=1) */

void ps2_prof_push(int bucket);
void ps2_prof_pop(void);

/* Call once per emulated frame (S9xMainLoop iteration). */
void ps2_prof_frame(void);

/* Log a CRC of the rendered frame every 60 frames (A/B equivalence). */
void ps2_prof_fingerprint(int width, int height);

/* Fold mixed output samples into the audio fingerprint (A/B checks). */
void ps2_prof_audio(const void *samples, int bytes);

/* Draw the latest averages into a rendered frame (BGR555, pitch in px). */
void ps2_prof_overlay(unsigned short *screen, int pitch, int width, int height);

struct Ps2ProfScope
{
	Ps2ProfScope(int b)
	{
		if (ps2_prof_enabled)
			ps2_prof_push(b);
		on = ps2_prof_enabled;
	}
	~Ps2ProfScope()
	{
		if (on)
			ps2_prof_pop();
	}
	int on;
};

#define PROF_SCOPE(b) Ps2ProfScope prof_scope_##b(b)
