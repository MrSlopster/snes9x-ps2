/* PS2 GS video output for Snes9x 1.63.
 *
 * VRAM: two 640x448 (NTSC) / 640x512 (PAL) PSMCT32 framebuffers + one
 * 512x480 PSMCT16 texture (3.1 MB of 4 MB worst case).
 *
 * The core renders straight into the GS's native 16-bit layout
 * (PIXEL_FORMAT=BGR555, see pixform.h), so a frame needs no CPU-side
 * conversion or copy. Per frame, one DMA chain on PATH3:
 *
 *   CNT  BITBLTBUF/TRXPOS/TRXREG/TRXDIR + IMAGE GIF tag
 *   REF  x rows     -> GFX.Screen rows, read in place (only the rendered
 *                      width, not the 512-pixel pitch)
 *   END  TEXFLUSH, FRAME (back buffer), textured sprite, FINISH
 *
 * The finished back buffer is shown by the vblank interrupt handler
 * (DISPFB flip), so the emulator never blocks on the display and the
 * picture never tears.
 */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_privileged.h>
#include <gs_psm.h>
#include <graph.h>
#include <stdio.h>
#include <packet.h>
#include <draw.h>
#include <draw2d.h>
#include <draw3d.h>

#include "ps2_video.h"
#include "ps2_watchdog.h"

#define FB_WIDTH  640

/* Video modes (ps2_video.h). Framebuffer height = displayed lines:
 * interlaced FIELD modes show both fields (448/512), progressive modes
 * show one frame of 224/256 lines (240p/288p) or 480 (480p). */
struct ModeSpec
{
	int crt, interlace, ffmd, flicker, height;
};

static const ModeSpec mode_specs[PS2_VIDEO_MODE_COUNT] = {
	{ 0, 0, 0, 0, 0 }, /* AUTO: resolved from the console region */
	{ GRAPH_MODE_NTSC, GRAPH_MODE_INTERLACED, GRAPH_MODE_FIELD, GRAPH_ENABLE, 448 },
	{ GRAPH_MODE_PAL, GRAPH_MODE_INTERLACED, GRAPH_MODE_FIELD, GRAPH_ENABLE, 512 },
	{ GRAPH_MODE_NTSC, GRAPH_MODE_NONINTERLACED, GRAPH_MODE_FRAME, GRAPH_DISABLE, 224 },
	{ GRAPH_MODE_PAL, GRAPH_MODE_NONINTERLACED, GRAPH_MODE_FRAME, GRAPH_DISABLE, 256 },
	{ GRAPH_MODE_HDTV_480P, GRAPH_MODE_NONINTERLACED, GRAPH_MODE_FRAME, GRAPH_DISABLE, 480 },
};

static int fb_height = 448;
#define FB_HEIGHT fb_height
static int cur_mode = PS2_VIDEO_AUTO;   /* as requested (may be AUTO) */
static int cur_filter = PS2_FILTER_SMOOTH;

/* Largest SNES frame the core renders: 512 wide x 478 high (PAL interlace).
 * 512*480 PSMCT16 = 30720 qwords: fits one IMAGE GIF tag (NLOOP <= 0x7FFF)
 * and one REF tag (QWC <= 0xFFFF). */
#define TEX_MAX_W 512
#define TEX_MAX_H 480

/* Chain budget: 7 header qwords + one REF per row + tail (TEXFLUSH/FRAME
 * 3, sprite 4, finish 2, END tag 1; the GS layout's present_commands
 * adds ten register writes), with room to spare: a 478-row frame came to
 * 505 qwords when this was 8 + rows + 16. */
#define FRAME_PACKET_QW (8 + TEX_MAX_H + 64)

/* GS renderer layout (ps2_gs.cpp): the SNES frame is drawn by the GS into
 * the texture the software path uploads to, and the rest of VRAM holds
 * what that needs. To make room the display buffers are 16-bit. */
static int gs_layout = 0;
static Ps2GsVram gs_vram;

