/* Host-side test for the PS2 BGR555 pixel format.
 *
 * Build with -DPIXEL_FORMAT=BGR555 (the PS2 build setting). Checks:
 *  1. BUILD_PIXEL / DECOMPOSE_PIXEL produce the GS PSMCT16 layout
 *     (R bits 0-4, G 5-9, B 10-14, bit 15 = 0) and round-trip.
 *  2. The core's colour math (gfx.h COLOR_ADD / COLOR_SUB / brightness)
 *     commutes with swapping the outer fields: f(swap a, swap b) ==
 *     swap f(a, b). That property is what makes it valid to reuse the
 *     RGB555 masks for BGR555 without touching the renderer. */
#include <cstdio>
#include <cstdlib>

#include "snes9x.h"
#include "gfx.h"

#ifndef PIXEL_FORMAT
#error "build with -DPIXEL_FORMAT=BGR555"
#endif

uint8 brightness_cap[64];
struct SGFX GFX; /* referenced by gfx.h inlines we don't call */

static int failures = 0;

static void check(bool ok, const char *what, unsigned a, unsigned b)
{
	if (!ok)
	{
		if (failures < 20)
			printf("FAIL: %s: 0x%04x vs 0x%04x\n", what, a, b);
		failures++;
	}
}

static uint16 swap_rb(uint16 c)
{
	return (uint16)(((c & 0x1F) << 10) | (c & 0x03E0) | ((c >> 10) & 0x1F));
}

int main()
{
	/* 1. Layout. */
	check(BUILD_PIXEL(31, 0, 0) == 0x001F, "red -> bits 0-4", BUILD_PIXEL(31, 0, 0), 0x001F);
	check(BUILD_PIXEL(0, 31, 0) == 0x03E0, "green -> bits 5-9", BUILD_PIXEL(0, 31, 0), 0x03E0);
	check(BUILD_PIXEL(0, 0, 31) == 0x7C00, "blue -> bits 10-14", BUILD_PIXEL(0, 0, 31), 0x7C00);
	check(BUILD_PIXEL(31, 31, 31) == 0x7FFF, "white, alpha bit clear", BUILD_PIXEL(31, 31, 31), 0x7FFF);
	check(BUILD_PIXEL2(1, 2, 3) == BUILD_PIXEL(1, 2, 3), "BUILD_PIXEL2 == BUILD_PIXEL", BUILD_PIXEL2(1, 2, 3), BUILD_PIXEL(1, 2, 3));

	for (unsigned r = 0; r < 32; r++)
		for (unsigned g = 0; g < 32; g++)
			for (unsigned b = 0; b < 32; b++)
			{
				uint16 p = BUILD_PIXEL(r, g, b);
				unsigned r2, g2, b2;
				DECOMPOSE_PIXEL(p, r2, g2, b2);
				check(r2 == r && g2 == g && b2 == b, "decompose round-trip", p, BUILD_PIXEL(r2, g2, b2));
			}

	/* 2. Colour math symmetry. brightness_cap as ppu.cpp builds it at
	 * full brightness: identity up to 31, saturating above. */
	for (int i = 0; i < 64; i++)
		brightness_cap[i] = i > 31 ? 31 : i;

	srand(1);
	unsigned checks = 0;
	for (unsigned a = 0; a < 0x8000; a++)
	{
		for (int k = 0; k < 48; k++)
		{
			uint16 b = (uint16)(rand() & 0x7FFF);
			uint16 sa = swap_rb(a), sb = swap_rb(b);

			uint16 x, y;
			x = COLOR_ADD::fn(a, b);
			y = swap_rb(COLOR_ADD::fn(sa, sb));
			check(x == y, "COLOR_ADD::fn symmetric", x, y);

			x = COLOR_ADD::fn1_2(a, b);
			y = swap_rb(COLOR_ADD::fn1_2(sa, sb));
			check(x == y, "COLOR_ADD::fn1_2 symmetric", x, y);

			x = COLOR_SUB::fn(a, b);
			y = swap_rb(COLOR_SUB::fn(sa, sb));
			check(x == y, "COLOR_SUB::fn symmetric", x, y);

			x = COLOR_ADD_BRIGHTNESS::fn(a, b);
			y = swap_rb(COLOR_ADD_BRIGHTNESS::fn(sa, sb));
			check(x == y, "COLOR_ADD_BRIGHTNESS::fn symmetric", x, y);

			checks += 4;
		}
	}

	/* COLOR_SUB::fn1_2 indexes GFX.ZERO; its operand is built from the
	 * same per-field masks, and the ZERO table (gfx.cpp) applies the
	 * identical rule to r and b, so check the index expression. */
	for (unsigned a = 0; a < 0x8000; a += 7)
	{
		uint16 b = (uint16)(rand() & 0x7FFF);
		unsigned ia = ((a | RGB_HI_BITS_MASKx2) - (b & RGB_REMOVE_LOW_BITS_MASK)) >> 1;
		unsigned ib = ((swap_rb(a) | RGB_HI_BITS_MASKx2) - (swap_rb(b) & RGB_REMOVE_LOW_BITS_MASK)) >> 1;
		check(swap_rb(ia & 0x7FFF) == (ib & 0x7FFF), "COLOR_SUB::fn1_2 index symmetric", ia, ib);
		checks++;
	}

	printf("%u colour-math checks\n", checks);
	if (failures)
	{
		printf("RED/FAIL (%d failures)\n", failures);
		return 1;
	}
	printf("GREEN/PASS (0 failures)\n");
	return 0;
}
