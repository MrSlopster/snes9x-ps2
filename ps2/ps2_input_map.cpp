#include "snes9x.h"
#include "controls.h"

#define MAKE_BUTTON(pad, btn) (((pad) << 4) | (btn))
#define PAD_1 1
#define PAD_2 2

/* Button indices follow libretro.cpp: B=0 Y=1 Select=2 Start=3 Up=4
 * Down=5 Left=6 Right=7 A=8 X=9 L=10 R=11. */
static const char *names[2][12] = {
	{
		"Joypad1 B", "Joypad1 Y", "Joypad1 Select", "Joypad1 Start",
		"Joypad1 Up", "Joypad1 Down", "Joypad1 Left", "Joypad1 Right",
		"Joypad1 A", "Joypad1 X", "Joypad1 L", "Joypad1 R",
	},
	{
		"Joypad2 B", "Joypad2 Y", "Joypad2 Select", "Joypad2 Start",
		"Joypad2 Up", "Joypad2 Down", "Joypad2 Left", "Joypad2 Right",
		"Joypad2 A", "Joypad2 X", "Joypad2 L", "Joypad2 R",
	},
};

/* S9xReportButton is a no-op for ids that were never mapped with
 * S9xMapButton, so this registration is what actually connects the pads
 * to the core (libretro map_buttons() equivalent for two joypads). */
void ps2_input_map_buttons(void)
{
	S9xUnmapAllControls();

	for (int pad = 0; pad < 2; pad++)
	{
		for (int i = 0; i < 12; i++)
		{
			s9xcommand_t cmd = S9xGetCommandT(names[pad][i]);
			S9xMapButton(MAKE_BUTTON(PAD_1 + pad, i), cmd, false);
		}
	}
}