static framebuffer_t fb[2];
static zbuffer_t zb;
static texbuffer_t texbuf;
static packet_t *frame_packet = NULL; /* per-frame DMA chain */
static packet_t *solid_packet = NULL; /* solid-colour fill (normal send) */
static u16 *fallback = NULL;          /* only if GFX.Screen is misaligned */
static int video_ready = 0;

/* Double buffering. 'displayed' is scanned out; 'pending' is finished and
 * waits for the next vblank. Written by the main thread with interrupts
 * disabled and by the vblank handler. */
static u64 dispfb1[2], dispfb2[2];
static volatile int displayed = 0;
static volatile int pending = -1;
/* A GS-rendered frame whose final stretch into this display buffer has
 * been sent but not waited for: the vblank handler makes it 'pending' once
 * the GS has signalled FINISH. The main thread used to spin until then,
 * about a millisecond per frame. */
static volatile int awaiting = -1;
static int vsync_id = -1;

static s32 vblank_flip(s32 cause)
{
	(void)cause;
	ps2_watchdog_vblank();

	int a = awaiting;
	if (a >= 0 && (*GS_REG_CSR & 2))
	{
		*GS_REG_CSR = 2; /* acknowledge FINISH */
		awaiting = -1;
		pending = a;
	}

	int p = pending;
	if (p >= 0)
	{
		*GS_REG_DISPFB1 = dispfb1[p];
		*GS_REG_DISPFB2 = dispfb2[p];
		displayed = p;
		pending = -1;
	}

	/* Per Sony docs / ps2sdk samples: last thing in an INTC handler. */
	ExitHandler();
	return 0;
}

/* Pick the buffer to draw into. If a finished frame has not been shown yet
 * (emulation outpacing the display, e.g. NTSC game on a 50 Hz console),
 * take it back and overwrite it: never draw into the scanned-out buffer. */
/* Bring a frame that is still on its way to the same point as one that
 * was waited for: everything sent has been drawn, FINISH is acknowledged.
 * Needed before anything else that uses the FINISH signal or the packet. */
void ps2_video_settle(void)
{
	if (awaiting < 0)
		return;
	dma_wait_fast();
	for (;;)
	{
		DIntr();
		if (awaiting < 0)
		{
			EIntr();
			break; /* the vblank handler got there first */
		}
		if (*GS_REG_CSR & 2)
		{
			*GS_REG_CSR = 2;
			pending = awaiting;
			awaiting = -1;
			EIntr();
			break;
		}
		EIntr();
	}
}

static int acquire_back_buffer(void)
{
	int target;

	ps2_video_settle();
	DIntr();
	if (pending >= 0)
	{
		target = pending;
		pending = -1;
	}
	else
		target = displayed ^ 1;
	EIntr();

	return target;
}

static void present(int target)
{
	pending = target; /* single aligned store; the handler reads it once */
}

static qword_t *set_frame(qword_t *q, int target)
{
	PACK_GIFTAG(q, GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(fb[target].address >> 11, fb[target].width >> 6,
				    fb[target].psm, fb[target].mask), GS_REG_FRAME);
	q++;
	return q;
}

static void setup_drawing_environment(void)
{
	packet_t *packet = packet_init(20, PACKET_NORMAL);
	qword_t *q = packet->data;

	q = draw_setup_environment(q, 0, &fb[0], &zb);
	/* draw_setup_environment already sets XYOFFSET to (2048, 2048), which
	 * is what 0-based 2D sprite coordinates need (the draw2d helpers add
	 * the +2048 screen offsets themselves). */
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();
	draw_wait_finish();

	packet_free(packet);
}

