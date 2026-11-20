/* PS2 GS video output for Snes9x 1.63.
 *
 * One 640x448 framebuffer (PSM_32), one RGB565 (GS_PSM_16) texture in VRAM.
 * Each frame: DMA GFX.Screen to VRAM, draw one textured sprite stretched
 * over the whole framebuffer, wait for vsync.
 *
 * Modeled on ps2sdk samples/draw/texture (graph + draw + draw2d APIs).
 */

#include <stdlib.h>
#include <string.h>

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <gs_psm.h>
#include <graph.h>
#include <packet.h>
#include <draw.h>
#include <draw2d.h>
#include <draw3d.h>

#include "ps2_video.h"
#include "ps2_video_convert.h"

#define FB_WIDTH  640
#define FB_HEIGHT 448

/* Largest SNES frame the core renders: 512 wide x 478 high (PAL interlace).
 * Staging buffer is 128-byte aligned for the DMA transfer. */
#define TEX_MAX_W 512
#define TEX_MAX_H 480

static framebuffer_t fb;
static zbuffer_t zb;
static texbuffer_t texbuf;
static packet_t *upload_packet = NULL; /* texture transfer + TEXFLUSH (chain) */
static packet_t *draw_packet = NULL;   /* textured sprite + finish (normal) */
static u16 staging[TEX_MAX_W * TEX_MAX_H] __attribute__((aligned(128)));
static int video_ready = 0;

static void setup_drawing_environment(void)
{
	packet_t *packet = packet_init(20, PACKET_NORMAL);
	qword_t *q = packet->data;

	q = draw_setup_environment(q, 0, &fb, &zb);
	/* draw_setup_environment already sets XYOFFSET to (2048, 2048), which
	 * is what 0-based 2D sprite coordinates need (the draw2d helpers add
	 * the +2048 screen offsets themselves). */
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();

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
	lod.mag_filter = LOD_MAG_LINEAR;
	lod.min_filter = LOD_MIN_LINEAR;
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

void ps2_video_init(void)
{
	memset(&fb, 0, sizeof(fb));
	memset(&zb, 0, sizeof(zb));
	memset(&texbuf, 0, sizeof(texbuf));

	/* 32-bit 640x448 framebuffer, no z-buffer (2D blit only). */
	fb.width = FB_WIDTH;
	fb.height = FB_HEIGHT;
	fb.mask = 0;
	fb.psm = GS_PSM_32;
	fb.address = graph_vram_allocate(FB_WIDTH, FB_HEIGHT, GS_PSM_32, GRAPH_ALIGN_PAGE);

	zb.enable = DRAW_DISABLE;
	zb.zsm = GS_ZBUF_32;
	zb.mask = 1;
	zb.address = 0;

	/* Texture in VRAM: RGB565, block-aligned. */
	texbuf.width = TEX_MAX_W;
	texbuf.psm = GS_PSM_16;
	texbuf.address = graph_vram_allocate(TEX_MAX_W, TEX_MAX_H, GS_PSM_16, GRAPH_ALIGN_BLOCK);

	graph_initialize(fb.address, fb.width, fb.height, fb.psm, 0, 0);

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	setup_drawing_environment();
	setup_texture_state();

	/* Upload packet budget: transfer writes 5 + (3 per GIF_BLOCK_SIZE
	 * chunk) qwords; 512x480 PSM16 is 30720 qwords = 1920 chunks
	 * -> ~5769 qwords worst case. Draw packet: sprite + finish < 16. */
	upload_packet = packet_init(6144, PACKET_NORMAL);
	draw_packet = packet_init(16, PACKET_NORMAL);

	video_ready = upload_packet && draw_packet;
}

void ps2_video_deinit(void)
{
	video_ready = 0;
	if (upload_packet)
	{
		packet_free(upload_packet);
		upload_packet = NULL;
	}
	if (draw_packet)
	{
		packet_free(draw_packet);
		draw_packet = NULL;
	}
	graph_shutdown();
}

void ps2_video_frame(const unsigned short *src, int src_width, int width, int height)
{
	if (!video_ready)
		return;

	/* Display sync: wait for the previous scanout to finish before
	 * drawing the next frame (single-buffered, so drawing mid-field
	 * tears). This is where the vsync wait lives. */
	ps2_video_sync();

	if (width > TEX_MAX_W)
		width = TEX_MAX_W;
	if (height > TEX_MAX_H)
		height = TEX_MAX_H;
	if (width < 1 || height < 1)
		return;

	if (!upload_packet || !draw_packet)
		return;

	/* Stage into the aligned buffer, converting 565 -> GS PSM 16 (5551):
	 * the GS has no RGB565 texture format. GFX.RealPPL may differ from
	 * the rendered width (interlace). */
	ps2_convert_frame_565_to_5551(staging, TEX_MAX_W, src, src_width,
				      width, height);

	SyncDCache(staging, (u8 *)staging + sizeof(u16) * TEX_MAX_W * height);

	/* ---- Transfer 1: texture upload (DMA chain) ----
	 * draw_texture_transfer emits CNT/REF DMA tags; draw_texture_flush
	 * ENDS the chain (DMATAG_END), so this packet must stop at the flush.
	 * Anything appended after it is never sent by the DMA controller. */
	qword_t *q = upload_packet->data;

	q = draw_texture_transfer(q, staging, TEX_MAX_W, height, GS_PSM_16,
				  texbuf.address, texbuf.width);
	q = draw_texture_flush(q);

	dma_channel_send_chain(DMA_CHANNEL_GIF, upload_packet->data, q - upload_packet->data, 0, 0);
	dma_wait_fast();

	/* ---- Transfer 2: draw the sprite (single normal send) ----
	 * draw_rect_textured emits plain GIF packets without DMA tags, so
	 * they can only follow a GIF-tagged chain sent normally. */
	q = draw_packet->data;

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

	dma_channel_send_normal(DMA_CHANNEL_GIF, draw_packet->data, q - draw_packet->data, 0, 0);
	dma_wait_fast();
	draw_wait_finish();
}

void ps2_video_test(int r, int g, int b)
{
	if (!video_ready || !draw_packet)
		return;

	qword_t *q = draw_packet->data;

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

	q = draw_rect_filled(q, 0, &rect);
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, draw_packet->data, q - draw_packet->data, 0, 0);
	dma_wait_fast();
	draw_wait_finish();
}

void ps2_video_sync(void)
{
	if (!video_ready)
		return;
	graph_wait_vsync();
}
