/* Host-side reference ("oracle") for the PS2 port.
 *
 * Headless Snes9x: same init sequence and core settings as ps2_main.cpp,
 * no input, no save data. Prints the port profiler's fingerprint lines
 * ("PFRAME n crc") so any PS2 build (PCSX2 or hardware) can be compared
 * against a core built natively - pristine upstream, or the port's
 * edited core with -D_EE. Pixels are normalized to the GS layout
 * (BGR555) before hashing, whatever PIXEL_FORMAT the core was built with.
 *
 *   oracle ROM FRAMES [--dump N[,N...]] [--dump-dir DIR]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <zlib.h>

#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "ppu.h"
#include "controls.h"
#include "cheats.h"
#include "snapshot.h"
#include "conffile.h"
#include "display.h"
#include "apu/bapu/snes/snes.hpp"
#ifdef ORACLE_IDLE_STATS
#include "ps2_idle.h"
#endif

static unsigned frame_no = 0;
static uLong audio_crc = 0;
static std::set<unsigned> dump_frames;
static std::string dump_dir = ".";
static unsigned apu_dump_frame = 0; /* --dump-apu N: write the APU state at checkpoint N */

static inline uint16 to_gs(uint16 c)
{
	unsigned r, g, b;
	DECOMPOSE_PIXEL(c, r, g, b);
	return (uint16)(((b & 31) << 10) | ((g & 31) << 5) | (r & 31));
}

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
	if (apu_dump_frame && apu_dump_frame == frame_no)
	{
		std::string path = dump_dir + "/apu_state.bin";
		FILE *f = fopen(path.c_str(), "wb");
		if (f)
		{
			fwrite(state, sizeof(state), 1, f);
			fclose(f);
		}
	}
	return crc32(crc32(0L, Z_NULL, 0), state, sizeof(state));
}

/* Same batching as ps2_samples_available. */
static void samples_available(void *)
{
	static unsigned char buf[32 * 1024];
	static unsigned calls = 0;
	if (++calls < 1)
		return;
	calls = 0;
	int samples = S9xGetSampleCount();
	if (samples < 1024)
		return;
	if (samples > (int)(sizeof(buf) / 2))
		samples = sizeof(buf) / 2;
	S9xMixSamples(buf, samples);
	audio_crc = crc32(audio_crc, buf, samples * 2);
}

bool8 S9xInitUpdate(void) { return TRUE; }
bool8 S9xContinueUpdate(int, int) { return TRUE; }

bool8 S9xDeinitUpdate(int width, int height)
{
	++frame_no;
	bool dump = dump_frames.count(frame_no) != 0;
	if (!dump && frame_no % 60)
		return TRUE;

	static uint16 row[MAX_SNES_WIDTH];
	uLong crc = crc32(0L, Z_NULL, 0);
	FILE *f = NULL;
	if (dump)
	{
		char path[512];
		snprintf(path, sizeof(path), "%s/frame_%05u.ppm", dump_dir.c_str(), frame_no);
		f = fopen(path, "wb");
		if (f)
			fprintf(f, "P6\n%d %d\n255\n", width, height);
	}
	for (int y = 0; y < height; y++)
	{
		const uint16 *src = GFX.Screen + y * GFX.RealPPL;
		for (int x = 0; x < width; x++)
			row[x] = to_gs(src[x]);
		crc = crc32(crc, (const Bytef *)row, width * 2);
		if (f)
			for (int x = 0; x < width; x++)
			{
				unsigned char px[3] = { (unsigned char)((row[x] & 31) << 3),
							(unsigned char)(((row[x] >> 5) & 31) << 3),
							(unsigned char)(((row[x] >> 10) & 31) << 3) };
				fwrite(px, 3, 1, f);
			}
	}
	if (f)
		fclose(f);
	if (frame_no % 60 == 0)
		printf("PFRAME %u %08lx apu %08lx\n", frame_no, crc, apu_state_crc());
	return TRUE;
}

void S9xSyncSpeed(void) { IPPU.RenderThisFrame = TRUE; }
void S9xAutoSaveSRAM(void) {}
void S9xMessage(int, int, const char *m) { fprintf(stderr, "S9x: %s\n", m); }
void S9xExit(void) { Settings.StopEmulation = TRUE; }
bool8 S9xOpenSoundDevice(void)
{
	S9xSetSamplesAvailableCallback(samples_available, NULL);
	return TRUE;
}
void S9xToggleSoundChannel(int) {}
const char *S9xStringInput(const char *) { return NULL; }
void S9xExtraUsage(void) {}
void S9xParseArg(char **, int &, int) {}
void S9xParsePortConfig(ConfigFile &, int) {}
void S9xHandlePortCommand(s9xcommand_t, int16, int16) {}
bool S9xPollButton(uint32, bool *) { return false; }
bool S9xPollPointer(uint32, int16 *, int16 *) { return false; }
bool S9xPollAxis(uint32, int16 *) { return false; }
std::string S9xGetDirectory(enum s9x_getdirtype) { return "/nonexistent-oracle-dir"; }
std::string S9xGetFilenameInc(std::string ext, enum s9x_getdirtype t) { return S9xGetFilename(ext, t); }
bool8 S9xOpenSnapshotFile(const char *, bool8, STREAM *) { return FALSE; }
void S9xCloseSnapshotFile(STREAM) {}

static void map_buttons(void)
{
	static const char *btn[12] = { "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R" };
	S9xUnmapAllControls();
	for (int pad = 0; pad < 2; pad++)
		for (int i = 0; i < 12; i++)
		{
			char name[32];
			snprintf(name, sizeof(name), "Joypad%d %s", pad + 1, btn[i]);
			S9xMapButton(((pad + 1) << 4) | i, S9xGetCommandT(name), false);
		}
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: oracle ROM FRAMES [--dump N[,N...]] [--dump-dir DIR]\n");
		return 2;
	}
	unsigned frames = (unsigned)atoi(argv[2]);
	for (int i = 3; i < argc; i++)
	{
		if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			for (char *t = strtok(argv[++i], ","); t; t = strtok(NULL, ","))
				dump_frames.insert((unsigned)atoi(t));
		else if (!strcmp(argv[i], "--dump-apu") && i + 1 < argc)
			apu_dump_frame = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc)
			dump_dir = argv[++i];
	}

	/* ps2_apply_settings() */
	memset(&Settings, 0, sizeof(Settings));
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

	CPU.Flags = 0;
	S9xDeleteCheats();
	if (!Memory.Init() || !S9xInitAPU())
		return 1;
	S9xInitSound(32);
	S9xSetSoundMute(TRUE);
	map_buttons();
	S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
	S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
	S9xReportControllers();
	if (!S9xGraphicsInit())
		return 1;
	if (!Memory.LoadROM(argv[1]))
	{
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 1;
	}
	S9xDeleteCheats();
	S9xCheatsEnable();
	Settings.StopEmulation = FALSE;
	S9xSetSoundMute(FALSE);

	while (frame_no < frames && !Settings.StopEmulation)
		S9xMainLoop();
#ifdef ORACLE_IDLE_STATS
	fprintf(stderr, "idle: plain skips %u, handshake skips %u (%u passes)\n", ps2_idle_plain_skips,
		ps2_idle_handshake_skips, ps2_idle_handshake_passes);
#endif
	return 0;
}
