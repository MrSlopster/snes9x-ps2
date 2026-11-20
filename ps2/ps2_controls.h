#pragma once
/* PS2 pad input for Snes9x 1.63: two controller ports (multitap optional),
 * reported into snes9x through S9xReportButton. */

void ps2_input_init(void);
void ps2_input_poll(void);
