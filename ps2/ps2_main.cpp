/* PS2 port of Snes9x 1.63 - stub port layer (scaffold) */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "ppu.h"
#include "controls.h"
#include "cheats.h"
#include "stream.h"
#include "conffile.h"
#include "ps2_video.h"
#include "s9x_sound_driver_audsrv.h"

#include "ps2_dirs.h"
#include "ps2_controls.h"
#include "ps2_iop.h"
#include "ps2_menu.h"

static S9xAudsrvSoundDriver audsrv_driver;
static bool ps2_sound_ok = false;

/* Resampler capacity requested from S9xInitSound, in ms of output. Must
 * comfortably exceed one AUDIO_BATCH (plus the core's 48-sample landing
 * granularity) or the DSP would overrun it between drains. */
#define AUDIO_BUFFER_MS 32

/* int16 values (stereo interleaved) per audsrv push: 1024 = 512 frames =
 * 10.7 ms at 48 kHz, ~1.6 pushes per 60 Hz frame (1600 values/frame). */
#define AUDIO_BATCH 1024

/* Only look at the sample count on every Nth callback. The callback fires
 * on nearly every scanline while samples accumulate, and
 * S9xGetSampleCount() does double-precision maths, which the R5900 FPU
 * (single precision only) runs in software. One scanline lands ~4 input
 * values, so 16 calls overshoot a batch by < 70 values: well inside the
 * AUDIO_BUFFER_MS resampler. */
#define AUDIO_POLL_EVERY 16

/* Core pushes samples through this callback (registered with
 * S9xSetSamplesAvailableCallback); we drain the resampler into audsrv.
 *
 * The core calls this whenever 48 samples are pending, i.e. ~33 times per
 * frame. Every drain is two blocking SIF RPCs to the IOP
 * (audsrv_wait_audio + audsrv_play_audio), so draining at that rate burns
 * a large slice of the EE frame on IOP round trips. Let the resampler
 * accumulate and push in AUDIO_BATCH chunks instead. */
