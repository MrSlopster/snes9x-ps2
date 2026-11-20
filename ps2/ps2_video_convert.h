#pragma once
/* Portable RGB565 -> GS 16-bit (RGBA5551 / PSM 16) pixel conversion.
 * The GS has no RGB565 PSM; the core renders 565, so the PS2 port stages
 * every frame through this conversion. Host-testable: no GS headers. */

unsigned short ps2_convert_565_to_5551(unsigned short c);

/* Convert a whole RGB565 frame into a 16-bit GS PSM 16 buffer,
 * width x height pixels; dst rows are TEX_MAX_W-pitched. */
void ps2_convert_frame_565_to_5551(unsigned short *dst, int dst_pitch,
				   const unsigned short *src, int src_pitch,
				   int width, int height);
