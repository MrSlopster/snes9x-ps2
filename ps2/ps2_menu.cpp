/* On-screen menus: ROM browser, settings, in-game menu.
 *
 * Renders text with the core's 8x9 bitmap font (font.h) into GFX.Screen
 * as a 512x224 BGR555 frame and shows it through the normal video path.
 * Input comes straight from libpad (pad 1).
 *
 * Browser:  up/down move, L1/R1 page, Cross open/load, Circle back,
 *           Start settings.
 * Settings: up/down move, left/right change, Circle back (saves).
 * In-game:  up/down move, Cross select, Circle resume.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include <libpad.h>
}

#include "snes9x.h"
#include "memmap.h"
#include "gfx.h"
#include "../font.h" /* the core's bitmap font; ps2sdk has its own font.h */

#include "ps2_menu.h"
#include "ps2_video.h"
#include "ps2_controls.h"
#include "ps2_config.h"
#include "ps2_net.h"
#include "ps2_spu.h"
#include "ps2_dirs.h"
#include "fscompat.h"
#include "snapshot.h"
#include "cpuexec.h"

#define MENU_W 512
#define MENU_H 224
#define CHAR_W 8
#define CHAR_H 9
#define LINE_H 10
#define COLS (MENU_W / CHAR_W)        /* 64 */
#define LIST_TOP 3                    /* first list row */
#define LIST_ROWS 16                  /* rows 3..18 */
#define STATUS_ROW 20
#define HELP_ROW 21

struct Entry
{
	std::string name;
	bool dir;
};

static const char *rom_exts[] = { ".sfc", ".smc", ".zip", ".jma", ".bin", ".fig", ".swc", NULL };

static uint16 col_bg, col_text, col_dim, col_title, col_sel_bg, col_dir, col_err;

static void init_colours(void)
{
	col_bg = BUILD_PIXEL(2, 3, 8);
	col_text = BUILD_PIXEL(28, 28, 28);
	col_dim = BUILD_PIXEL(16, 16, 18);
	col_title = BUILD_PIXEL(31, 26, 8);
	col_sel_bg = BUILD_PIXEL(6, 10, 22);
	col_dir = BUILD_PIXEL(14, 26, 31);
	col_err = BUILD_PIXEL(31, 10, 10);
}

/* ---- drawing ------------------------------------------------------- */

static void fill_rows(int row, int nrows, uint16 c)
{
	for (int y = row * LINE_H; y < (row + nrows) * LINE_H && y < MENU_H; y++)
	{
		uint16 *p = GFX.Screen + y * GFX.RealPPL;
		for (int x = 0; x < MENU_W; x++)
			p[x] = c;
	}
}

static void draw_char(int col, int row, unsigned char ch, uint16 fg)
{
	if (ch < 32 || ch > 32 + 14 * 16 - 1)
		ch = '?';

	int gi = ch - 32;
	int gr = gi / 16, gc = gi % 16;
	int x0 = col * CHAR_W, y0 = row * LINE_H;

	for (int y = 0; y < CHAR_H; y++)
	{
		const char *line = font[gr * CHAR_H + y] + gc * CHAR_W;
		uint16 *p = GFX.Screen + (y0 + y) * GFX.RealPPL + x0;
		for (int x = 0; x < CHAR_W; x++)
		{
			if (line[x] == '#')
				p[x] = fg;
			else if (line[x] == '.')
				p[x] = 0; /* outline: keeps text readable on any bg */
		}
	}
}

static void draw_text(int col, int row, const char *s, uint16 fg, int max_cols = COLS)
{
	int n = (int)strlen(s);
	bool cut = n > max_cols;
	for (int i = 0; i < n && i < max_cols; i++)
		draw_char(col + i, row, (unsigned char)((cut && i == max_cols - 1) ? '~' : s[i]), fg);
}

/* Text on top of a game frame (profiler): yellow with black outline,
 * clipped to the rendered frame size. */
