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
static FILE *wav_file = NULL;       /* --wav F: the mixed sound output (48 kHz stereo) */
static unsigned wav_frames = 0;

static void wav_header(FILE *f, unsigned frames)
{
	unsigned bytes = frames * 4, riff = bytes + 36;
	unsigned char h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
				0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
	memcpy(h + 4, &riff, 4);
	memcpy(h + 40, &bytes, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
}

#ifdef _EE
/* The GS renderer's hooks in gfx.cpp: not here. */
uint8 S9xGSTileValid2[4096], S9xGSTileValid4[2048], S9xGSTileValid8[1024];
uint8 S9xGSMode7Cell[0x4000], S9xGSMode7Row[128], S9xGSMode7Char[256], S9xGSMode7AnyDirty;
bool8 S9xGSBandBegin(void) { return FALSE; }
void S9xGSBandEnd(void) {}
void S9xGSBandSoftware(void) {}
bool8 S9xGSCompareMode(void) { return FALSE; }
void S9xGSBandPause(bool8) {}
void S9xGSScreen(bool8) {}
void S9xGSSelectRenderers(void) {}
void S9xGSOBJBegin(void) {}
void S9xGSOBJEnd(void) {}
#endif

#ifdef _EE
/* --smp-selftest N: the port's SPC700 loop (SMP::ee_enter) against the
 * reference op_step(), from N random machine states: random registers,
 * code and data, timers and I/O included, run for a random number of
 * cycles so that slices end in the middle of opcodes. The complete sound
 * unit state must come out the same. */
static int smp_selftest(unsigned trials)
{
	static uint8 before[SPC_SAVE_STATE_BLOCK_SIZE], a[SPC_SAVE_STATE_BLOCK_SIZE], b[SPC_SAVE_STATE_BLOCK_SIZE];
	unsigned seed = 12345, failures = 0;
	#define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)
	uint8 *ram = S9xAPUGetRAM();
	for (unsigned i = 0; i < 0x10000; i++)
		ram[i] = (uint8)RND();

	for (unsigned t = 0; t < trials; t++)
	{
		/* Fresh code and data around a random PC; now and then in the
		 * direct page (I/O at $f0-$ff) or the IPL ROM area. */
		unsigned pc = RND() * 2 + (RND() & 1);
		if (t % 7 == 0)
			pc = 0x00d0 + (RND() & 0x3f);
		if (t % 11 == 0)
			pc = 0xffb0 + (RND() & 0x3f);
		for (unsigned i = 0; i < 48; i++)
			ram[(pc + i) & 0xffff] = (uint8)RND();
		for (unsigned i = 0; i < 16; i++)
			ram[RND() * 2 & 0xffff] = (uint8)RND();
		/* (STOP and SLEEP never leave their opcode) */
		SNES::smp.regs.pc = pc;
		SNES::smp.regs.sp = RND();
		SNES::smp.regs.B.a = RND();
		SNES::smp.regs.x = RND();
		SNES::smp.regs.B.y = RND();
		SNES::smp.regs.p = RND();
		SNES::smp.opcode_cycle = 0;
		if (t % 5 == 0)
			ram[0xf1] = RND(), SNES::smp.mmio_write(0xf1, ram[0xf1]);
		if (t % 13 == 0)
			SNES::smp.mmio_write(0xfa + RND() % 3, RND());
		S9xAPUSaveState(before);

		int cycles = 1 + RND() % 60;
		S9xAPULoadState(before);
		SNES::smp.clock = -cycles;
		while (SNES::smp.clock < 0)
			SNES::smp.op_step();
		int end_a = SNES::smp.clock;
		S9xAPUSaveState(a);

		S9xAPULoadState(before);
		SNES::smp.clock = -cycles;
		SNES::smp.ee_enter();
		int end_b = SNES::smp.clock;
		S9xAPUSaveState(b);

		if (end_a != end_b || memcmp(a, b, sizeof(a)))
		{
			if (failures++ < 10)
			{
				unsigned at = 0;
				while (at < sizeof(a) && a[at] == b[at])
					at++;
				printf("FAIL trial %u: pc %04x opcode %02x, %d cycles: clock %d vs %d, first difference at state byte %u\n",
				       t, pc, before[pc], cycles, end_a, end_b, at);
			}
		}
		/* Carry on from the reference result so states stay varied. */
		S9xAPULoadState(a);
	}
	#undef RND
	printf("%u SPC700 trials\n%s (%u failures)\n", trials, failures ? "RED/FAIL" : "GREEN/PASS", failures);
	return failures ? 1 : 0;
}

/* --fast-wav F: run the sound unit in fast mode (voices played by SPU2
 * hardware on the PS2) and render what a software SPU2 makes of it. */
#include "ps2_spu_core.h"
#include "spu2sim.h"
static bool fast_sound = false;
static unsigned smp_selftest_trials = 0;
static const SpuBackend fast_backend = { spu2sim_upload, spu2sim_events };
static const S9xFastSoundSink fast_sink = { spu_core_reset, spu_core_key_on, spu_core_key_off, spu_core_voice,
					    spu_core_global };
#endif

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
	if (samples < 1024 && !wav_file) /* a recording takes everything, at once */
		return;
	if (samples > (int)(sizeof(buf) / 2))
		samples = sizeof(buf) / 2;
	S9xMixSamples(buf, samples);
	audio_crc = crc32(audio_crc, buf, samples * 2);
	if (wav_file)
	{
		fwrite(buf, 2, samples, wav_file);
		wav_frames += samples / 2;
	}
}

