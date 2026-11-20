#pragma once
/* Port settings, persisted as key=value lines in <base>/ps2.cfg. */

#include <string>

#define PS2_FRAMESKIP_AUTO (-1)

enum
{
	PS2_OVERLAY_OFF = 0,
	PS2_OVERLAY_FPS,
	PS2_OVERLAY_PROFILER,
	PS2_OVERLAY_COUNT
};

struct Ps2Config
{
	int video_mode;   /* PS2_VIDEO_* */
	int filter;       /* PS2_FILTER_* */
	int frameskip;    /* PS2_FRAMESKIP_AUTO or 0..4 frames skipped */
	int overlay;      /* PS2_OVERLAY_* */
	bool sound;
	std::string last_dir; /* browser start folder */
};

extern Ps2Config ps2_cfg;

void ps2_config_defaults(void);
/* Missing file or unknown keys keep the defaults. */
void ps2_config_load(void);
bool ps2_config_save(void);

/* Push frameskip / overlay / sound into the core and the profiler. */
void ps2_config_apply_core(void);
