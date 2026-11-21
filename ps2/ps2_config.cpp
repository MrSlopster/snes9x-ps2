/* Port settings: defaults, load/save as key=value text, core hookup. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes9x.h"
#include "apu.h"

#include "ps2_config.h"
#include "ps2_dirs.h"
#include "ps2_video.h"
#include "ps2_prof.h"
#include "ps2_idle.h"

Ps2Config ps2_cfg;

static std::string config_path(void)
{
	return std::string(ps2_base_dir()) + "/ps2.cfg";
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void ps2_config_defaults(void)
{
	ps2_cfg.video_mode = PS2_VIDEO_AUTO;
	ps2_cfg.filter = PS2_FILTER_SMOOTH;
	ps2_cfg.frameskip = PS2_FRAMESKIP_AUTO;
	ps2_cfg.overlay = PS2_OVERLAY_OFF;
	/* Falls back to the accurate path where the IOP module is missing. */
	ps2_cfg.sound = PS2_SOUND_FAST;
	ps2_cfg.netlog = false;
	ps2_cfg.renderer = PS2_RENDERER_GS;
	ps2_cfg.gs_compare = false;
	ps2_cfg.idle_skip = true;
	ps2_cfg.dump_frames = false;
	ps2_cfg.netlog_host.clear();
	ps2_cfg.last_dir.clear();
}

void ps2_config_load(void)
{
	ps2_config_defaults();

	FILE *f = fopen(config_path().c_str(), "r");
	if (!f)
		return;

	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = 0;
		const char *key = line, *val = eq + 1;
		int n = atoi(val);

		if (!strcmp(key, "video_mode"))
			ps2_cfg.video_mode = clamp(n, 0, PS2_VIDEO_MODE_COUNT - 1);
		else if (!strcmp(key, "filter"))
			ps2_cfg.filter = clamp(n, 0, PS2_FILTER_COUNT - 1);
		else if (!strcmp(key, "frameskip"))
			ps2_cfg.frameskip = clamp(n, PS2_FRAMESKIP_AUTO, 4);
		else if (!strcmp(key, "show_fps")) /* 0 off, 1 FPS, 2 profiler, 3 +PMC */
			ps2_cfg.overlay = clamp(n, 0, PS2_OVERLAY_COUNT - 1);
		else if (!strcmp(key, "sound"))
			ps2_cfg.sound = (n >= 0 && n < PS2_SOUND_COUNT) ? n : PS2_SOUND_ACCURATE;
		else if (!strcmp(key, "renderer"))
			ps2_cfg.renderer = (n >= 0 && n < PS2_RENDERER_COUNT) ? n : PS2_RENDERER_SOFTWARE;
		else if (!strcmp(key, "gs_compare"))
			ps2_cfg.gs_compare = n != 0;
		else if (!strcmp(key, "dump_frames"))
			ps2_cfg.dump_frames = n != 0;
		else if (!strcmp(key, "idle_skip"))
			ps2_cfg.idle_skip = n != 0;
		else if (!strcmp(key, "netlog"))
			ps2_cfg.netlog = n != 0;
		else if (!strcmp(key, "netlog_host"))
			ps2_cfg.netlog_host = val;
		else if (!strcmp(key, "last_dir"))
			ps2_cfg.last_dir = val;
	}
	fclose(f);
	printf("config: loaded %s\n", config_path().c_str());
}

bool ps2_config_save(void)
{
	FILE *f = fopen(config_path().c_str(), "w");
	if (!f)
	{
		printf("config: cannot write %s\n", config_path().c_str());
		return false;
	}

	fprintf(f, "# Snes9x PS2 settings\n");
	fprintf(f, "video_mode=%d\n", ps2_cfg.video_mode);
	fprintf(f, "filter=%d\n", ps2_cfg.filter);
	fprintf(f, "frameskip=%d\n", ps2_cfg.frameskip);
	fprintf(f, "show_fps=%d\n", ps2_cfg.overlay);
	fprintf(f, "sound=%d\n", ps2_cfg.sound);
	fprintf(f, "renderer=%d\n", ps2_cfg.renderer);
	fprintf(f, "idle_skip=%d\n", ps2_cfg.idle_skip ? 1 : 0);
	fprintf(f, "netlog=%d\n", ps2_cfg.netlog ? 1 : 0);
	fprintf(f, "netlog_host=%s\n", ps2_cfg.netlog_host.c_str());
	fprintf(f, "last_dir=%s\n", ps2_cfg.last_dir.c_str());
	fclose(f);
	return true;
}

void ps2_config_apply_core(void)
{
	/* S9xSyncSpeed renders 1 of every SkipFrames frames (unix semantics);
	 * "skip N" therefore means SkipFrames = N + 1. */
	Settings.SkipFrames = ps2_cfg.frameskip == PS2_FRAMESKIP_AUTO
				      ? AUTO_FRAMERATE
				      : (uint32)(ps2_cfg.frameskip + 1);
	/* Profiler mode shows its own fps line; the core counter would also
	 * draw into the frame and spoil the A/B frame fingerprints. */
	Settings.DisplayFrameRate = ps2_cfg.overlay == PS2_OVERLAY_FPS ? TRUE : FALSE;
	ps2_prof_enabled = ps2_cfg.overlay >= PS2_OVERLAY_PROFILER;
	ps2_prof_pmc = ps2_cfg.overlay == PS2_OVERLAY_PROFILER_PMC;
	/* Only the accurate path produces samples (and paces by them). */
	Settings.Mute = ps2_cfg.sound == PS2_SOUND_ACCURATE ? FALSE : TRUE;
	ps2_idle_enabled = ps2_cfg.idle_skip ? 1 : 0;
}
