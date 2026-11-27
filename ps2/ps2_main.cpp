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
#include "ps2_spad.h"
#include "s9x_sound_driver_audsrv.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ps2_spu.h"
#include "ps2_gs.h"
#include "ps2_watchdog.h"

#include "display.h"
#include "ps2_dirs.h"
#include "ps2_controls.h"
#include "ps2_iop.h"
#include "ps2_menu.h"
#include "ps2_config.h"
#include "ps2_rom.h"
#include "ps2_prof.h"
#include "ps2_net.h"

static S9xAudsrvSoundDriver audsrv_driver;
static bool ps2_sound_ok = false;

/* Resampler capacity requested from S9xInitSound, in ms of output. Must
 * comfortably exceed one AUDIO_BATCH (plus the core's 48-sample landing
 * granularity) or the DSP would overrun it between drains. */
#define AUDIO_BUFFER_MS 32

/* int16 values (stereo interleaved) per audsrv push: 1024 = 512 frames =
 * 10.7 ms at 48 kHz, ~1.6 pushes per 60 Hz frame (1600 values/frame). */
#define AUDIO_BATCH 1024

/* The core lands samples when the deferred APU queue is replayed
 * (apu.cpp, ~8 times per frame), so the callback can check the sample
 * count every time. */
#define AUDIO_POLL_EVERY 1

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
		int max_samples;
		{
			PROF_SCOPE(PROF_IOP);
			max_samples = audsrv_driver.space_free() / 2;
		}
		if (max_samples <= 0)
			return;
		if (samples > max_samples)
			samples = max_samples;
	}

	{
		PROF_SCOPE(PROF_MIX);
		S9xMixSamples(sound_buffer, samples);
		ps2_prof_audio(sound_buffer, samples * 2);
	}
	PROF_SCOPE(PROF_IOP);
	audsrv_driver.write_samples((int16_t *)sound_buffer, samples);
}

/* Start or resume the sound path the settings ask for. Fast mode needs
 * our IOP module and an initialised SPU2 (the audsrv device); without
 * them it falls back to the accurate path. */
static void ps2_sound_resume(void)
{
	bool fast = ps2_cfg.sound == PS2_SOUND_FAST && ps2_sound_ok && ps2_spu_available();
	ps2_spu_enable(fast);
	fast = ps2_spu_enabled();
	S9xSetSoundMute((fast || ps2_cfg.sound == PS2_SOUND_OFF) ? TRUE : FALSE);

	char line[160];
	snprintf(line, sizeof(line), "SOUND setting=%d active=%s (audsrv device %d, snesspu module %d, %s)", ps2_cfg.sound,
		 fast ? "fast" : Settings.Mute ? "off" : "accurate", ps2_sound_ok ? 1 : 0, ps2_spu_available() ? 1 : 0,
		 ps2_spu_status());
	printf("%s\n", line);
	ps2_net_log(line);
}

/* Bring the renderer in line with the settings (re-lays out GS memory
 * when it changes). */
static void ps2_renderer_apply(void)
{
	ps2_gs_compare = (ps2_cfg.gs_compare || ps2_cfg.renderer == PS2_RENDERER_GS_CHECK) ? 1 : 0;
	bool gs = ps2_cfg.renderer != PS2_RENDERER_SOFTWARE;
	if (gs != ps2_gs_enabled())
		ps2_gs_enable(gs);
}

/* Silence for menus and between games. */
static void ps2_sound_pause(void)
{
	S9xSetSoundMute(TRUE);
	ps2_spu_silence();
}

extern "C" {
#include <kernel.h>
#include <libpad.h>
}

bool8 S9xInitUpdate(void)
{
	ps2_gs_frame_begin(ps2_cfg.overlay != PS2_OVERLAY_OFF || GFX.InfoStringTimeout > 0);
	return TRUE;
}

bool8 S9xDeinitUpdate(int width, int height)
{
	PROF_SCOPE(PROF_VIDEO);
	/* A frame the GS drew is in GS memory, not in GFX.Screen. */
	if (ps2_gs_frame_picture(width, height))
	{
		ps2_prof_overlay(GFX.Screen, GFX.RealPPL, width, height);
		ps2_gs_frame_end(width, height);
		ps2_gs_log_stats();
		ps2_video_present_target(width, height);
		return TRUE;
	}
	ps2_prof_fingerprint(width, height);
	ps2_prof_overlay(GFX.Screen, GFX.RealPPL, width, height);
	ps2_video_frame(GFX.Screen, GFX.RealPPL, width, height);
	return TRUE;
}

bool8 S9xContinueUpdate(int width, int height)
{
	PROF_SCOPE(PROF_VIDEO);
	ps2_video_frame(GFX.Screen, GFX.RealPPL, width, height);
	return TRUE;
}

