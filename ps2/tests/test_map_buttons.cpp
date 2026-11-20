/* Host-side test: ps2_input_map_buttons() must register all 24 pad button
 * ids with the core controls subsystem, since S9xReportButton is a no-op
 * for unmapped ids. Build with the core's controls.cpp on the host. */
#include <cstdio>
#include "snes9x.h"
#include "ps2_input_map.h"
#include "controls.h"

#define MAKE_BUTTON(pad, btn) (((pad) << 4) | (btn))

int failures = 0;

static void expect_mapped(int pad, int idx, const char *what)
{
	s9xcommand_t cmd = S9xGetMapping(MAKE_BUTTON(pad, idx));
	if (cmd.type != S9xButtonJoypad || cmd.button.joypad.buttons == 0)
	{
		printf("FAIL: pad %d button %d (%s) not mapped (type=%d)\n",
			   pad, idx, what, cmd.type);
		failures++;
	}
	else
		printf("ok: pad %d %s -> joypad mask 0x%04x\n",
			   pad, what, cmd.button.joypad.buttons);
}

int main()
{
	ps2_input_map_buttons();

	/* Spot-check every SNES function across both pads. */
	expect_mapped(1, 0, "B");
	expect_mapped(1, 1, "Y");
	expect_mapped(1, 2, "Select");
	expect_mapped(1, 3, "Start");
	expect_mapped(1, 4, "Up");
	expect_mapped(1, 5, "Down");
	expect_mapped(1, 6, "Left");
	expect_mapped(1, 7, "Right");
	expect_mapped(1, 8, "A");
	expect_mapped(1, 9, "X");
	expect_mapped(1, 10, "L");
	expect_mapped(1, 11, "R");
	expect_mapped(2, 0, "B");
	expect_mapped(2, 11, "R");

	/* The mapping must actually point at the right SNES joypad slot. */
	s9xcommand_t p2 = S9xGetMapping(MAKE_BUTTON(2, 0));
	if (p2.type == S9xButtonJoypad && p2.button.joypad.idx != 1)
	{
		printf("FAIL: pad2 B maps to joypad idx %d, expected 1\n",
			   p2.button.joypad.idx);
		failures++;
	}

	printf("%s (%d failures)\n", failures ? "RED/FAIL" : "GREEN/PASS", failures);
	return failures ? 1 : 0;
}