void ps2_menu_overlay_text(int x, int y, const char *s, int width, int height)
{
	uint16 fg = BUILD_PIXEL(31, 31, 0);
	for (int i = 0; s[i]; i++)
	{
		int x0 = x + i * CHAR_W;
		if (x0 + CHAR_W > width || y + CHAR_H > height)
			return;

		unsigned char ch = (unsigned char)s[i];
		if (ch < 32 || ch > 32 + 14 * 16 - 1)
			ch = '?';
		int gi = ch - 32, gr = gi / 16, gc = gi % 16;
		for (int yy = 0; yy < CHAR_H; yy++)
		{
			const char *line = font[gr * CHAR_H + yy] + gc * CHAR_W;
			uint16 *p = GFX.Screen + (y + yy) * GFX.RealPPL + x0;
			for (int xx = 0; xx < CHAR_W; xx++)
			{
				if (line[xx] == '#')
					p[xx] = fg;
				else if (line[xx] == '.')
					p[xx] = 0;
			}
		}
	}
}

/* ---- filesystem ---------------------------------------------------- */

static bool has_rom_ext(const char *name)
{
	size_t ln = strlen(name);
	for (int e = 0; rom_exts[e]; e++)
	{
		size_t le = strlen(rom_exts[e]);
		if (ln > le && strcasecmp(name + ln - le, rom_exts[e]) == 0)
			return true;
	}
	return false;
}

static bool dir_exists(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return false;
	closedir(d);
	return true;
}

/* Device roots that are present right now. */
static void list_devices(std::vector<Entry> &out)
{
	out.clear();
	char path[16];

	/* bdm numbers USB drives mass0:, mass1:, ... */
	for (int i = 0; i < 4; i++)
	{
		snprintf(path, sizeof(path), "mass%d:/", i);
		if (dir_exists(path))
			out.push_back({ path, true });
	}
	for (int i = 0; i < 2; i++)
	{
		snprintf(path, sizeof(path), "mc%d:/", i);
		if (dir_exists(path))
			out.push_back({ path, true });
	}
	if (dir_exists("host:/"))
		out.push_back({ "host:/", true });
}

static bool entry_less(const Entry &a, const Entry &b)
{
	if (a.dir != b.dir)
		return a.dir;
	return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

static bool list_dir(const std::string &path, std::vector<Entry> &out)
{
	out.clear();
	DIR *d = opendir(path.c_str());
	if (!d)
		return false;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL)
	{
		const char *n = ent->d_name;
		if (!strcmp(n, ".") || !strcmp(n, ".."))
			continue;

		bool is_dir;
		if (ent->d_type == DT_DIR)
			is_dir = true;
		else if (ent->d_type == DT_REG)
			is_dir = false;
		else
		{
			struct stat st;
			std::string full = path + n;
			is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
		}

		if (is_dir || has_rom_ext(n))
			out.push_back({ n, is_dir });
	}
	closedir(d);

	std::sort(out.begin(), out.end(), entry_less);
	return true;
}

/* "mass0:/a/b/" -> "mass0:/a/"; a device root returns "" (device list). */
static std::string parent_of(const std::string &path)
{
	size_t colon = path.find(':');
	if (colon == std::string::npos || path.size() <= colon + 2)
		return "";
	size_t slash = path.rfind('/', path.size() - 2);
	if (slash == std::string::npos || slash <= colon)
		return "";
	return path.substr(0, slash + 1);
}

/* ---- input --------------------------------------------------------- */

/* Edge-triggered buttons with auto-repeat for the d-pad and page keys. */
static unsigned poll_buttons(void)
{
	static unsigned prev = 0;
	static int held_frames = 0;
	const unsigned repeatable = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1;

	unsigned now = ps2_pad_held(0);
	unsigned pressed = now & ~prev;

	if (now & repeatable && now == prev)
	{
		if (++held_frames > 18 && (held_frames % 4) == 0)
			pressed |= now & repeatable;
	}
	else
		held_frames = 0;

	prev = now;
	return pressed;
}

/* Wait until every button is up, so a held combo (Select+Start) or the
 * button that opened a menu cannot trigger something in the next one. */
static void wait_release(void)
{
	for (int i = 0; i < 300 && ps2_pad_held(0); i++)
		ps2_video_sync();
	poll_buttons(); /* resync edge detection */
}

/* ---- shared list screen ------------------------------------------- */

struct Row
{
	std::string left;
	std::string right; /* value column (settings) */
	uint16 colour;
};