static inline uint32 ps2_cycle_count(void)
{
	uint32 c;
	__asm__ volatile("mfc0 %0, $9" : "=r"(c));
	return c;
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

		if (ps2_sound_ok && Settings.SoundSync && !Settings.Mute)
			return;
	}

	/* A fixed frame skip applies whatever paces emulation: render 1 of
	 * every SkipFrames frames. AUTO needs the wall-clock path below. */
	if (Settings.SkipFrames != AUTO_FRAMERATE)
	{
		IPPU.RenderThisFrame = (++IPPU.SkippedFrames >= Settings.SkipFrames) ? TRUE : FALSE;
		if (IPPU.RenderThisFrame)
			IPPU.SkippedFrames = 0;
	}

	/* audsrv's blocking write paces emulation only while samples are
	 * actually written: muted, fall through to wall-clock pacing. */
	if (Settings.SoundSync && ps2_sound_ok && !Settings.Mute)
		return;

	/* Wall-clock pacing on the CPU's own cycle counter (COP0 Count,
	 * 294.912 MHz, wraps every 14.6 s), waiting in a plain loop. This used
	 * to sleep in 0.2 ms nanosleep() slices; each of those creates a
	 * semaphore and a timer alarm, and on real hardware the main thread
	 * was found asleep for good right after a frame (hang watchdog), all
	 * the sooner the more spare time per frame there was to sleep away. */
	const uint32 ee_hz = 294912000u;
	const uint32 frame_cycles = (uint32)((uint64)Settings.FrameTime * ee_hz / 1000000);
	static uint32 next = 0;
	static bool started = false;
	uint32 now = ps2_cycle_count();

	if (!started)
	{
		next = now + ee_hz / 1000;
		started = true;
	}

	/* > 0: ahead of schedule. Anything further ahead than two frames is
	 * the counter having wrapped during a pause. */
	int32 lead = (int32)(next - now);
	if (lead > (int32)(2 * frame_cycles))
	{
		next = now;
		lead = 0;
	}
	bool late = lead < 0;

	/* AUTO_FRAMERATE: render everything when there is spare time,
	 * otherwise render 1-in-10 while catching up (unix logic). (A fixed
	 * frame skip was applied above.) */
	if (Settings.SkipFrames == AUTO_FRAMERATE)
	{
		IPPU.RenderThisFrame = (++IPPU.SkippedFrames >= (late ? 10u : 1u)) ? TRUE : FALSE;
		if (IPPU.RenderThisFrame)
			IPPU.SkippedFrames = 0;
	}

	/* Half a second behind means a pause happened (menu, state load,
	 * another ROM); resync instead of fast-forwarding. */
	if (late && (uint32)(-lead) >= ee_hz / 2)
		next = now;

	/* Wait for this frame's target time. */
	{
		PROF_SCOPE(PROF_WAIT);
		PS2_STAGE("pacing: waiting");
		while ((int32)(next - ps2_cycle_count()) > 0)
			;
		PS2_STAGE("pacing: done");
	}

	next += frame_cycles;
}

void ps2_log_storage(const char *what, int err);

void S9xAutoSaveSRAM(void)
{
	/* Called from RenderLine once per Settings.AutoSaveDelay frames when
	 * SRAM was modified. */
	std::string path = S9xGetFilename(".srm", SRAM_DIR);
	std::string line = "SRAM autosave: " + path;
	printf("%s\n", line.c_str());
	ps2_net_log(line.c_str());
	PS2_STAGE("SRAM autosave (file write)");
	int err;
	bool ok = ps2_sram_save(&err);
	PS2_STAGE("SRAM autosave done");
	if (ok)
	{
		printf("SRAM autosave: written\n");
		ps2_net_log("SRAM autosave: written");
	}
	else
	{
		ps2_log_storage("SRAM autosave FAILED", err);
		S9xSetInfoString("SAVE FAILED - game progress is not being saved");
	}
}

/* One log line about where settings and saves go. */
void ps2_log_storage(const char *what, int err)
{
	char line[240];
	snprintf(line, sizeof(line), "STORAGE %s (errno %d): ROMs under %s; settings in %s (%s); saves for this ROM in %s",
		 what, err, ps2_rom_base_dir(), ps2_base_dir(), ps2_base_writable() ? "writable" : "NOT writable",
		 ps2_save_dir());
	printf("%s\n", line);
	ps2_net_log(line);
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
	/* Non-zero core defaults from S9xLoadConfigFiles (snes9x.cpp) that a
	 * zeroed Settings would silently break: sprite tiles per scanline,
	 * Gaussian DSP interpolation, SuperFX clock. */
	Settings.MaxSpriteTilesPerLine = 34;
	Settings.InterpolationMethod = 2;
	Settings.SuperFXClockMultiplier = 100;
	Settings.CartAName[0] = 0;
	Settings.CartBName[0] = 0;
}

/* Scratchpad layout from the Makefile's --defsym list. */
#define SPAD_BASE 0x70000000u
#define SPAD_USED 0x100
static_assert(sizeof(Registers) <= 0x20, "Registers overflows its scratchpad slot");
static_assert(sizeof(ICPU) <= 0x20, "ICPU overflows its scratchpad slot");
static_assert(sizeof(CPU) <= 0x50, "CPU overflows its scratchpad slot");
static_assert(sizeof(Timings) <= 0x50, "Timings overflows its scratchpad slot");

