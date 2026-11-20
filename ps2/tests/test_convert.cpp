/* Host-side test for the 565 -> 5551 staging conversion. */
#include <cstdio>
#include "ps2_video_convert.h"

int failures = 0;

static void expect(unsigned in, unsigned out, const char *what)
{
	unsigned got = ps2_convert_565_to_5551((unsigned short)in);
	if (got != out)
	{
		printf("FAIL: %s: 0x%04x -> 0x%04x, expected 0x%04x\n", what, in, got, out);
		failures++;
	}
	else
		printf("ok: %s: 0x%04x -> 0x%04x\n", what, in, got);
}

int main()
{
	expect(0x0000, 0x0000, "black");
	expect(0xFFFF, 0x7FFF, "white");

	/* GS PSM 16 (RGBA5551) layout: R = bits 0-4, G = bits 5-9,
	 * B = bits 10-14, A = bit 15 (kept 0). */
	/* 565 red max (0xF800) -> 5551 red sits in the LOW 5 bits. */
	expect(0xF800, 0x001F, "red max");

	/* G: 565 green is 6 bits (0x3F max at 0x07E0); 5551 green is 5 bits,
	 * high bit of the 6 must survive. */
	expect(0x07E0, 0x03E0, "green max");
	expect(0x0020, 0x0000, "green low bit only (rounds away)");

	/* 565 blue max (0x001F) -> 5551 blue sits in bits 10-14. */
	expect(0x001F, 0x7C00, "blue max");

	/* 0x841F: R5=16, G6=33 (truncates to G5=16), B5=31
	 * -> (31<<10)|(16<<5)|16 = 0x7E10. */
	expect(0x841F, 0x7E10, "mixed bits");

	/* Frame pass: pitch handling (row1 must come from src row1). */
	unsigned short src[16] = {
		0xF800, 0x07E0, 0, 0, 0, 0, 0, 0,	/* row 0 + fillers */
		0x001F, 0xFFFF, 0, 0, 0, 0, 0, 0,	/* row 1 + fillers */
	};
	unsigned short dst[12];
	for (int i = 0; i < 12; i++) dst[i] = 0xAAAA;
	ps2_convert_frame_565_to_5551(dst, 6, src, 8, 2, 2);
	if (dst[0] != 0x001F || dst[1] != 0x03E0 || dst[6] != 0x7C00 || dst[7] != 0x7FFF)
	{
		printf("FAIL: frame layout wrong: %04x %04x %04x %04x\n",
			   dst[0], dst[1], dst[6], dst[7]);
		failures++;
	}
	if (dst[2] != 0xAAAA || dst[11] != 0xAAAA)
	{
		printf("FAIL: frame conversion wrote outside width\n");
		failures++;
	}

	printf("%s (%d failures)\n", failures ? "RED/FAIL" : "GREEN/PASS", failures);
	return failures ? 1 : 0;
}
