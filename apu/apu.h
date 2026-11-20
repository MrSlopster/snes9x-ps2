/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _APU_H_
#define _APU_H_

#include "../snes9x.h"

typedef void (*apu_callback) (void *);

#define SPC_SAVE_STATE_BLOCK_SIZE (1024 * 65)
#define SPC_FILE_SIZE             (66048)

bool8 S9xInitAPU (void);
void S9xDeinitAPU (void);
void S9xResetAPU (void);
void S9xSoftResetAPU (void);
uint8 S9xAPUReadPort (int);
void S9xAPUWritePort (int, uint8);
void S9xAPUExecute (void);
void S9xAPUEndScanline (void);
#ifdef _EE
int S9xAPUHandshakeSkip (int32 c, int32 kmax);

/* Fast sound: the DSP stops producing samples and reports what its voices
 * do instead (see SPC_DSP::fast_sink_t, whose members these are), for
 * playback on SPU2 hardware voices. NULL returns to normal operation. */
struct S9xFastSoundSink
{
	void (*reset) (unsigned time);
	void (*key_on) (unsigned time, int voice, unsigned start, unsigned loop);
	void (*key_off) (unsigned time, int voice);
	void (*voice) (unsigned time, int voice, int vol_l, int vol_r, int pitch);
	void (*global) (unsigned time, int mvol_l, int mvol_r, int flg, int non, int pmon);
};
void S9xAPUSetFastSound (const S9xFastSoundSink *);
/* Catches the sound unit up to the CPU; returns its time in 32 kHz samples. */
unsigned S9xAPUFastSoundSync (void);
uint8 *S9xAPUGetRAM (void);
#endif
void S9xAPUSetReferenceTime (int32);
void S9xAPUTimingSetSpeedup (int);
void S9xAPULoadState (uint8 *);
void S9xAPULoadBlarggState(uint8 *oldblock);
void S9xAPUSaveState (uint8 *);
void S9xDumpSPCSnapshot (void);
bool8 S9xSPCDump (const char *);

bool8 S9xInitSound (int);
bool8 S9xOpenSoundDevice (void);

bool8 S9xSyncSound (void);
int S9xGetSampleCount (void);
void S9xSetSoundControl (uint8);
void S9xSetSoundMute (bool8);
void S9xLandSamples (void);
void S9xClearSamples (void);
bool8 S9xMixSamples (uint8 *, int);
void S9xSetSamplesAvailableCallback (apu_callback, void *);
void S9xUpdateDynamicRate (int empty = 1, int buffer_size = 2);

#define DSP_INTERPOLATION_NONE     0
#define DSP_INTERPOLATION_LINEAR   1
#define DSP_INTERPOLATION_GAUSSIAN 2
#define DSP_INTERPOLATION_CUBIC    3
#define DSP_INTERPOLATION_SINC     4

#endif