static void render_list(const char *title, const std::string &subtitle,
			const std::vector<Row> &rows, int sel, int top,
			const char *empty_text, const std::string &status,
			bool status_err, const char *help)
{
	/* Menus draw into the core's frame buffer: nothing to draw into before
	 * S9xGraphicsInit (a null GFX.Screen would scribble over the EE kernel
	 * in low RAM). */
	if (!GFX.Screen)
		return;

	fill_rows(0, MENU_H / LINE_H + 1, col_bg);

	draw_text(1, 0, title, col_title);
	draw_text(1, 1, subtitle.c_str(), col_dim, COLS - 10);

	if (rows.empty() && empty_text)
		draw_text(2, LIST_TOP, empty_text, col_dim);

	for (int r = 0; r < LIST_ROWS && top + r < (int)rows.size(); r++)
	{
		const Row &e = rows[top + r];
		int row = LIST_TOP + r;
		if (top + r == sel)
			fill_rows(row, 1, col_sel_bg);

		int left_max = e.right.empty() ? COLS - 3 : 30;
		draw_text(2, row, e.left.c_str(), e.colour, left_max);
		if (!e.right.empty())
			draw_text(34, row, e.right.c_str(), col_text, COLS - 35);
	}

	if ((int)rows.size() > LIST_ROWS)
	{
		char pos[32];
		snprintf(pos, sizeof(pos), "%d/%d", sel + 1, (int)rows.size());
		draw_text(COLS - 1 - (int)strlen(pos), 1, pos, col_dim);
	}

	if (!status.empty())
		draw_text(1, STATUS_ROW, status.c_str(), status_err ? col_err : col_text, COLS - 2);

	draw_text(1, HELP_ROW, help, col_dim);

	ps2_video_frame(GFX.Screen, GFX.RealPPL, MENU_W, MENU_H);
	ps2_video_sync(); /* menu pacing: one step per vblank */
}

static void keep_visible(int sel, int &top)
{
	if (sel < top)
		top = sel;
	if (sel >= top + LIST_ROWS)
		top = sel - LIST_ROWS + 1;
}

void ps2_menu_status(const char *title, const char *text, const char *detail)
{
	init_colours();

	std::vector<Row> rows;
	rows.push_back({ text, "", col_text });
	if (detail && detail[0])
		rows.push_back({ detail, "", col_dim });

	render_list(title, "", rows, -1, 0, NULL, "", false, "");
}

/* ---- settings ----------------------------------------------------- */

static const char *video_mode_names[PS2_VIDEO_MODE_COUNT] = {
	"Auto (console region)", "NTSC 480i", "PAL 576i", "NTSC 240p", "PAL 288p", "480p (component)"
};
static const char *filter_names[PS2_FILTER_COUNT] = { "Smooth", "Sharp" };
static const char *overlay_names[PS2_OVERLAY_COUNT] = { "Off", "FPS", "Profiler", "Profiler+PMC" };
static const char *sound_names[PS2_SOUND_COUNT] = { "Off", "Accurate", "Fast (SPU2 voices)" };
static const char *renderer_names[PS2_RENDERER_COUNT] = { "Software", "GS", "GS + check vs software (slow)" };

enum
{
	SET_VIDEO,
	SET_FILTER,
	SET_FRAMESKIP,
	SET_FPS,
	SET_SOUND,
	SET_RENDERER,
	SET_NETLOG,
	SET_DEFAULTS,
	SET_BACK,
	SET_COUNT
};

static std::string frameskip_name(int fs)
{
	if (fs == PS2_FRAMESKIP_AUTO)
		return "Auto";
	char b[8];
	snprintf(b, sizeof(b), "%d", fs);
	return b;
}

static int wrap(int v, int lo, int hi)
{
	return v < lo ? hi : (v > hi ? lo : v);
}

/* After a video mode change: keep it only if the user can see this and
 * presses Cross within ~10 s; anything else restores the old mode. */
