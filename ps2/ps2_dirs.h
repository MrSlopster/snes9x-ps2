#pragma once
/* PS2 filesystem paths for Snes9x: base dir selection (boot device, then
 * USB -> MC0 -> host),
 * snes9x subdirectory creation, and first-ROM discovery. */

/* argv0: the ELF path the loader passed (may be NULL). */
void ps2_dirs_init(const char *argv0);
/* Where ps2.cfg and the data subdirectories are: a writable place (see
 * ps2_dirs.cpp), which may be the memory card when the boot device cannot
 * be written. */
const char *ps2_base_dir(void);
bool ps2_base_writable(void);
/* Where ROMs are looked for first (<device the ELF came from>/snes9x). */
const char *ps2_rom_base_dir(void);

/* After a ROM is loaded: decide where its battery save goes - next to
 * the ROM if that folder can be written, else <base>/srm. */
void ps2_dirs_rom_loaded(void);
const char *ps2_save_dir(void);
#ifdef __cplusplus
#include <string>
/* The .srm to load: the save location, else a copy next to the ROM. */
std::string ps2_sram_load_path(void);
#endif

/* Search the roms dir for a loadable ROM in priority order; NULL when none. */
const char *ps2_find_rom(void);
