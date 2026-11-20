/* PS2 port of Snes9x 1.63 - stub port layer (scaffold) */
#include <stdio.h>
#include <string.h>

#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "controls.h"
#include "cheats.h"
#include "stream.h"
#include "conffile.h"
#include "ps2_video.h"
#include "s9x_sound_driver_audsrv.h"

#include "ps2_dirs.h"
#include "ps2_controls.h"

static S9xAudsrvSoundDriver audsrv_driver;

/* Core pushes samples through this callback (registered with
 * S9xSetSamplesAvailableCallback); we drain the resampler into audsrv. */
static void ps2_samples_available(void *)
{
	if (!Settings.SoundPlaybackRate || Settings.Mute)
		return;

	static unsigned char sound_buffer[32 * 1024];
	int samples = S9xGetSampleCount();

	if (samples <= 0)
		return;
	if (samples > (int)(sizeof(sound_buffer) / 2))
		samples = sizeof(sound_buffer) / 2;

	S9xMixSamples(sound_buffer, samples);
	audsrv_driver.write_samples((int16_t *)sound_buffer, samples);
}

extern "C" {
#include <kernel.h>
}

bool8 S9xInitUpdate(void)
{
	return TRUE;
}

bool8 S9xDeinitUpdate(int width, int height)
{
	ps2_video_frame(GFX.Screen, GFX.RealPPL, width, height);
	return TRUE;
}

bool8 S9xContinueUpdate(int width, int height)
{
	ps2_video_frame(GFX.Screen, GFX.RealPPL, width, height);
	return TRUE;
}

/* Core calls this once per rendered frame for pacing; the GS vsync is the
 * display sync and the frame-rate reference (NTSC/PAL follows the console
 * region set by the GS mode, no hardcoded frame timer here). */
void S9xSyncSpeed(void)
{
	ps2_video_sync();
}

void S9xAutoSaveSRAM(void)
{
	/* Called from RenderLine once per Settings.AutoSaveDelay frames when
	 * SRAM was modified. */
	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
}

void S9xMessage(int type, int number, const char *message)
{
	(void)type;
	printf("S9x[%d] %s\n", number, message);
}

void S9xExit(void)
{
	/* Called from the core on fatal errors; when emulation is running this
	 * unwinds the main loop instead of halting. */
	Settings.StopEmulation = TRUE;
}

/* apu.cpp calls this via S9xInitSound: open + configure the audsrv device. */
bool8 S9xOpenSoundDevice(void)
{
	audsrv_driver.init();

	if (!audsrv_driver.open_device(Settings.SoundPlaybackRate, 96))
	{
		audsrv_driver.deinit();
		printf("Failed to open audsrv sound device - muting.\n");
		return FALSE;
	}

	S9xSetSamplesAvailableCallback(ps2_samples_available, NULL);
	audsrv_driver.start();
	return TRUE;
}

/* Port hooks from display.h: PS2 build has no command line beyond argv[1],
 * no sound-channel UI, and no interactive prompts. */
void S9xToggleSoundChannel(int c)
{
	(void)c;
}

const char *S9xStringInput(const char *prompt)
{
	(void)prompt;
	return NULL;
}

void S9xExtraUsage(void)
{
	printf("  rom path              path to a ROM file (default: first ROM in roms/)\n");
}

void S9xParseArg(char **argv, int &i, int argc)
{
	(void)argv;
	(void)i;
	(void)argc;
}

/* S9xGetDirectory/S9xGetFilenameInc/S9xOpenSnapshotFile/
 * S9xCloseSnapshotFile are implemented in ps2_dirs.cpp.
 * fscompat.cpp already provides both S9xGetFilename overloads and
 * S9xBasename(NoExt). */

void S9xParsePortConfig(ConfigFile &conf, int pass)
{
	(void)conf;
	(void)pass;
}

void S9xHandlePortCommand(s9xcommand_t cmd, int16 data1, int16 data2)
{
	(void)cmd;
	(void)data1;
	(void)data2;
}

bool S9xPollButton(uint32 id, bool *pressed)
{
	(void)id;
	(void)pressed;
	return FALSE;
}

bool S9xPollPointer(uint32 id, int16 *x, int16 *y)
{
	(void)id;
	(void)x;
	(void)y;
	return FALSE;
}

bool S9xPollAxis(uint32 id, int16 *value)
{
	(void)id;
	(void)value;
	return FALSE;
}

static void ps2_apply_settings(void)
{
	/* Defaults follow unix/unix.cpp main(); PS2-specific choices are the
	 * controller masters (only joypads) and 30s autosave. */
	memset(&Settings, 0, sizeof(Settings));
	Settings.MouseMaster = FALSE;
	Settings.SuperScopeMaster = FALSE;
	Settings.JustifierMaster = FALSE;
	Settings.MultiPlayer5Master = FALSE;
	Settings.FrameTimePAL = 20000;
	Settings.FrameTimeNTSC = 16667;
	Settings.SixteenBitSound = TRUE;
	Settings.Stereo = TRUE;
	Settings.SoundPlaybackRate = 48000;
	Settings.SoundInputRate = 31950;
	Settings.SoundSync = TRUE;
	Settings.Transparency = TRUE;
	Settings.AutoDisplayMessages = TRUE;
	Settings.InitialInfoStringTimeout = 120;
	Settings.HDMATimingHack = 100;
	Settings.BlockInvalidVRAMAccessMaster = TRUE;
	Settings.StopEmulation = TRUE;
	Settings.WrongMovieStateProtection = TRUE;
	Settings.StretchScreenshots = 1;
	Settings.SnapshotScreenshots = TRUE;
	Settings.SkipFrames = AUTO_FRAMERATE;
	Settings.TurboSkipFrames = 15;
	Settings.AutoSaveDelay = 30;
	Settings.CartAName[0] = 0;
	Settings.CartBName[0] = 0;
}

int main(int argc, char **argv)
{
	printf("\nSnes9x " VERSION " for PS2\n");

	ps2_dirs_init();
	ps2_apply_settings();

	CPU.Flags = 0;
	S9xDeleteCheats();

	if (!Memory.Init() || !S9xInitAPU())
	{
		printf("Snes9x: Memory allocation failure\n");
		while (1)
			;
	}

	S9xInitSound(0);
	S9xSetSoundMute(TRUE);
	ps2_input_init();

	/* ROM: argv[1], else first ROM in the roms/ dir. */
	const char *rom = (argc > 1) ? argv[1] : ps2_find_rom();
	if (!rom)
	{
		printf("No ROM found. Put ROM files in %s/roms/\n", ps2_base_dir());
		while (1)
			;
	}

	printf("Loading ROM: %s\n", rom);

	if (!Memory.LoadROM(rom))
	{
		printf("Error opening ROM: %s\n", rom);
		while (1)
			;
	}

	S9xDeleteCheats();
	S9xCheatsEnable();
	Memory.LoadSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());

	Settings.StopEmulation = FALSE;

	ps2_video_init();
	S9xSetSoundMute(FALSE);

	printf("Starting emulation\n");

	while (!Settings.StopEmulation)
	{
		ps2_input_poll();
		S9xMainLoop();
	}

	printf("Saving SRAM: %s\n", S9xGetFilename(".srm", SRAM_DIR).c_str());
	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());

	audsrv_driver.stop();
	audsrv_driver.deinit();

	while (1)
		;

	return 0;
}
