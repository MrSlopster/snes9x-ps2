/* Host-test stubs: port-supplied hooks the test TU doesn't implement. */
#include "snes9x.h"
#include "memmap.h"
#include "movie.h"
#include "conffile.h"
#include "controls.h"

void S9xMessage(int, int, const char *) {}
void S9xExit(void) {}
bool8 S9xInitUpdate(void) { return TRUE; }
bool8 S9xDeinitUpdate(int, int) { return TRUE; }
bool8 S9xContinueUpdate(int, int) { return TRUE; }
void S9xSyncSpeed(void) {}
void S9xAutoSaveSRAM(void) {}
std::string S9xGetDirectory(enum s9x_getdirtype) { return "."; }
std::string S9xGetFilenameInc(std::string, enum s9x_getdirtype) { return "x"; }
bool8 S9xOpenSnapshotFile(const char *, bool8, STREAM *) { return FALSE; }
void S9xCloseSnapshotFile(STREAM) {}
void S9xParsePortConfig(ConfigFile &, int) {}
void S9xHandlePortCommand(s9xcommand_t, int16, int16) {}
bool S9xPollButton(uint32, bool *) { return FALSE; }
bool S9xPollPointer(uint32, int16 *, int16 *) { return FALSE; }
bool S9xPollAxis(uint32, int16 *) { return FALSE; }
bool8 S9xOpenSoundDevice(void) { return TRUE; }
void S9xToggleSoundChannel(int) {}
const char *S9xStringInput(const char *) { return NULL; }
void S9xExtraUsage(void) {}
void S9xParseArg(char **, int &, int) {}
bool8 S9xMoviePlaying() { return FALSE; }
bool8 S9xMovieActive() { return FALSE; }
void S9xMovieUpdate() {}
void S9xUpdateIRQLoading() {}
void S9xMovieUpdateOnReset() {}
void S9xMovieStop(unsigned char) {}
uint32 S9xMovieGetFrameCounter() { return 0; }
void S9xMovieUpdate(bool) {}
uint8 S9xGetCrosshair(int) { return 255; }

/* Globals the core declares extern but the test TU must own. */
#include "ppu.h"
struct SPPU PPU;
struct InternalPPU IPPU;
CMemory Memory;
struct SSettings Settings;
uint8 OpenBus;

void S9xSetInfoString(const char *) {}
void S9xDrawCrosshair(const char *, uint8, uint8, int16, int16) {}
void S9xDumpSPCSnapshot() {}
void S9xFreezeGame(const char *) {}
bool8 S9xUnfreezeGame(const char *) { return FALSE; }
void S9xReset() {}
void S9xSoftReset() {}
std::string S9xGetFilename(std::string, enum s9x_getdirtype) { return ""; }
std::string S9xBasename(std::string s) { return s; }
