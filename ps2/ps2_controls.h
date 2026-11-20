#pragma once
/* PS2 pad input for Snes9x 1.63: two controller ports (multitap optional),
 * reported into snes9x through S9xReportButton. */

void ps2_input_init(void);
void ps2_input_poll(void);

/* Raw pad state for the ROM browser: PAD_* bits (libpad.h), pressed = 1.
 * Returns 0 while the pad is not ready or input is unavailable. */
unsigned ps2_pad_held(int port);

/* True once libpad is initialized (padman loaded and ports opened). */
bool ps2_controls_available(void);
