#pragma once
/* PS2 filesystem paths for Snes9x: base dir selection (boot device, then
 * USB -> MC0 -> host),
 * snes9x subdirectory creation, and first-ROM discovery. */

/* argv0: the ELF path the loader passed (may be NULL). */
void ps2_dirs_init(const char *argv0);
const char *ps2_base_dir(void);

/* Search the roms dir for a loadable ROM in priority order; NULL when none. */
const char *ps2_find_rom(void);