static void setup_texture_state(void)
{
	packet_t *packet = packet_init(10, PACKET_NORMAL);
	qword_t *q = packet->data;

	lod_t lod;
	clutbuffer_t clut;

	lod.calculation = LOD_USE_K;
	lod.max_level = 0;
	lod.mag_filter = cur_filter == PS2_FILTER_SHARP ? LOD_MAG_NEAREST : LOD_MAG_LINEAR;
	lod.min_filter = cur_filter == PS2_FILTER_SHARP ? LOD_MIN_NEAREST : LOD_MIN_LINEAR;
	lod.l = 0;
	lod.k = 0;

	texbuf.info.width = draw_log2(TEX_MAX_W);
	texbuf.info.height = draw_log2(TEX_MAX_H);
	texbuf.info.components = TEXTURE_COMPONENTS_RGB;
	texbuf.info.function = TEXTURE_FUNCTION_DECAL;

	clut.storage_mode = CLUT_STORAGE_MODE1;
	clut.start = 0;
	clut.psm = 0;
	clut.load_method = CLUT_NO_LOAD;
	clut.address = 0;

	q = draw_texture_sampling(q, 0, &lod);
	q = draw_texturebuffer(q, 0, &texbuf, &clut);

	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();

	packet_free(packet);
}

/* Fill one framebuffer with a solid colour and wait for the GS. */
static void fill_buffer(int target, int r, int g, int b)
{
	qword_t *q = solid_packet->data;

	rect_t rect;
	rect.v0.x = 0.0f;
	rect.v0.y = 0.0f;
	rect.v0.z = 1.0f;
	rect.v1.x = FB_WIDTH;
	rect.v1.y = FB_HEIGHT;
	rect.v1.z = 1.0f;
	rect.color.r = (u8)r;
	rect.color.g = (u8)g;
	rect.color.b = (u8)b;
	rect.color.a = 0x80;
	rect.color.q = 1.0f;

	q = set_frame(q, target);
	q = draw_rect_filled(q, 0, &rect);
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, solid_packet->data, q - solid_packet->data, 0, 0);
	dma_wait_fast();
	draw_wait_finish();
}

static qword_t *present_commands(qword_t *q, int target, int width, int height);

static int resolve_mode(int mode)
{
	if (mode <= PS2_VIDEO_AUTO || mode >= PS2_VIDEO_MODE_COUNT)
		return graph_get_region() == GRAPH_MODE_PAL ? PS2_VIDEO_PAL_576I : PS2_VIDEO_NTSC_480I;
	return mode;
}

/* (Re)program the CRTC and VRAM layout for a mode. Safe to call at
 * runtime: the vblank flip is parked first so it never shows a buffer
 * from the old layout. */
