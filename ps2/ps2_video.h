#pragma once
/* PS2 GS video output for Snes9x 1.63.
 * Uploads the core's GFX.Screen (BGR555 = GS PSMCT16, no conversion) and
 * stretches it over a double-buffered 640-wide display, flipped at vblank. */

enum
{
	PS2_VIDEO_AUTO = 0,   /* console region: NTSC 480i or PAL 576i */
	PS2_VIDEO_NTSC_480I,
	PS2_VIDEO_PAL_576I,
	PS2_VIDEO_NTSC_240P,
	PS2_VIDEO_PAL_288P,
	PS2_VIDEO_480P,       /* component cable only */
	PS2_VIDEO_MODE_COUNT
};

enum
{
	PS2_FILTER_SMOOTH = 0, /* bilinear */
	PS2_FILTER_SHARP,      /* nearest */
	PS2_FILTER_COUNT
};

void ps2_video_init(int mode, int filter);
void ps2_video_deinit(void);

/* Runtime changes (settings menu). set_mode reprograms the CRTC and
 * clears the screen. */
void ps2_video_set_mode(int mode);
int ps2_video_get_mode(void);
void ps2_video_set_filter(int filter);

/* Draw one frame: src is GFX.Screen (BGR555), src_pitch is the row pitch
 * of the core's buffer (pixels), width/height are the rendered size.
 * src is DMA'd in place; returns once the DMA has finished reading it. */
void ps2_video_frame(const unsigned short *src, int src_pitch, int width, int height);

/* Draw a solid test colour: lets a TV (no ps2link) tell init/boot failure
 * states apart once the GS is initialized early at boot. */
void ps2_video_test(int r, int g, int b);

/* Blocking wait for the GS vertical blank (menu pacing). */
void ps2_video_sync(void);
