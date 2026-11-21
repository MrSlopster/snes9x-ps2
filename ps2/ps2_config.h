#pragma once
/* Port settings, persisted as key=value lines in <base>/ps2.cfg. */

#include <string>

#define PS2_FRAMESKIP_AUTO (-1)

enum
{
	PS2_OVERLAY_OFF = 0,
	PS2_OVERLAY_FPS,
	PS2_OVERLAY_PROFILER,
	PS2_OVERLAY_PROFILER_PMC, /* + R5900 performance counters (hardware only) */
	PS2_OVERLAY_COUNT
};

enum
{
	PS2_SOUND_OFF = 0,
	PS2_SOUND_ACCURATE, /* software DSP, streamed */
	PS2_SOUND_FAST,     /* voices on SPU2 hardware (ps2_spu.h) */
	PS2_SOUND_COUNT
};

enum
{
	PS2_RENDERER_SOFTWARE = 0,
	PS2_RENDERER_GS, /* drawn by the Graphics Synthesizer (ps2_gs.h) */
	PS2_RENDERER_GS_CHECK, /* GS, with every frame also drawn in software and compared (slow; reports on the log) */
	PS2_RENDERER_COUNT
};

struct Ps2Config
{
	int video_mode;   /* PS2_VIDEO_* */
	int filter;       /* PS2_FILTER_* */
	int frameskip;    /* PS2_FRAMESKIP_AUTO or 0..4 frames skipped */
	int overlay;      /* PS2_OVERLAY_* */
	int sound;        /* PS2_SOUND_* */
	int renderer;     /* PS2_RENDERER_* */
	bool gs_compare;  /* debug: check the GS renderer against software every frame */
	bool netlog;            /* UDP log over the network adapter */
	bool idle_skip;         /* exact 65816 idle-loop skipping (ps2_idle.cpp) */
	bool dump_frames;       /* debug: raw frame dumps with the profiler */
	std::string netlog_host; /* "a.b.c.d[:port]", empty = subnet broadcast */
	std::string last_dir; /* browser start folder */
};

extern Ps2Config ps2_cfg;

void ps2_config_defaults(void);
/* Missing file or unknown keys keep the defaults. */
void ps2_config_load(void);
bool ps2_config_save(void);

/* Push frameskip / overlay / sound into the core and the profiler. */
void ps2_config_apply_core(void);