bool8 S9xInitUpdate(void) { return TRUE; }
bool8 S9xContinueUpdate(int, int) { return TRUE; }

bool8 S9xDeinitUpdate(int width, int height)
{
	++frame_no;
#ifdef _EE
	if (fast_sound)
		spu_core_flush(S9xAPUFastSoundSync());
#endif
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

/* --seed N: deterministic random pad input on both pads, so that the
 * comparison reaches gameplay and not only boot code and title screens.
 * Start, A and X are pressed more often to get through menus. */
static bool in_enabled;
static uint32 in_seed = 1;
static uint8 in_held[2][12];
static void input_frame(void)
{
	for (int pad = 0; pad < 2; pad++)
		for (int i = 0; i < 12; i++)
		{
			if (in_held[pad][i])
				in_held[pad][i]--;
			else
			{
				in_seed = in_seed * 1103515245u + 12345u;
				unsigned r = in_seed >> 13;
				unsigned p = (i == 3 || i == 8 || i == 9) ? 500u : 150u;
				if ((r & 0x7fff) < p)
					in_held[pad][i] = 1 + ((r >> 8) % 45);
			}
			S9xReportButton(((pad + 1) << 4) | i, in_held[pad][i] != 0);
		}
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: oracle ROM FRAMES [--dump N[,N...]] [--dump-dir DIR] [--dump-apu N] [--seed N]\n"
				"              [--wav FILE] [--fast-wav FILE | --smp-selftest N (port build)]\n");
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
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
		{
			wav_file = fopen(argv[++i], "wb");
			if (wav_file)
				wav_header(wav_file, 0);
		}
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
		{
			in_enabled = true;
			in_seed = (uint32)atoi(argv[++i]) | 1;
		}
#ifdef _EE
		else if (!strcmp(argv[i], "--no-idle"))
			ps2_idle_enabled = 0;
		else if (!strcmp(argv[i], "--idle-shadow"))
			ps2_idle_enabled = 2;
		else if (!strcmp(argv[i], "--smp-selftest") && i + 1 < argc)
			smp_selftest_trials = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fast-wav") && i + 1 < argc)
			fast_sound = spu2sim_open(argv[++i]);
#endif
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
#ifdef _EE
	if (fast_sound)
	{
		spu_core_init(S9xAPUGetRAM(), &fast_backend);
		S9xAPUSetFastSound(&fast_sink);
	}
#endif
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
#ifdef _EE
	if (smp_selftest_trials)
		return smp_selftest(smp_selftest_trials);
#endif

	while (frame_no < frames && !Settings.StopEmulation)
	{
		if (in_enabled)
			input_frame();
		S9xMainLoop();
	}
	if (wav_file)
	{
		wav_header(wav_file, wav_frames);
		fclose(wav_file);
	}
#ifdef _EE
	if (fast_sound)
	{
		spu2sim_close();
		fprintf(stderr, "fast sound: %u key-ons, %u events, %u samples (%u bytes), %u bad, %u RAM resets\n",
			spu_core_stats.key_ons, spu_core_stats.events, spu_core_stats.samples,
			spu_core_stats.sample_bytes, spu_core_stats.bad_samples, spu_core_stats.ram_resets);
	}
#endif
#ifdef ORACLE_IDLE_STATS
	fprintf(stderr, "idle: plain skips %u, handshake skips %u (%u passes)\n", ps2_idle_plain_skips,
		ps2_idle_handshake_skips, ps2_idle_handshake_passes);
	if (ps2_idle_enabled == 2)
		fprintf(stderr, "idle shadow: %u skips checked, %u wrong\n", ps2_idle_shadow_checked, ps2_idle_shadow_wrong);
	fprintf(stderr, "spc700 idle: %u skips, %.1f%% of %u cycles\n", SNES::smp.ee_idle_skips,
		100.0 * SNES::smp.ee_idle_skipped_cycles / (SNES::smp.ee_cycles ? SNES::smp.ee_cycles : 1), SNES::smp.ee_cycles);
#endif
	return 0;
}
