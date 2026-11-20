#pragma once
/* On-screen ROM browser: USB (massN:), memory cards (mcN:) and host:.
 * Needs S9xGraphicsInit (draws into GFX.Screen), ps2_video_init and
 * ps2_input_init. */

#include <string>

/* Browse for a ROM, starting in start_dir (NULL or unreadable: the device
 * list) with the cursor on 'select_name' if it is listed there. 'message',
 * if non-NULL, is shown on the status line (e.g. why the previous ROM
 * failed to load). Returns the chosen ROM path; returns an empty string
 * only if no pad is available to drive the menu. */
std::string ps2_menu_pick_rom(const char *start_dir, const char *message,
			      const char *select_name = NULL);
