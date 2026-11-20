#pragma once
/* ROM loading for the PS2 port with on-screen progress.
 *
 * Plain ROM files are read in chunks straight into Memory.ROM (the core's
 * own FileLoader issues one MAX_ROM_SIZE+512 = 16 MB read, a single huge
 * request through fileXio/bdm/USB with no progress). .zip/.jma/.msu1 go
 * through the core's loader, which has to decompress them. */

bool ps2_load_rom(const char *path);