static bool confirm_video_mode(int old_mode)
{
	wait_release();
	for (int frames = 600; frames > 0; frames--)
	{
		std::vector<Row> rows;
		char msg[64];
		snprintf(msg, sizeof(msg), "Reverting in %d s", frames / 60 + 1);
		rows.push_back({ "Keep this video mode?", "", col_title });
		rows.push_back({ "Press X to keep it.", "", col_text });
		rows.push_back({ msg, "", col_dim });
		render_list("Video mode", video_mode_names[ps2_cfg.video_mode], rows, -1, 0,
			    NULL, "", false, "X keep   O revert");

		unsigned b = poll_buttons();
		if (b & PAD_CROSS)
			return true;
		if (b & (PAD_CIRCLE | PAD_TRIANGLE))
			break;
	}

	ps2_cfg.video_mode = old_mode;
	ps2_video_set_mode(old_mode);
	return false;
}

void ps2_menu_settings(void)
{
	init_colours();
	wait_release();

	int sel = 0, top = 0;
	std::string status;
	bool status_err = false;

	/* Left/Right only browses video modes; X applies the shown one (then
	 * the keep/revert prompt). Applying on every step would force a
	 * confirm timeout per mode while scrolling past unwanted ones. */
	int video_choice = ps2_cfg.video_mode;

	while (1)
	{
		std::vector<Row> rows(SET_COUNT);
		rows[SET_VIDEO] = { "Video mode", std::string("< ") + video_mode_names[video_choice] + " >",
				    video_choice != ps2_cfg.video_mode ? col_title : col_text };
		rows[SET_FILTER] = { "Screen filter", std::string("< ") + filter_names[ps2_cfg.filter] + " >", col_text };
		rows[SET_FRAMESKIP] = { "Frame skip", "< " + frameskip_name(ps2_cfg.frameskip) + " >", col_text };
		rows[SET_FPS] = { "Overlay", std::string("< ") + overlay_names[ps2_cfg.overlay] + " >", col_text };
		rows[SET_SOUND] = { "Sound", std::string("< ") + sound_names[ps2_cfg.sound] + " >", col_text };
		rows[SET_RENDERER] = { "Renderer", std::string("< ") + renderer_names[ps2_cfg.renderer] + " >", col_text };
		rows[SET_NETLOG] = { "Network log", !ps2_cfg.netlog ? "< Off >" : ps2_net_ready() ? "< On >" : "< On (not connected) >", col_text };
		rows[SET_DEFAULTS] = { "Reset to defaults", "", col_dir };
		rows[SET_BACK] = { "Back", "", col_dir };

		keep_visible(sel, top);
		render_list("Settings", std::string(ps2_base_dir()) + "/ps2.cfg", rows, sel, top,
			    NULL, status, status_err, "Left/Right change   X select   O back");

		unsigned b = poll_buttons();
		int dir = (b & PAD_RIGHT) ? 1 : (b & PAD_LEFT) ? -1 : 0;
		bool activate = (b & PAD_CROSS) != 0;

		if (b & PAD_UP)
			sel = wrap(sel - 1, 0, SET_COUNT - 1);
		else if (b & PAD_DOWN)
			sel = wrap(sel + 1, 0, SET_COUNT - 1);
		else if (b & (PAD_CIRCLE | PAD_TRIANGLE) || (activate && sel == SET_BACK))
			break;
		else if (sel == SET_VIDEO && (dir || activate))
		{
			status_err = false;
			if (dir)
			{
				video_choice = wrap(video_choice + dir, 0, PS2_VIDEO_MODE_COUNT - 1);
				status = video_choice != ps2_cfg.video_mode ? "Press X to apply this video mode" : "";
			}
			else if (video_choice != ps2_cfg.video_mode)
			{
				int old = ps2_cfg.video_mode;
				ps2_cfg.video_mode = video_choice;
				ps2_video_set_mode(video_choice);
				status = confirm_video_mode(old) ? "" : "Video mode reverted";
				video_choice = ps2_cfg.video_mode;
				wait_release();
			}
		}
		else if (dir || activate)
		{
			if (activate && dir == 0)
				dir = 1;
			status.clear();
			status_err = false;

			switch (sel)
			{
			case SET_FILTER:
				ps2_cfg.filter = wrap(ps2_cfg.filter + dir, 0, PS2_FILTER_COUNT - 1);
				ps2_video_set_filter(ps2_cfg.filter);
				break;
			case SET_FRAMESKIP:
				ps2_cfg.frameskip = wrap(ps2_cfg.frameskip + dir, PS2_FRAMESKIP_AUTO, 4);
				break;
			case SET_FPS:
				ps2_cfg.overlay = wrap(ps2_cfg.overlay + dir, 0, PS2_OVERLAY_COUNT - 1);
				break;
			case SET_SOUND:
				ps2_cfg.sound = wrap(ps2_cfg.sound + dir, 0, PS2_SOUND_COUNT - 1);
				if (ps2_cfg.sound == PS2_SOUND_FAST && !ps2_spu_available())
					ps2_cfg.sound = dir > 0 ? PS2_SOUND_OFF : PS2_SOUND_ACCURATE;
				break;
			case SET_RENDERER:
				ps2_cfg.renderer = wrap(ps2_cfg.renderer + dir, 0, PS2_RENDERER_COUNT - 1);
				break;
			case SET_NETLOG:
				ps2_cfg.netlog = !ps2_cfg.netlog;
				if (ps2_cfg.netlog && !ps2_net_start())
					status = "Network log: no connection", status_err = true;
				wait_release();
				break;
			case SET_DEFAULTS:
				if (activate)
				{
					std::string keep_dir = ps2_cfg.last_dir;
					int old_mode = ps2_cfg.video_mode;
					ps2_config_defaults();
					ps2_cfg.last_dir = keep_dir;
					ps2_video_set_filter(ps2_cfg.filter);
					if (ps2_cfg.video_mode != old_mode)
					{
						ps2_video_set_mode(ps2_cfg.video_mode);
						if (!confirm_video_mode(old_mode))
							status = "Video mode reverted";
						wait_release();
					}
					video_choice = ps2_cfg.video_mode;
					if (status.empty())
						status = "Defaults restored";
				}
				break;
			}
			ps2_config_apply_core();
		}
	}

	if (!ps2_config_save())
	{
		/* Saving is best effort (read-only device): settings still apply
		 * for this session. */
		printf("settings: not saved\n");
	}
	wait_release();
}

