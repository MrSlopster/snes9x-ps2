#pragma once
/* PS2 GS video output for Snes9x 1.63.
 * Uploads the core's GFX.Screen (BGR555 = GS PSMCT16, no conversion) and
 * stretches it over a double-buffered 640x448 display, flipped at vblank. */

void ps2_video_init(void);
void ps2_video_deinit(void);

/* Draw one frame: src is GFX.Screen (BGR555), src_pitch is the row pitch
 * of the core's buffer (pixels), width/height are the rendered size.
 * src is DMA'd in place; returns once the DMA has finished reading it. */
void ps2_video_frame(const unsigned short *src, int src_pitch, int width, int height);

/* Draw a solid test colour: lets a TV (no ps2link) tell init/boot failure
 * states apart once the GS is initialized early at boot. */
void ps2_video_test(int r, int g, int b);

/* Blocking wait for the GS vertical blank. Not used by the frame path
 * (flips happen in the vblank interrupt); kept for diagnostics. */
void ps2_video_sync(void);