int main(int argc, char **argv)
{
	/* The scratchpad is not part of the ELF image: clear the core state
	 * that lives there before anything can touch it. */
	memset((void *)SPAD_BASE, 0, SPAD_USED);
	ps2_spad_init();

	printf("\nSnes9x " VERSION " for PS2 (argv[0]=%s)\n", argc > 0 ? argv[0] : "?");

	/* GS comes up first: every later failure path can then show a
	 * color-coded screen instead of an invisible black screen on a TV
	 * without a ps2link console (colors: yellow=alloc, magenta=no ROM,
	 * red=ROM load failed, blue=graphics init failed). */
	ps2_video_init(PS2_VIDEO_AUTO, PS2_FILTER_SMOOTH);

	/* IOP drivers before anything touches files, pads or audio. */
	ps2_iop_init(argc > 0 ? argv[0] : NULL);

	ps2_dirs_init(argc > 0 ? argv[0] : NULL);
	ps2_apply_settings();

	/* Saved settings live next to the snes9x data on the base device. */
	ps2_config_load();
	if (ps2_cfg.video_mode != PS2_VIDEO_AUTO)
		ps2_video_set_mode(ps2_cfg.video_mode);
	if (ps2_cfg.filter != PS2_FILTER_SMOOTH)
		ps2_video_set_filter(ps2_cfg.filter);
	ps2_config_apply_core();

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

	/* After S9xGraphicsInit: its status screens draw into GFX.Screen. */
	if (ps2_cfg.netlog)
		ps2_net_start();

	/* ROM: argv[1] when the loader passes one, otherwise the on-screen
	 * browser (starting in <base>/roms). Without a pad driver the browser
	 * cannot be used: fall back to the first ROM in <base>/roms. */
	std::string rom_path = (argc > 1) ? argv[1] : "";
	std::string load_error, failed_name;
	std::string start_dir = ps2_cfg.last_dir.empty()
					? std::string(ps2_rom_base_dir()) + "/roms"
					: ps2_cfg.last_dir;

	while (1) /* one iteration per game session */
	{
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
			printf("No ROM found. Put ROM files in %s/roms/\n", ps2_rom_base_dir());
			ps2_video_test(0xFF, 0x00, 0xFF);
			while (1)
				;
		}

		printf("Loading ROM: %s\n", rom_path.c_str());
		if (ps2_load_rom(rom_path.c_str()))
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
	ps2_menu_status("Loading", Memory.ROMFilename.c_str(), "Loading save data...");
	ps2_dirs_rom_loaded();
	Memory.LoadSRAM(ps2_sram_load_path().c_str());

	/* Stays on screen until the first emulated frame replaces it: if it
	 * never goes away, emulation (or the first audio push) is stuck. */
	ps2_menu_status("Loading", Memory.ROMFilename.c_str(), "Starting game...");

	Settings.StopEmulation = FALSE;

	ps2_renderer_apply();
	ps2_gs_reset();
	ps2_sound_resume();
	ps2_watchdog_init();
	ps2_log_storage("at game start", 0);
	ps2_watchdog_arm(true);

	printf("Starting emulation\n");

	bool load_other = false;
	while (!Settings.StopEmulation)
	{
		/* Select+Start: in-game menu. */
		if ((ps2_pad_held(0) & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START))
		{
			ps2_watchdog_arm(false);
			ps2_sound_pause();
			int action = ps2_menu_pause();
			ps2_renderer_apply();
			ps2_sound_resume();
			ps2_watchdog_arm(true);
			if (action == PS2_PAUSE_LOAD_ROM)
			{
				load_other = true;
				break;
			}
		}

		PS2_STAGE("input");
		ps2_input_poll();
		PS2_STAGE("emulating");
		S9xMainLoop();
		PS2_STAGE("sound events");
		ps2_spu_frame();
		PS2_STAGE("profiler");
		ps2_prof_frame();
		ps2_watchdog_kick();
	}

	printf("Saving SRAM: %s\n", S9xGetFilename(".srm", SRAM_DIR).c_str());
	int save_err;
	bool saved = ps2_sram_save(&save_err);
	if (!saved)
		ps2_log_storage("SRAM save FAILED", save_err);

	if (!load_other)
		break;

	/* Back to the browser in the current game's folder. */
	ps2_watchdog_arm(false);
	ps2_sound_pause();
	start_dir = Memory.ROMFilename.substr(0, Memory.ROMFilename.rfind('/') + 1);
	failed_name = Memory.ROMFilename.substr(Memory.ROMFilename.rfind('/') + 1);
	load_error = saved ? std::string() : "Could not write the save of " + failed_name;
	rom_path.clear();
	} /* game session */

	ps2_sound_ok = false;
	S9xGraphicsDeinit();
	ps2_video_deinit();

	audsrv_driver.stop();
	audsrv_driver.deinit();

	while (1)
		;

	return 0;
}
