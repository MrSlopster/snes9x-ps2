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

	/* R: 565 top 5 bits map straight to 5551 bits 10-14. */
	expect(0xF800, 0x7C00, "red max");

	/* G: 565 green is 6 bits (0x3F max at 0x07E0); 5551 green is 5 bits,
	 * high bit of the 6 must survive. */
	expect(0x07E0, 0x03E0, "green max");
	expect(0x0020, 0x0000, "green low bit only (rounds away)");

	/* B: 565 bottom 5 bits map straight to bits 0-4. */
	expect(0x001F, 0x001F, "blue max");

	/* Bit15 in 565 is the R field's top bit, not alpha; this input has
	 * R=16, G6=33 (truncates to G5=16), B=31 -> 0x421F. */
	expect(0x841F, 0x421F, "mixed bits");

	/* Frame pass: pitch handling (row1 must come from src row1). */
	unsigned short src[16] = {
		0xF800, 0x07E0, 0, 0, 0, 0, 0, 0,	/* row 0 + fillers */
		0x001F, 0xFFFF, 0, 0, 0, 0, 0, 0,	/* row 1 + fillers */
	};
	unsigned short dst[12];
	for (int i = 0; i < 12; i++) dst[i] = 0xAAAA;
	ps2_convert_frame_565_to_5551(dst, 6, src, 8, 2, 2);
	if (dst[0] != 0x7C00 || dst[1] != 0x03E0 || dst[6] != 0x001F || dst[7] != 0x7FFF)
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
