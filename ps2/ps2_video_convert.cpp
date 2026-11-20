/* RGB565 (core framebuffer) -> GS PSM 16 (RGBA5551) conversion.
 *
 * The GS has no RGB565 pixel format: its 16-bit PSM is 5551. The core
 * renders 565, so every frame is converted during the staging copy.
 * 5551 layout: bit 15 = alpha (kept 0), bits 14-10 = R5, 9-5 = G5,
 * bits 4-0 = B5. 565: bits 15-11 = R5, 10-5 = G6, 4-0 = B5. */

unsigned short ps2_convert_565_to_5551(unsigned short c)
{
	unsigned r = (c >> 11) & 0x1F;
	unsigned g = (c >> 6) & 0x1F; /* drop the 6-bit green's LSB */
	unsigned b = c & 0x1F;

	return (unsigned short)((r << 10) | (g << 5) | b);
}

void ps2_convert_frame_565_to_5551(unsigned short *dst, int dst_pitch,
				   const unsigned short *src, int src_pitch,
				   int width, int height)
{
	for (int y = 0; y < height; y++)
	{
		const unsigned short *s = src + y * src_pitch;
		unsigned short *d = dst + y * dst_pitch;
		for (int x = 0; x < width; x++)
			d[x] = ps2_convert_565_to_5551(s[x]);
	}
}
