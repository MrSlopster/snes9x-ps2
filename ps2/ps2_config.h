#pragma once
/* Port settings, persisted as key=value lines in <base>/ps2.cfg. */

#include <string>

#define PS2_FRAMESKIP_AUTO (-1)

struct Ps2Config
{
	int video_mode;   /* PS2_VIDEO_* */
	int filter;       /* PS2_FILTER_* */
	int frameskip;    /* PS2_FRAMESKIP_AUTO or 0..4 frames skipped */
	bool show_fps;
	bool sound;
	std::string last_dir; /* browser start folder */
};

extern Ps2Config ps2_cfg;

void ps2_config_defaults(void);
/* Missing file or unknown keys keep the defaults. */
void ps2_config_load(void);
bool ps2_config_save(void);

/* Push frameskip / FPS / sound into the core's Settings. */
void ps2_config_apply_core(void);
