/* ROM loading with on-screen progress; mirrors CMemory::LoadROM
 * (memmap.cpp) for plain files, reading in chunks. */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "snes9x.h"
#include "memmap.h"
#include "snapshot.h"

#include "ps2_rom.h"
#include "ps2_menu.h"

#define READ_CHUNK (256 * 1024)

static bool has_ext(const char *path, const char *ext)
{
	size_t lp = strlen(path), le = strlen(ext);
	return lp > le && strcasecmp(path + lp - le, ext) == 0;
}

/* Read the whole file into Memory.ROM; returns bytes read (0 = error). */
static uint32 read_rom_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		printf("rom: cannot open %s\n", path);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	const uint32 limit = CMemory::MAX_ROM_SIZE + 0x200;
	uint32 total = 0;
	char line[64];

	while (total < limit)
	{
		uint32 want = limit - total < READ_CHUNK ? limit - total : READ_CHUNK;
		size_t got = fread(Memory.ROM + total, 1, want, f);
		total += (uint32)got;

		snprintf(line, sizeof(line), "Reading %.1f / %.1f MB", total / 1048576.0,
			 file_size > 0 ? file_size / 1048576.0 : 0.0);
		ps2_menu_status("Loading", path, line);

		if (got < want)
			break; /* EOF or error */
	}

	if (ferror(f))
	{
		printf("rom: read error after %u bytes\n", (unsigned)total);
		total = 0;
	}
	fclose(f);
	return total;
}

bool ps2_load_rom(const char *path)
{
	if (has_ext(path, ".zip") || has_ext(path, ".jma") || has_ext(path, ".msu1"))
	{
		ps2_menu_status("Loading", path, "Decompressing...");
		return Memory.LoadROM(path);
	}

	S9xResetSaveTimer(FALSE);

	/* CMemory::LoadROM re-reads the file whenever LoadROMInt asks for
	 * another pass; cap the passes so a bad file cannot loop forever. */
	for (int pass = 0; pass < 4; pass++)
	{
		memset(Memory.ROM, 0, CMemory::MAX_ROM_SIZE);
		memset(&Multi, 0, sizeof(Multi));
		memset(Memory.NSRTHeader, 0, sizeof(Memory.NSRTHeader));
		Memory.HeaderCount = 0;

		uint32 size = read_rom_file(path);
		if (!size)
			return false;

		Memory.ROMFilename = path;
		int32 total = (int32)Memory.HeaderRemove(size, Memory.ROM);

		ps2_menu_status("Loading", path, "Checking for patches...");
		Memory.CheckForAnyPatch(path, Memory.HeaderCount != 0, total);

		if (Memory.HeaderCount == 0)
			S9xMessage(S9X_INFO, S9X_HEADERS_INFO, "No ROM file header found.");

		ps2_menu_status("Loading", path, "Initializing...");
		if (Memory.LoadROMInt(total))
			return true;
	}
	return false;
}
