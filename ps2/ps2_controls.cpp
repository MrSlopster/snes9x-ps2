/* PS2 pad input.
 *
 * libpad digital mode on ports 0/1 -> SNES joypads via S9xReportButton.
 * Multitap detection is best-effort: on failure we silently fall back to
 * two pads (there is no fatal path in this file).
 *
 * MAKE_BUTTON(pad, btn) = ((pad)<<4)|btn with pad = 1..; button indices
 * follow libretro.cpp: B=0 Y=1 Select=2 Start=3 Up=4 Down=5 Left=6
 * Right=7 A=8 X=9 L=10 R=11.
 */

extern "C" {
#include <libpad.h>
#include <libmtap.h>
}

#include <stdio.h>

#include "ps2_controls.h"
#include "snes9x.h"
#include "controls.h"

#define MAKE_BUTTON(pad, btn) (((pad) << 4) | (btn))
#define PS2_PORT_TO_PAD(port) ((port) + 1)

enum
{
    SN_B = 0,
    SN_Y = 1,
    SN_SELECT = 2,
    SN_START = 3,
    SN_UP = 4,
    SN_DOWN = 5,
    SN_LEFT = 6,
    SN_RIGHT = 7,
    SN_A = 8,
    SN_X = 9,
    SN_L = 10,
    SN_R = 11,
};

/* padRead wants a 256-byte-aligned buffer per port (see libpad docs). */
static unsigned char pad_area[2][256] __attribute__((aligned(256)));

struct snes_mask
{
    unsigned short bits;
    unsigned char snes;
};

static const struct snes_mask mask_table[12] = {
    { PAD_CROSS, SN_B },
    { PAD_SQUARE, SN_Y },
    { PAD_SELECT, SN_SELECT },
    { PAD_START, SN_START },
    { PAD_UP, SN_UP },
    { PAD_DOWN, SN_DOWN },
    { PAD_LEFT, SN_LEFT },
    { PAD_RIGHT, SN_RIGHT },
    { PAD_CIRCLE, SN_A },
    { PAD_TRIANGLE, SN_X },
    { PAD_L1, SN_L },
    { PAD_R1, SN_R },
};

static void report_port(int port, unsigned short btns)
{
    /* btns is active-low. */
    unsigned pressed = ~btns;

    for (int i = 0; i < 12; i++)
        S9xReportButton(MAKE_BUTTON(PS2_PORT_TO_PAD(port), mask_table[i].snes),
                        (pressed & mask_table[i].bits) != 0);
}

void ps2_input_init(void)
{
    padInit(0);
    mtapInit();

    /* Multitap open is best-effort; both slots failing keeps 2-pad mode. */
    mtapPortOpen(0);
    mtapPortOpen(1);

    for (int port = 0; port < 2; port++)
    {
        if (padPortOpen(port, 0, pad_area[port]) != 1)
            printf("pad: port %d open failed\n", port);

        /* Lock to digital mode: the SNES joypad is a digital pad, and
         * digital locking avoids the DS2 mode-switch dialog quirks. */
        padSetMainMode(port, 0, PAD_MMODE_DIGITAL, PAD_MMODE_LOCK);
    }

    S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
    S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
    S9xReportControllers();
}

void ps2_input_poll(void)
{
    for (int port = 0; port < 2; port++)
    {
        if (padGetState(port, 0) != PAD_STATE_STABLE)
        {
            /* Keep the last reported state while the pad is being found. */
            continue;
        }

        struct padButtonStatus status;
        padRead(port, 0, &status);

        report_port(port, status.btns);
    }
}
