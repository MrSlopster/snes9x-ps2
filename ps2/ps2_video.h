#pragma once
/* PS2 GS video output for Snes9x 1.63.
 * Blits the core's RGB565 GFX.Screen to a 640x448 framebuffer. */

void ps2_video_init(void);
void ps2_video_deinit(void);

/* Draw one frame: src is GFX.Screen (RGB565), src_width is the framebuffer
 * pitch of the core's buffer (pixels), width/height are the rendered size. */
void ps2_video_frame(const unsigned short *src, int src_width, int width, int height);

/* Draw a solid test colour: lets a TV (no ps2link) tell init/boot failure
 * states apart once the GS is initialized early at boot. */
void ps2_video_test(int r, int g, int b);

/* Display sync: wait for the GS vertical blank. */
void ps2_video_sync(void);