static void apply_mode(int mode)
{
	const ModeSpec &m = mode_specs[resolve_mode(mode)];

	ps2_video_settle();
	DIntr();
	pending = -1;
	EIntr();

	fb_height = m.height;

	graph_vram_clear();
	for (int i = 0; i < 2; i++)
	{
		fb[i].width = FB_WIDTH;
		fb[i].height = FB_HEIGHT;
		fb[i].mask = 0;
		fb[i].psm = gs_layout ? GS_PSM_16 : GS_PSM_32;
		fb[i].address = graph_vram_allocate(FB_WIDTH, FB_HEIGHT, fb[i].psm, GRAPH_ALIGN_PAGE);

		/* With the flicker filter both read circuits are blended and
		 * circuit 2 reads one line lower. Without it, graph_enable_output
		 * shows circuit 2 alone, so it must read line 0. */
		dispfb1[i] = GS_SET_DISPFB(fb[i].address >> 11, fb[i].width >> 6, fb[i].psm, 0, 0);
		dispfb2[i] = GS_SET_DISPFB(fb[i].address >> 11, fb[i].width >> 6, fb[i].psm, 0, m.flicker ? 1 : 0);
	}

	/* Texture in VRAM: PSMCT16 (BGR555 from the core). Page-aligned so
	 * the GS renderer can use it as a frame buffer. */
	texbuf.width = TEX_MAX_W;
	texbuf.psm = GS_PSM_16;
	texbuf.address = graph_vram_allocate(TEX_MAX_W, TEX_MAX_H, GS_PSM_16, GRAPH_ALIGN_PAGE);

	static int generation = 0;
	memset(&gs_vram, 0, sizeof(gs_vram));
	gs_vram.generation = ++generation;
	if (gs_layout)
	{
		gs_vram.target = texbuf.address;
		gs_vram.z = graph_vram_allocate(512, 256, GS_PSMZ_16, GRAPH_ALIGN_PAGE);
		gs_vram.sub = graph_vram_allocate(512, 256, GS_PSM_16, GRAPH_ALIGN_PAGE);
		gs_vram.atlas2 = graph_vram_allocate(1024, 512, GS_PSM_4, GRAPH_ALIGN_BLOCK);
		gs_vram.atlas4 = graph_vram_allocate(512, 512, GS_PSM_4, GRAPH_ALIGN_BLOCK);
		gs_vram.atlas8 = graph_vram_allocate(512, 256, GS_PSM_8, GRAPH_ALIGN_BLOCK);
		bool cluts = true;
		for (int i = 0; i < 4; i++)
		{
			/* 64 x 32 PSMCT16 = 16 GS blocks */
			gs_vram.clut[i] = graph_vram_allocate(64, 32, GS_PSM_16, GRAPH_ALIGN_PAGE);
			cluts = cluts && gs_vram.clut[i] > 0;
		}
		gs_vram.ok = gs_vram.z > 0 && gs_vram.sub > 0 && gs_vram.atlas2 > 0 && gs_vram.atlas4 > 0 &&
			     gs_vram.atlas8 > 0 && cluts;
		/* Last and optional: a quarter of GS memory. */
		gs_vram.m7 = graph_vram_allocate(1024, 1024, GS_PSM_8, GRAPH_ALIGN_BLOCK);
		printf("GS VRAM: fb %x %x target %x z %x sub %x atlas %x %x %x clut %x.. m7 %x %s\n", fb[0].address,
		       fb[1].address, gs_vram.target, gs_vram.z, gs_vram.sub, gs_vram.atlas2, gs_vram.atlas4,
		       gs_vram.atlas8, gs_vram.clut[0], gs_vram.m7, gs_vram.ok ? "ok" : "FAILED");
	}

	graph_set_mode(m.interlace, m.crt, m.ffmd, m.flicker);
	graph_set_screen(0, 0, FB_WIDTH, FB_HEIGHT);
	graph_set_bgcolor(0, 0, 0);
	*GS_REG_DISPFB1 = dispfb1[0];
	*GS_REG_DISPFB2 = dispfb2[0];
	graph_enable_output();

	setup_drawing_environment();
	setup_texture_state();

	/* Both buffers start black instead of whatever VRAM held. */
	fill_buffer(0, 0, 0, 0);
	fill_buffer(1, 0, 0, 0);
	displayed = 0;
	pending = -1;
}

void ps2_video_init(int mode, int filter)
{
	memset(fb, 0, sizeof(fb));
	memset(&zb, 0, sizeof(zb));
	memset(&texbuf, 0, sizeof(texbuf));

	cur_mode = mode;
	cur_filter = filter;

	zb.enable = DRAW_DISABLE;
	zb.zsm = GS_ZBUF_32;
	zb.mask = 1;
	zb.address = 0;

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	/* Arm CPCOND0 for the GIF channel. Without this, dma_wait_fast()
	 * (which spins on bc0f) returns immediately and waits for nothing. */
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	frame_packet = packet_init(FRAME_PACKET_QW, PACKET_NORMAL);
	solid_packet = packet_init(16, PACKET_NORMAL);
	if (!frame_packet || !solid_packet)
		return;

	apply_mode(mode);

	vsync_id = graph_add_vsync_handler(vblank_flip);

	video_ready = 1;
}

void ps2_video_set_mode(int mode)
{
	cur_mode = mode;
	if (video_ready)
		apply_mode(mode);
}

int ps2_video_get_mode(void)
{
	return cur_mode;
}

void ps2_video_set_filter(int filter)
{
	cur_filter = filter;
	if (video_ready)
		setup_texture_state();
}

void ps2_video_deinit(void)
{
	ps2_video_settle();
	video_ready = 0;
	if (vsync_id >= 0)
	{
		graph_remove_vsync_handler(vsync_id);
		vsync_id = -1;
	}
	if (frame_packet)
	{
		packet_free(frame_packet);
		frame_packet = NULL;
	}
	if (solid_packet)
	{
		packet_free(solid_packet);
		solid_packet = NULL;
	}
	if (fallback)
	{
		free(fallback);
		fallback = NULL;
	}
	graph_shutdown();
}

