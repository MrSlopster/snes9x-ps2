#pragma once
/* On-screen menus: ROM browser (massN:, mcN:, host:), settings, in-game
 * menu.
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

/* Settings screen: edits ps2_cfg, applies changes live, saves on exit. */
void ps2_menu_settings(void);

/* In-game menu (Select+Start): resume, save/load state, reset, settings,
 * load another ROM. */
enum
{
	PS2_PAUSE_RESUME,
	PS2_PAUSE_LOAD_ROM
};
int ps2_menu_pause(void);

/* Progress screen: title, a main line and a detail line; draws one frame
 * per call (cheap enough to call per read chunk). */
void ps2_menu_status(const char *title, const char *text, const char *detail);
