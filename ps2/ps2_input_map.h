#pragma once
/* Portable button->command registration for the PS2 port (host-testable:
 * no libpad/libmtap includes here - core controls.h only). */

void ps2_input_map_buttons(void);