void ps2_video_frame(const unsigned short *src, int src_pitch, int width, int height)
{
	if (!video_ready)
		return;

	if (width > TEX_MAX_W)
		width = TEX_MAX_W;
	if (height > TEX_MAX_H)
		height = TEX_MAX_H;
	if (width < 1 || height < 1)
		return;

	/* Rows go out as whole qwords (8 PSMCT16 pixels). Rounding up only
	 * reads pixels that exist in the 512-wide pitch; the sprite below
	 * never samples past 'width'. */
	int upload_w = (width + 7) & ~7;
	if (upload_w > src_pitch)
		upload_w = src_pitch & ~7;
	int row_qw = upload_w >> 3;
	int total_qw = row_qw * height;

	/* DMA reads physical memory in 16-byte units. GFX.Screen is a
	 * std::vector, so check rather than assume alignment; copy only if
	 * the allocator let us down. */
	if ((u32)src & 15)
	{
		if (!fallback)
			fallback = (u16 *)memalign(64, TEX_MAX_W * TEX_MAX_H * sizeof(u16));
		if (!fallback)
			return;
		for (int y = 0; y < height; y++)
			memcpy(fallback + y * upload_w, src + y * src_pitch, upload_w * sizeof(u16));
		src = fallback;
		src_pitch = upload_w;
	}

	int target = acquire_back_buffer();
	qword_t *q = frame_packet->data;

	DMATAG_CNT(q, 6, 0, 0, 0);
	q++;
	PACK_GIFTAG(q, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_BITBLTBUF(0, 0, 0, texbuf.address >> 6, texbuf.width >> 6, GS_PSM_16), GS_REG_BITBLTBUF);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXPOS(0, 0, 0, 0, 0), GS_REG_TRXPOS);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXREG(upload_w, height), GS_REG_TRXREG);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR);
	q++;
	PACK_GIFTAG(q, GIF_SET_TAG(total_qw, 0, 0, 0, GIF_FLG_IMAGE, 0), 0);
	q++;

	if (src_pitch == upload_w)
	{
		DMATAG_REF(q, total_qw, (u32)src, 0, 0, 0);
		q++;
	}
	else
	{
		for (int y = 0; y < height; y++)
		{
			DMATAG_REF(q, row_qw, (u32)(src + y * src_pitch), 0, 0, 0);
			q++;
		}
	}

	/* Tail: everything after the END tag's qword is sent, then the
	 * chain stops. */
	qword_t *end_tag = q;
	q++;

	q = present_commands(q, target, width, height);

	DMATAG_END(end_tag, q - end_tag - 1, 0, 0, 0);

	/* Write back the core's freshly rendered pixels (and this packet) to
	 * RAM. FlushCache(0) costs a fixed 8 KB D-cache walk, much cheaper
	 * than a per-line SyncDCache over up to 480 KB of frame. */
	FlushCache(0);
	dma_channel_send_chain(DMA_CHANNEL_GIF, frame_packet->data, q - frame_packet->data, 0, 0);

	/* The DMA reads GFX.Screen in place: the core must not render the next
	 * frame until it is done (dma_wait_fast, armed in init). FINISH is the
	 * last thing in the chain, so draw_wait_finish also proves every REF
	 * read is done, and that the back buffer is complete before it is
	 * queued for display. Keep both. */
	dma_wait_fast();
	draw_wait_finish();

	present(target);
}

const Ps2GsVram *ps2_video_gs_vram(void)
{
	return (video_ready && gs_layout && gs_vram.ok) ? &gs_vram : NULL;
}

void ps2_video_set_gs_layout(int on)
{
	on = on ? 1 : 0;
	if (on == gs_layout)
		return;
	gs_layout = on;
	if (video_ready)
		apply_mode(cur_mode);
}

