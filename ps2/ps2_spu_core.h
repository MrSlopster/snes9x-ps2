#pragma once
/* Fast sound, portable half: turns what the DSP core reports in fast mode
 * (SPC_DSP::fast_sink_t) into the snesspu event stream, and keeps the
 * BRR samples the voices use transcoded and loaded in SPU2 RAM. The
 * transport is a pair of callbacks: RPC to snesspu.irx on the PS2
 * (ps2_spu.cpp), a software SPU2 on the host (tools/oracle). */

#include <stdint.h>
#include "snesspu/snesspu.h"

struct SpuBackend
{
	/* Write sample data to SPU2 RAM. addr and size are multiples of 64. */
	void (*upload)(uint32_t addr, const uint8_t *data, uint32_t size);
	/* Deliver events (in time order). 'now' is the emulated time. */
	void (*events)(const snesspu_event *ev, unsigned count, uint32_t now);
};

/* apuram: the SPC700's 64 KB (samples are read from it at key-on). */
void spu_core_init(const uint8_t *apuram, const SpuBackend *backend);

/* The DSP core's reports (same signatures as SPC_DSP::fast_sink_t). */
void spu_core_reset(unsigned time);
void spu_core_key_on(unsigned time, int voice, unsigned start, unsigned loop);
void spu_core_key_off(unsigned time, int voice);
void spu_core_voice(unsigned time, int voice, int vol_l, int vol_r, int pitch);
void spu_core_global(unsigned time, int mvol_l, int mvol_r, int flg, int non, int pmon);

/* Send what has been collected; call once per frame with the DSP caught
 * up to the present. */
void spu_core_flush(unsigned now);

/* Forget the loaded samples (the SPU2 RAM is about to be reused). */
void spu_core_forget_samples(void);

struct SpuCoreStats
{
	unsigned samples;       /* samples transcoded */
	unsigned sample_bytes;  /* ADPCM bytes uploaded */
	unsigned bad_samples;   /* key-ons on data that is not a sample */
	unsigned ram_resets;    /* times the SPU2 RAM filled up */
	unsigned events;
	unsigned key_ons;
};
extern SpuCoreStats spu_core_stats;
