#pragma once
/* GS renderer: the SNES picture drawn by the Graphics Synthesizer instead
 * of the core's per-pixel loops.
 *
 * The core already renders in bands of scanlines with constant PPU state
 * (S9xUpdateScreen) and walks backgrounds and sprites tile by tile,
 * calling a "draw tile" function for each. In GS mode those functions
 * (gfx.cpp swaps the pointers) emit one textured sprite per tile:
 *   - tiles live in GS memory as 4/8-bit textures, palettes as CLUTs;
 *   - priorities are the GS Z buffer, with the core's own depth values;
 *   - colour math is the GS blender: the sub screen is drawn to its own
 *     buffer and blended onto the pixels whose frame-buffer alpha bit
 *     says "math applies";
 *   - windows arrive as clipped tiles, as they do for the software loops.
 * A band that uses something this does not handle (Mode 7, hi-res,
 * mosaic, direct colour, ...) is rendered in software as before and its
 * lines are uploaded, so the two can mix within a frame.
 *
 * Not bit-identical to the software renderer by construction; the
 * compare mode measures how close it is. */

/* Renderer setting. Changing it re-lays out GS memory. */
void ps2_gs_enable(bool on);
bool ps2_gs_enabled(void);

/* Debug: also render every GS band in software, read the GS picture back
 * at the end of the frame and report the differences on the log. */
extern int ps2_gs_compare;
/* Debug (ps2.cfg gs_defer=0, gs_crc=1): split bands at every fixed-colour
 * write as the software renderer does; log a checksum of the GS picture
 * every 10th frame ("GSCRC"). The two together check the per-scanline
 * fixed colour, which the compare mode cannot (it switches it off). */
extern int ps2_gs_defer, ps2_gs_crc;
/* With gs_crc: also print every pixel of this frame number (gs_dump=N; a
 * multiple of 10), as "GSPIX y x colours..." lines. */
extern int ps2_gs_dump;

/* Frame hooks (ps2_main.cpp). A GS frame is in the GS target and wants
 * ps2_video_present_target(); a frame that was all software is presented
 * from GFX.Screen as usual. */
/* overlay: text will be drawn into GFX.Screen before frame_end (profiler,
 * frame rate, core messages); it is laid over the GS picture. */
void ps2_gs_frame_begin(bool overlay);
/* The picture is complete (software bands uploaded, comparison done):
 * true for a GS frame. The overlay may now be drawn into GFX.Screen. */
bool ps2_gs_frame_picture(int width, int height);
/* For a GS frame: lay the overlay over it and send everything. */
void ps2_gs_frame_end(int width, int height);

/* Forget everything in GS memory (ROM load, state load, layout change). */
void ps2_gs_reset(void);

/* With the profiler on: one line a second on the log about the last frame. */
void ps2_gs_log_stats(void);

/* Statistics of the last frame. */
struct Ps2GsStats
{
	unsigned gs_bands, sw_bands, tiles, uploads, bytes;
	/* palettes sent (16-, 4- and 256-colour), bands that found the
	 * colours changed; Mode 7 map updates, of which with a scan of the
	 * whole map for changed characters; stream buffers sent */
	unsigned clut4, clut2, clut8, clut_bands, m7_updates, m7_scans, sends;
	unsigned cmp_pixels, cmp_frames; /* compare mode: differing pixels, frames compared */
	const char *sw_reason;            /* why the last software band was one */
};
extern Ps2GsStats ps2_gs_stats;
