/* On-screen ROM browser.
 *
 * Renders text with the core's 8x9 bitmap font (font.h) into GFX.Screen
 * as a 512x224 BGR555 frame and shows it through the normal video path.
 * Input comes straight from libpad (pad 1).
 *
 *   D-pad up/down   move        L1/R1 or left/right   page
 *   Cross           open / load  Circle or Triangle    back
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
#include "gfx.h"
#include "../font.h" /* the core's bitmap font; ps2sdk has its own font.h */

#include "ps2_menu.h"
#include "ps2_video.h"
#include "ps2_controls.h"

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

/* ---- browser ------------------------------------------------------- */

static void render(const std::string &cwd, const std::vector<Entry> &items, int sel,
		   int top, const std::string &status, bool status_err)
{
	fill_rows(0, MENU_H / LINE_H + 1, col_bg);

	draw_text(1, 0, "Snes9x " VERSION " for PS2 - select a ROM", col_title);
	draw_text(1, 1, cwd.empty() ? "Devices" : cwd.c_str(), col_dim, COLS - 2);

	if (items.empty())
		draw_text(2, LIST_TOP, cwd.empty() ? "(no storage devices found)" : "(no ROMs or folders here)", col_dim);

	for (int r = 0; r < LIST_ROWS && top + r < (int)items.size(); r++)
	{
		const Entry &e = items[top + r];
		int row = LIST_TOP + r;
		if (top + r == sel)
			fill_rows(row, 1, col_sel_bg);

		std::string label = e.dir && !cwd.empty() ? e.name + "/" : e.name;
		draw_text(2, row, label.c_str(), e.dir ? col_dir : col_text, COLS - 3);
	}

	if ((int)items.size() > LIST_ROWS)
	{
		char pos[32];
		snprintf(pos, sizeof(pos), "%d/%d", sel + 1, (int)items.size());
		draw_text(COLS - 1 - (int)strlen(pos), 1, pos, col_dim);
	}

	if (!status.empty())
		draw_text(1, STATUS_ROW, status.c_str(), status_err ? col_err : col_text, COLS - 2);

	draw_text(1, HELP_ROW, "X open   O back   L1/R1 page", col_dim);

	ps2_video_frame(GFX.Screen, GFX.RealPPL, MENU_W, MENU_H);
	ps2_video_sync(); /* menu pacing: one step per vblank */
}

std::string ps2_menu_pick_rom(const char *start_dir, const char *message,
			      const char *select_name)
{
	/* No pad driver: nothing could ever drive the menu. */
	if (!ps2_controls_available())
		return "";

	init_colours();

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
		/* Keep sel visible. */
		if (sel < top)
			top = sel;
		if (sel >= top + LIST_ROWS)
			top = sel - LIST_ROWS + 1;

		render(cwd, items, sel, top, status, status_err);

		unsigned b = poll_buttons();
		int n = (int)items.size();

		if (b & PAD_UP && n)
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
				return cwd + e.name;
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