/* ---- in-game menu ------------------------------------------------- */

enum
{
	PAUSE_ITEM_RESUME,
	PAUSE_ITEM_SAVE,
	PAUSE_ITEM_LOAD,
	PAUSE_ITEM_RESET,
	PAUSE_ITEM_SETTINGS,
	PAUSE_ITEM_ROM,
	PAUSE_ITEM_COUNT
};

int ps2_menu_pause(void)
{
	init_colours();
	wait_release();

	int sel = 0, top = 0;
	std::string status;
	bool status_err = false;
	std::string state_file = S9xGetFilename(".000", SNAPSHOT_DIR);
	std::string game = Memory.ROMFilename.substr(Memory.ROMFilename.rfind('/') + 1);

	while (1)
	{
		std::vector<Row> rows(PAUSE_ITEM_COUNT);
		rows[PAUSE_ITEM_RESUME] = { "Resume", "", col_text };
		rows[PAUSE_ITEM_SAVE] = { "Save state", "", col_text };
		rows[PAUSE_ITEM_LOAD] = { "Load state", "", col_text };
		rows[PAUSE_ITEM_RESET] = { "Reset game", "", col_text };
		rows[PAUSE_ITEM_SETTINGS] = { "Settings", "", col_dir };
		rows[PAUSE_ITEM_ROM] = { "Load another ROM", "", col_dir };

		keep_visible(sel, top);
		render_list("Paused", game, rows, sel, top, NULL, status, status_err,
			    "X select   O resume");

		unsigned b = poll_buttons();
		if (b & PAD_UP)
			sel = wrap(sel - 1, 0, PAUSE_ITEM_COUNT - 1);
		else if (b & PAD_DOWN)
			sel = wrap(sel + 1, 0, PAUSE_ITEM_COUNT - 1);
		else if (b & (PAD_CIRCLE | PAD_TRIANGLE))
			break;
		else if (b & PAD_CROSS)
		{
			status_err = false;
			switch (sel)
			{
			case PAUSE_ITEM_RESUME:
				wait_release();
				return PS2_PAUSE_RESUME;
			case PAUSE_ITEM_SAVE:
				if (S9xFreezeGame(state_file.c_str()))
					status = "State saved";
				else
					status = "Could not save " + state_file, status_err = true;
				break;
			case PAUSE_ITEM_LOAD:
				if (S9xUnfreezeGame(state_file.c_str()))
				{
					wait_release();
					return PS2_PAUSE_RESUME;
				}
				status = "No saved state", status_err = true;
				break;
			case PAUSE_ITEM_RESET:
				S9xSoftReset();
				wait_release();
				return PS2_PAUSE_RESUME;
			case PAUSE_ITEM_SETTINGS:
				ps2_menu_settings();
				break;
			case PAUSE_ITEM_ROM:
				wait_release();
				return PS2_PAUSE_LOAD_ROM;
			}
		}
	}

	wait_release();
	return PS2_PAUSE_RESUME;
}

