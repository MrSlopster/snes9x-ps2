/* PS2 filesystem paths.
 *
 * Modern ps2sdk's newlib port maps POSIX file calls (fopen/stat/mkdir/
 * opendir) onto iomanX, so this file uses plain POSIX APIs over PS2 device
 * paths.
 *
 * Two places matter:
 *  - the ROM base: where ROMs are looked for first - <device>/snes9x on
 *    the device the ELF came from, else USB, memory card, host;
 *  - the data base: where ps2.cfg and the per-type subdirectories live.
 *    It has to be writable, which a device that can be read is not
 *    necessarily (USB writes have been seen to fail with I/O errors on
 *    real hardware): the ROM base if a file can be written there, else
 *    mc0:/snes9x, else mc1:/snes9x.
 * Battery saves (.srm) go next to the ROM when that folder is writable,
 * otherwise into <data base>/srm.
 *
 * NOTE: direct fileXio/fio calls fail to compile here on purpose
 * (iox_stat.h errors out when included outside ps2sdk's own libcglue). */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ps2_dirs.h"
#include "snes9x.h"
#include "memmap.h"

static char base_dir[256];     /* data base */
static char rom_base_dir[256]; /* ROM base */
static std::string save_dir;   /* .srm folder of the loaded ROM; empty: <data base>/srm */
static bool base_writable = false;

/* Can a file be created, written and closed in this folder? */
static bool dir_writable(const char *dir)
{
	std::string path = std::string(dir) + "/_s9xw.tmp";
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite("w", 1, 1, f) == 1;
	ok = (fclose(f) == 0) && ok;
	remove(path.c_str());
	return ok;
}

/* Index order must match enum s9x_getdirtype in fscompat.h:
 * DEFAULT, HOME, ROMFILENAME, ROM, SRAM, SNAPSHOT, SCREENSHOT, SPC,
 * CHEAT, PATCH, BIOS, LOG, SAT, LAST. */
static const char *subdir_names[] = {
	"",		/* DEFAULT_DIR */
	"",		/* HOME_DIR */
	"",		/* ROMFILENAME_DIR */
	"roms",		/* ROM_DIR */
	"srm",		/* SRAM_DIR */
	"saves",	/* SNAPSHOT_DIR */
	"shots",	/* SCREENSHOT_DIR */
	"spc",		/* SPC_DIR */
	"cheats",	/* CHEAT_DIR */
	"patch",	/* PATCH_DIR */
	"bios",		/* BIOS_DIR */
	"log",		/* LOG_DIR */
	"",		/* SAT_DIR */
};

/* The device the ELF was launched from (argv[0] "mass:/...", "mc0:/...",
 * "host:...") is the most likely home of the user's ROMs: try it first. */
static const char *boot_candidate(const char *argv0)
{
	if (!argv0)
		return NULL;
	if (strncmp(argv0, "mass", 4) == 0)
		return "mass:/snes9x";
	if (strncmp(argv0, "mc0:", 4) == 0)
		return "mc0:/snes9x";
	if (strncmp(argv0, "mc1:", 4) == 0)
		return "mc1:/snes9x";
	if (strncmp(argv0, "host", 4) == 0)
		return "host:/snes9x";
	return NULL;
}

void ps2_dirs_init(const char *argv0)
{
	const char *candidates[] = { boot_candidate(argv0), "mass:/snes9x", "mc0:/snes9x", "host:/snes9x" };
	const int n_candidates = sizeof(candidates) / sizeof(candidates[0]);
	int i, j;

	base_dir[0] = 0;

	for (i = 0; i < n_candidates; i++)
	{
		if (!candidates[i])
			continue;

		char probe[64];
		snprintf(probe, sizeof(probe), "%s", candidates[i]);
		char *slash = strrchr(probe, '/');
		*slash = 0; /* probe the mount root */

		struct stat st;
		if (stat(probe, &st) == 0)
		{
			strcpy(base_dir, candidates[i]);
			break;
		}
	}

	if (!base_dir[0])
		strcpy(base_dir, "mc0:/snes9x");
	strcpy(rom_base_dir, base_dir);

	mkdir(base_dir, 0777); /* EEXIST is not an error */
	base_writable = dir_writable(base_dir);
	if (!base_writable)
	{
		static const char *fallback[] = { "mc0:/snes9x", "mc1:/snes9x" };
		for (i = 0; i < 2 && !base_writable; i++)
		{
			if (!strcmp(fallback[i], rom_base_dir))
				continue;
			mkdir(fallback[i], 0777);
			if (dir_writable(fallback[i]))
			{
				strcpy(base_dir, fallback[i]);
				base_writable = true;
			}
		}
	}
	printf("ROM base: %s, data base: %s (%s)\n", rom_base_dir, base_dir, base_writable ? "writable" : "NOT writable");

	for (j = 0; j <= SAT_DIR; j++)
	{
		if (subdir_names[j][0])
		{
			char path[300];
			snprintf(path, sizeof(path), "%s/%s", base_dir, subdir_names[j]);
			mkdir(path, 0777);
		}
	}
}