static void ps2_samples_available(void *)
{
	if (!ps2_sound_ok || Settings.Mute)
		return;

	static unsigned char sound_buffer[32 * 1024];
	static unsigned calls = 0;

	if (++calls < AUDIO_POLL_EVERY)
		return;
	calls = 0;

	int samples = S9xGetSampleCount();

	if (samples < AUDIO_BATCH)
		return;
	if (samples > (int)(sizeof(sound_buffer) / 2))
		samples = sizeof(sound_buffer) / 2;

	if (Settings.TurboMode || !Settings.SoundSync)
	{
		/* Never block when pacing doesn't apply: write only what the
		 * audsrv ring accepts right now, drop the rest. */
		int max_samples = audsrv_driver.space_free() / 2;
		if (max_samples <= 0)
			return;
		if (samples > max_samples)
			samples = max_samples;
	}

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

/* Core calls this once per rendered frame. Ported from unix/unix.cpp
 * S9xSyncSpeed: with a live sound device, the blocking audsrv ring write
 * is the throttle (unix's SoundSync path); otherwise pace by wall clock
 * with unix-style auto frameskip. The GS vsync wait stays inside the
 * video blit as display sync only - it is never the sole frame timer, so
 * PAL/NTSC region changes cannot freeze the port. */
void S9xSyncSpeed(void)
{
	if (Settings.TurboMode)
	{
		if ((++IPPU.FrameSkip >= Settings.TurboSkipFrames) && !Settings.HighSpeedSeek)
		{
			IPPU.FrameSkip = 0;
			IPPU.SkippedFrames = 0;
			IPPU.RenderThisFrame = TRUE;
		}
		else
		{
			IPPU.SkippedFrames++;
			IPPU.RenderThisFrame = FALSE;
		}

		if (ps2_sound_ok && Settings.SoundSync)
			return;
	}

	if (Settings.SoundSync && ps2_sound_ok)
		return;

	static struct timespec next1 = { 0, 0 };
	struct timespec now;

	while (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		;

	if (next1.tv_sec == 0)
	{
		next1 = now;
		next1.tv_nsec += 1000000;
	}

	/* AUTO_FRAMERATE: render everything when there is spare time,
	 * otherwise render 1-in-10 while catching up (unix logic). */
	bool late = (next1.tv_sec < now.tv_sec) ||
		    (next1.tv_sec == now.tv_sec && next1.tv_nsec < now.tv_nsec);
	unsigned limit = (Settings.SkipFrames == AUTO_FRAMERATE)
				 ? (late ? 10 : 1)
				 : (unsigned)Settings.SkipFrames;

	IPPU.RenderThisFrame = (++IPPU.SkippedFrames >= limit) ? TRUE : FALSE;

	if (IPPU.RenderThisFrame)
		IPPU.SkippedFrames = 0;
	else if (late)
	{
		uint64 lag_us = (uint64)(now.tv_sec - next1.tv_sec) * 1000000 +
				(now.tv_nsec - next1.tv_nsec) / 1000;
		if (lag_us >= 500000)
		{
			/* Half a second behind means a pause happened; resync
			 * instead of fast-forwarding. */
			next1 = now;
		}
	}

	/* Sleep until this frame's target time in small slices. */
	while (1)
	{
		while (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
			;
		if (now.tv_sec > next1.tv_sec ||
		    (now.tv_sec == next1.tv_sec && now.tv_nsec >= next1.tv_nsec))
			break;

		struct timespec slice = { 0, 200000 }; /* 0.2 ms */
		nanosleep(&slice, NULL);
	}

	/* Next frame target: Settings.FrameTime is in microseconds. */
	next1.tv_nsec += (long)Settings.FrameTime * 1000;
	while (next1.tv_nsec >= 1000000000)
	{
		next1.tv_sec++;
		next1.tv_nsec -= 1000000000;
	}
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
	/* audsrv_init() spins forever if the AUDSRV module is not running. */
	if (!ps2_iop_audio_ok())
	{
		printf("audsrv module not loaded - falling back to wall-clock pacing.\n");
		return FALSE;
	}

	audsrv_driver.init();

	if (!audsrv_driver.open_device(Settings.SoundPlaybackRate, 96))
	{
		audsrv_driver.deinit();
		printf("Failed to open audsrv sound device - falling back to wall-clock pacing.\n");
		return FALSE;
	}

	S9xSetSamplesAvailableCallback(ps2_samples_available, NULL);
	audsrv_driver.start();
	ps2_sound_ok = true;
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
	printf("\nSnes9x " VERSION " for PS2 (argv[0]=%s)\n", argc > 0 ? argv[0] : "?");

	/* GS comes up first: every later failure path can then show a
	 * color-coded screen instead of an invisible black screen on a TV
	 * without a ps2link console (colors: yellow=alloc, magenta=no ROM,
	 * red=ROM load failed, blue=graphics init failed). */
	ps2_video_init();

	/* IOP drivers before anything touches files, pads or audio. */
	ps2_iop_init(argc > 0 ? argv[0] : NULL);

	ps2_dirs_init(argc > 0 ? argv[0] : NULL);
	ps2_apply_settings();

	CPU.Flags = 0;
	S9xDeleteCheats();

	if (!Memory.Init() || !S9xInitAPU())
	{
		printf("Snes9x: Memory allocation failure\n");
		ps2_video_test(0xFF, 0xFF, 0x00);
		while (1)
			;
	}

	S9xInitSound(AUDIO_BUFFER_MS);
	S9xSetSoundMute(TRUE);
	ps2_input_init();

	/* Allocates GFX.Screen and the renderer buffers; without it the first
	 * rendered frame writes through a NULL pointer. */
	if (!S9xGraphicsInit())
	{
		printf("Snes9x: graphics init failed (out of memory)\n");
		ps2_video_test(0x00, 0x00, 0xFF);
		while (1)
			;
	}

	/* ROM: argv[1] when the loader passes one, otherwise the on-screen
	 * browser (starting in <base>/roms). Without a pad driver the browser
	 * cannot be used: fall back to the first ROM in <base>/roms. */
	std::string rom_path = (argc > 1) ? argv[1] : "";
	std::string load_error, failed_name;
	std::string start_dir = std::string(ps2_base_dir()) + "/roms";

	while (1)
	{
		if (rom_path.empty())
		{
			rom_path = ps2_menu_pick_rom(start_dir.c_str(),
						     load_error.empty() ? NULL : load_error.c_str(),
						     failed_name.empty() ? NULL : failed_name.c_str());
			if (rom_path.empty())
			{
				const char *first = ps2_find_rom();
				if (first)
					rom_path = first;
			}
		}

		if (rom_path.empty())
		{
			printf("No ROM found. Put ROM files in %s/roms/\n", ps2_base_dir());
			ps2_video_test(0xFF, 0x00, 0xFF);
			while (1)
				;
		}

		printf("Loading ROM: %s\n", rom_path.c_str());
		if (Memory.LoadROM(rom_path.c_str()))
			break;

		printf("Error opening ROM: %s\n", rom_path.c_str());
		if (!ps2_controls_available())
		{
			ps2_video_test(0xFF, 0x00, 0x00);
			while (1)
				;
		}

		/* Back to the browser, in the folder of the failed file. */
		failed_name = rom_path.substr(rom_path.rfind('/') + 1);
		load_error = "Could not load " + failed_name;
		start_dir = rom_path.substr(0, rom_path.rfind('/') + 1);
		rom_path.clear();
	}

	S9xDeleteCheats();
	S9xCheatsEnable();
	Memory.LoadSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());

	Settings.StopEmulation = FALSE;

	S9xSetSoundMute(FALSE);

	printf("Starting emulation\n");

	while (!Settings.StopEmulation)
	{
		ps2_input_poll();
		S9xMainLoop();
	}

	printf("Saving SRAM: %s\n", S9xGetFilename(".srm", SRAM_DIR).c_str());
	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());

	ps2_sound_ok = false;
	S9xGraphicsDeinit();
	ps2_video_deinit();

	audsrv_driver.stop();
	audsrv_driver.deinit();

	while (1)
		;

	return 0;
}