/* ---- browser ------------------------------------------------------- */

std::string ps2_menu_pick_rom(const char *start_dir, const char *message,
			      const char *select_name)
{
	/* No pad driver: nothing could ever drive the menu. */
	if (!ps2_controls_available())
		return "";

	init_colours();
	wait_release();

	std::string cwd;
	std::vector<Entry> items;
	std::string status = message ? message : "";
	bool status_err = message != NULL;

	if (start_dir && start_dir[0])
	{
		cwd = start_dir;
		if (cwd[cwd.size() - 1] != '/')
			cwd += '/';
		if (!list_dir(cwd, items))
			cwd.clear();
	}
	if (cwd.empty())
		list_devices(items);

	int sel = 0, top = 0;
	if (select_name && !cwd.empty())
	{
		for (int i = 0; i < (int)items.size(); i++)
			if (items[i].name == select_name)
				sel = i;
	}

	while (1)
	{
		keep_visible(sel, top);

		std::vector<Row> rows;
		for (const Entry &e : items)
			rows.push_back({ e.dir && !cwd.empty() ? e.name + "/" : e.name, "", e.dir ? col_dir : col_text });

		render_list("Snes9x " VERSION " for PS2 - select a ROM", cwd.empty() ? "Devices" : cwd,
			    rows, sel, top,
			    cwd.empty() ? "(no storage devices found)" : "(no ROMs or folders here)",
			    status, status_err, "X open   O back   L1/R1 page   START settings");

		unsigned b = poll_buttons();
		int n = (int)items.size();

		if (b & PAD_START)
		{
			ps2_menu_settings();
			wait_release();
		}
		else if (b & PAD_UP && n)
			sel = (sel + n - 1) % n;
		else if (b & PAD_DOWN && n)
			sel = (sel + 1) % n;
		else if (b & (PAD_L1 | PAD_LEFT) && n)
			sel = std::max(0, sel - LIST_ROWS);
		else if (b & (PAD_R1 | PAD_RIGHT) && n)
			sel = std::min(n - 1, sel + LIST_ROWS);
		else if (b & PAD_CROSS && n)
		{
			const Entry &e = items[sel];
			if (e.dir)
			{
				std::string next = cwd.empty() ? e.name : cwd + e.name + "/";
				std::vector<Entry> next_items;
				if (list_dir(next, next_items))
				{
					cwd = next;
					items.swap(next_items);
					sel = top = 0;
					status.clear();
				}
				else
				{
					status = "Cannot open " + next;
					status_err = true;
				}
			}
			else
			{
				ps2_cfg.last_dir = cwd;
				ps2_config_save();
				wait_release();
				return cwd + e.name;
			}
		}
		else if (b & (PAD_CIRCLE | PAD_TRIANGLE))
		{
			if (!cwd.empty())
			{
				std::string came_from = cwd;
				cwd = parent_of(cwd);
				if (cwd.empty() || !list_dir(cwd, items))
				{
					cwd.clear();
					list_devices(items);
				}

				/* Re-select the folder we just left. */
				sel = top = 0;
				for (int i = 0; i < (int)items.size(); i++)
				{
					std::string full = cwd.empty() ? items[i].name : cwd + items[i].name + "/";
					if (full == came_from)
						sel = i;
				}
				status.clear();
			}
			else
				list_devices(items); /* refresh: a stick may have appeared */
		}
	}
}