const char *ps2_base_dir(void)
{
	return base_dir;
}

const char *ps2_rom_base_dir(void)
{
	return rom_base_dir;
}

bool ps2_base_writable(void)
{
	return base_writable;
}

static std::string rom_folder(void)
{
	size_t slash = Memory.ROMFilename.rfind('/');
	return slash != std::string::npos ? Memory.ROMFilename.substr(0, slash) : std::string(rom_base_dir);
}

void ps2_dirs_rom_loaded(void)
{
	std::string folder = rom_folder();
	save_dir = dir_writable(folder.c_str()) ? folder : std::string();
	printf("saves for this ROM: %s\n", ps2_save_dir());
}

const char *ps2_save_dir(void)
{
	static std::string dir;
	dir = save_dir.empty() ? std::string(base_dir) + "/srm" : save_dir;
	return dir.c_str();
}

std::string ps2_sram_load_path(void)
{
	/* Where it would be saved; failing that, a copy next to the ROM (put
	 * there from a PC, say) on a device that cannot be written. */
	std::string path = S9xGetFilename(".srm", SRAM_DIR);
	struct stat st;
	if (stat(path.c_str(), &st) == 0)
		return path;
	std::string beside = S9xGetFilename(".srm", ROMFILENAME_DIR);
	if (stat(beside.c_str(), &st) == 0)
		return beside;
	return path;
}

static const char *rom_exts[] = { ".sfc", ".smc", ".zip", ".jma", ".bin", ".fig", ".swc", NULL };

static int has_ext(const char *name, const char *ext)
{
	size_t ln = strlen(name);
	size_t le = strlen(ext);
	return ln > le && strcasecmp(name + ln - le, ext) == 0;
}

const char *ps2_find_rom(void)
{
	static char found[300];
	std::string roms = std::string(rom_base_dir) + "/roms";

	DIR *d = opendir(roms.c_str());
	if (!d)
	{
		printf("roms dir not found under %s\n", rom_base_dir);
		return NULL;
	}

	/* Priority: one pass per extension, first hit wins. */
	for (int e = 0; rom_exts[e]; e++)
	{
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL)
		{
			if (has_ext(ent->d_name, rom_exts[e]))
			{
				snprintf(found, sizeof(found), "%s/%s", roms.c_str(), ent->d_name);
				closedir(d);
				return found;
			}
		}
		rewinddir(d);
	}

	closedir(d);
	return NULL;
}

/* ===== snes9x port hooks ===== */

/* 1.63: S9xGetDirectory / S9xGetFilenameInc are std::string (fscompat.h);
 * both S9xGetFilename overloads live in fscompat.cpp. */
std::string S9xGetDirectory(enum s9x_getdirtype t)
{
	if (t == ROMFILENAME_DIR)
	{
		/* Memory.ROMFilename is a std::string in 1.63. */
		size_t slash = Memory.ROMFilename.rfind('/');
		if (slash != std::string::npos)
			return Memory.ROMFilename.substr(0, slash);
		return rom_base_dir;
	}

	if (t == SRAM_DIR)
		return ps2_save_dir();

	if (subdir_names[t][0])
		return std::string(base_dir) + "/" + subdir_names[t];

	return base_dir;
}

std::string S9xGetFilenameInc(std::string ext, enum s9x_getdirtype t)
{
	std::string base = S9xGetFilename(ext, t);
	const char *p = base.c_str();

	/* Insert ".<i>" before the existing extension. */
	char dirpart[384], extpart[128];
	dirpart[0] = 0;
	extpart[0] = 0;
	strncpy(dirpart, p, sizeof(dirpart) - 1);
	dirpart[sizeof(dirpart) - 1] = 0;
	char *dot = strrchr(dirpart, '.');
	if (dot)
	{
		strcpy(extpart, dot);
		*dot = 0;
	}

	for (int i = 0; i < 1000; i++)
	{
		char candidate[512];
		snprintf(candidate, sizeof(candidate), "%s.%d%s", dirpart, i, extpart);
		struct stat st;
		if (stat(candidate, &st) != 0)
			return std::string(candidate);
	}

	return std::string(p);
}

/* Snapshot streams: plain FILE streams via the core's Stream classes
 * (mirrors unix/unix.cpp; 1.63 snapshots are uncompressed). */
bool8 S9xOpenSnapshotFile(const char *filename, bool8 read_only, STREAM *file)
{
	if (read_only)
	{
		if ((*file = OPEN_STREAM(filename, "rb")))
			return true;
	}
	else
	{
		if ((*file = OPEN_STREAM(filename, "wb")))
			return true;
	}

	fprintf(stderr, "Couldn't open snapshot file: %s\n", filename);
	return false;
}

void S9xCloseSnapshotFile(STREAM file)
{
	CLOSE_STREAM(file);
}
