/* PS2 port of Snes9x 1.63 - stub port layer (scaffold) */
#include <stdio.h>
#include <string.h>

#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "controls.h"
#include "stream.h"
#include "conffile.h"
#include "ps2_video.h"

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
}

void S9xMessage(int type, int number, const char *message)
{
	(void)type;
	printf("S9x[%d] %s\n", number, message);
}

void S9xExit(void)
{
	while (1)
		;
}

/* apu.cpp calls this when S9xInitSound needs a device; the audsrv driver
 * (Task 3) will own the real implementation. */
bool8 S9xOpenSoundDevice(void)
{
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

/* 1.63 core: S9xGetDirectory/S9xGetFilename/S9xGetFilenameInc return
 * std::string (fscompat.h), unlike the 1.52-era char* API. */
/* fscompat.cpp already provides both S9xGetFilename overloads and
 * S9xBasename(NoExt); the port provides GetDirectory and GetFilenameInc. */
std::string S9xGetDirectory(enum s9x_getdirtype t)
{
	(void)t;
	return "mass:/snes9x";
}

std::string S9xGetFilenameInc(std::string ext, enum s9x_getdirtype t)
{
	(void)ext;
	(void)t;
	return "";
}

bool8 S9xOpenSnapshotFile(const char *filepath, bool8 read_only, STREAM *file)
{
	(void)filepath;
	(void)read_only;
	(void)file;
	return FALSE;
}

void S9xCloseSnapshotFile(STREAM file)
{
	(void)file;
}

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

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("\nSnes9x " VERSION " for PS2 (scaffold build)\n");
	while (1)
		;
	return 0;
}
