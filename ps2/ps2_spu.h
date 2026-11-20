#pragma once
/* Fast sound: the SNES voices played by SPU2 hardware voices instead of
 * being decoded, interpolated and mixed on the EE (which costs about a
 * third of a frame). The DSP core keeps the program-visible state going
 * and reports what the voices do (apu.h, S9xFastSoundSink);
 * ps2_spu_core.cpp turns that into events and ADPCM samples; this file
 * carries them to snesspu.irx on the IOP.
 *
 * Differences from the accurate path: samples are re-encoded for the
 * SPU2 (same family of 4-bit ADPCM), the chip's own interpolation is
 * used, and there is no echo. */

/* snesspu.irx is loaded (and libsd/audsrv with it). */
bool ps2_spu_available(void);

/* Switch the sound unit to or from fast mode. Also call to resume after
 * ps2_spu_silence(). */
void ps2_spu_enable(bool on);
bool ps2_spu_enabled(void);

/* Once per emulated frame: send the frame's events. */
void ps2_spu_frame(void);

/* Stop every voice now (menu, leaving a game). */
void ps2_spu_silence(void);