void ps2_video_present_target(int width, int height)
{
	if (!video_ready)
		return;
	PS2_STAGE("present: waiting for a back buffer");
	int target = acquire_back_buffer();
	qword_t *q = present_commands(frame_packet->data, target, width, height);

	/* The GS renderer's last buffer goes first. */
	PS2_STAGE("present: waiting for DMA");
	dma_wait_fast();
	FlushCache(0);
	PS2_STAGE("present: sending");
	dma_channel_send_normal(DMA_CHANNEL_GIF, frame_packet->data, q - frame_packet->data, 0, 0);
	PS2_STAGE("present: done");

	/* Not waited for: see 'awaiting'. */
	awaiting = target;
}

/* Stretch the top-left width x height of the SNES texture over a display
 * buffer. Sets every register it relies on: the GS renderer leaves its
 * own drawing state behind. */
static qword_t *present_commands(qword_t *q, int target, int width, int height)
{
	PACK_GIFTAG(q, GIF_SET_TAG(gs_layout ? 12 : 2, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, 1, GS_REG_TEXFLUSH);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(fb[target].address >> 11, fb[target].width >> 6,
				    fb[target].psm, fb[target].mask), GS_REG_FRAME);
	q++;
	if (gs_layout)
	{
		PACK_GIFTAG(q, GS_SET_ZBUF(gs_vram.z >> 11, GS_PSMZ_16 & 0xf, 1), GS_REG_ZBUF); /* no Z writes */
		q++;
		PACK_GIFTAG(q, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, 1), GS_REG_TEST); /* Z test: always */
		q++;
		PACK_GIFTAG(q, GS_SET_SCISSOR(0, FB_WIDTH - 1, 0, FB_HEIGHT - 1), GS_REG_SCISSOR);
		q++;
		PACK_GIFTAG(q, GS_SET_TEX0(texbuf.address >> 6, texbuf.width >> 6, GS_PSM_16, draw_log2(TEX_MAX_W),
					   draw_log2(TEX_MAX_H), 0, 1, 0, 0, 0, 0, 0), GS_REG_TEX0);
		q++;
		PACK_GIFTAG(q, GS_SET_TEX1(1, 0, cur_filter == PS2_FILTER_SHARP ? 0 : 1,
					   cur_filter == PS2_FILTER_SHARP ? 0 : 1, 0, 0, 0), GS_REG_TEX1);
		q++;
		PACK_GIFTAG(q, GS_SET_CLAMP(1, 1, 0, 0, 0, 0), GS_REG_CLAMP);
		q++;
		PACK_GIFTAG(q, GS_SET_FBA(0), GS_REG_FBA);
		q++;
		PACK_GIFTAG(q, GS_SET_PABE(0), GS_REG_PABE);
		q++;
		PACK_GIFTAG(q, GS_SET_DTHE(0), GS_REG_DTHE);
		q++;
		PACK_GIFTAG(q, GS_SET_COLCLAMP(1), GS_REG_COLCLAMP);
		q++;
	}

	texrect_t rect;
	rect.v0.x = 0.0f;
	rect.v0.y = 0.0f;
	rect.v0.z = 1.0f;
	rect.v1.x = FB_WIDTH;
	rect.v1.y = FB_HEIGHT;
	rect.v1.z = 1.0f;
	/* Due to the GS DDA, texel 0.5 maps to screen pixel 0, hence the
	 * +/- 0.5f texels. */
	rect.t0.u = 0.5f;
	rect.t0.v = 0.5f;
	rect.t1.u = (float)width - 0.5f;
	rect.t1.v = (float)height - 0.5f;
	rect.color.r = 0x80;
	rect.color.g = 0x80;
	rect.color.b = 0x80;
	rect.color.a = 0x80;
	rect.color.q = 1.0f;

	q = draw_rect_textured(q, 0, &rect);
	q = draw_finish(q);
	return q;
}

void ps2_video_test(int r, int g, int b)
{
	if (!video_ready)
		return;

	int target = acquire_back_buffer();
	fill_buffer(target, r, g, b);
	present(target);
}

void ps2_video_sync(void)
{
	if (!video_ready)
		return;
	graph_wait_vsync();
}
