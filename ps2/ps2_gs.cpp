/* GS renderer (see ps2_gs.h). */

#include <stdio.h>
#include <string.h>
#include <malloc.h>

extern "C" {
#include <kernel.h>
#include <dma.h>
#include <draw.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <screenshot.h>
}

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "gfx.h"
#include "tile.h"

#include "ps2_gs.h"
#include "ps2_video.h"
#include "ps2_net.h"
#include "ps2_watchdog.h"

int ps2_gs_compare = 0;
Ps2GsStats ps2_gs_stats;

static bool enabled = false;
static const Ps2GsVram *vram = NULL;

/* ---- command stream ------------------------------------------------- */

/* GIF packets are built in order into one of two buffers and sent when a
 * buffer fills or the frame ends; the other buffer is filled while the
 * GS works through the first. */
#define STREAM_QW 16384 /* 256 KB each */
static u64 *stream[2];
static int cur = 0;
static u64 *sp, *send_limit; /* write position, flush threshold */
static u64 *run_tag = NULL;  /* open REGLIST run of tiles, or NULL */
static unsigned run_count;

#define TILE_REGS 6 /* TEX0, RGBAQ, UV, XYZ2, UV, XYZ2 */
#define TILE_QW 3

static void stream_begin(void)
{
	sp = stream[cur];
	send_limit = sp + (STREAM_QW - 512) * 2;
	run_tag = NULL;
}

static inline void run_close(void)
{
	if (run_tag)
	{
		run_tag[0] = GIF_SET_TAG(run_count, 1, 0, 0, GIF_FLG_REGLIST, TILE_REGS);
		run_tag = NULL;
	}
}

static void stream_send(void)
{
	run_close();
	unsigned qw = (unsigned)(sp - stream[cur]) / 2;
	if (qw)
	{
		ps2_gs_stats.bytes += qw * 16;
		PS2_STAGE("gs: waiting for the previous buffer's DMA");
		dma_wait_fast();
		FlushCache(0);
		PS2_STAGE("gs: sending a buffer");
		dma_channel_send_normal(DMA_CHANNEL_GIF, stream[cur], qw, 0, 0);
		PS2_STAGE("gs: buffer sent");
		cur ^= 1;
	}
	stream_begin();
}

/* Room for qw more qwords. Requests come in two sizes: small ones, which
 * the slack behind send_limit absorbs even when it is not checked for
 * every tile, and big image transfers, which must fit outright. */
static inline void need(unsigned qw)
{
	if (sp + qw * 2 > send_limit && sp != stream[cur])
		stream_send();
}

/* An A+D packet of n registers: returns where the pairs go. */
static inline u64 *ad(unsigned n)
{
	run_close();
	need(n + 1);
	sp[0] = GIF_SET_TAG(n, 1, 0, 0, GIF_FLG_PACKED, 1);
	sp[1] = GIF_REG_AD;
	u64 *p = sp + 2;
	sp += 2 + n * 2;
	return p;
}
#define AD(p, value, reg) ((p)[0] = (u64)(value), (p)[1] = (reg), (p) += 2)

/* Image transfer of w x h pixels to a buffer; returns where the data
 * goes ('bytes' of it, padded to whole qwords with zeros by the caller's
 * layout). */
static u64 *image(int dbp, int dbw, int psm, int x, int y, int w, int h, unsigned bytes)
{
	unsigned qw = (bytes + 15) / 16;
	run_close();
	need(qw + 7);
	u64 *p = ad(4);
	AD(p, GS_SET_BITBLTBUF(0, 0, 0, dbp, dbw, psm), GS_REG_BITBLTBUF);
	AD(p, GS_SET_TRXPOS(0, 0, x, y, 0), GS_REG_TRXPOS);
	AD(p, GS_SET_TRXREG(w, h), GS_REG_TRXREG);
	AD(p, GS_SET_TRXDIR(0), GS_REG_TRXDIR);
	sp[0] = GIF_SET_TAG(qw, 1, 0, 0, GIF_FLG_IMAGE, 0);
	sp[1] = 0;
	u64 *data = sp + 2;
	data[qw * 2 - 1] = data[qw * 2 - 2] = 0;
	sp += 2 + qw * 2;
	return data;
}

/* ---- palettes ------------------------------------------------------- */

/* Palettes in GS memory, all views of the 256 screen colours (CSM1):
 *   clut[0]: 4bpp tiles - 16 palettes of 16 colours;
 *   clut[1]: 8bpp tiles - one 16x16 palette of 256;
 *   clut[2], clut[3]: 2bpp tiles - 32 palettes of 4 colours.
 * A 4-bit texture's palette is the 8x2 image at its CLUT base pointer, so
 * each small palette sits at the start of its own GS block (64 words) and
 * is selected by base pointer. Colour 0 of every palette is transparent:
 * alpha bit clear, which the alpha test rejects. */
static uint16 shadow4[16][16], shadow2[32][4], shadow8[256];
static bool clut_valid = false;

static void clut_small(int cbp, const uint16 *colours, unsigned n)
{
	uint16 *img = (uint16 *)image(cbp, 1, GS_PSM_16, 0, 0, 8, 2, 32);
	for (unsigned j = 0; j < 16; j++)
		img[j] = (uint16)((j && j < n) ? ((colours[j] & 0x7FFF) | 0x8000) : 0);
}

static void clut_update(void)
{
	const uint16 *c = IPPU.ScreenColors;
	bool any = false;

	/* Usually nothing changed since the last band: one compare says so. */
	static uint16 last[256];
	static int last_mode = -1;
	if (clut_valid && last_mode == PPU.BGMode && !memcmp(last, c, sizeof(last)))
		return;
	memcpy(last, c, sizeof(last));
	last_mode = PPU.BGMode;

	for (unsigned p = 0; p < 16; p++)
		if (!clut_valid || memcmp(shadow4[p], c + p * 16, sizeof(shadow4[p])))
		{
			memcpy(shadow4[p], c + p * 16, sizeof(shadow4[p]));
			clut_small((vram->clut[0] >> 6) + p, shadow4[p], 16);
			any = true;
		}

	/* 2bpp palettes are only reachable in modes 0, 1, 4 and 5; the first
	 * 128 colours. */
	for (unsigned g = 0; g < 32; g++)
		if (!clut_valid || memcmp(shadow2[g], c + g * 4, sizeof(shadow2[g])))
		{
			memcpy(shadow2[g], c + g * 4, sizeof(shadow2[g]));
			clut_small((vram->clut[2 + (g >> 4)] >> 6) + (g & 15), shadow2[g], 4);
			any = true;
		}

	const bool uses8 = PPU.BGMode == 3 || PPU.BGMode == 4 || PPU.BGMode == 7;
	if (uses8 && (!clut_valid || memcmp(shadow8, c, sizeof(shadow8))))
	{
		memcpy(shadow8, c, sizeof(shadow8));
		uint16 *img = (uint16 *)image(vram->clut[1] >> 6, 1, GS_PSM_16, 0, 0, 16, 16, 512);
		for (unsigned i = 0; i < 256; i++)
		{
			/* CSM1 order: bits 3 and 4 of the index swapped */
			unsigned pos = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
			img[pos] = (uint16)(i ? ((c[i] & 0x7FFF) | 0x8000) : 0);
		}
		any = true;
	}
	else if (!uses8)
		shadow8[0] ^= 0x8000; /* stale: upload when an 8bpp mode comes */

	clut_valid = true;
	if (any)
	{
		u64 *p = ad(1);
		AD(p, 0, GS_REG_TEXFLUSH);
	}
}

/* ---- tiles ---------------------------------------------------------- */

/* One atlas per bit depth, direct-mapped: tile number n is at grid cell
 * (n % COLS, n / COLS), 8 pixels in from the edges. 'valid' is cleared by
 * the core when VRAM is written (S9xGSVRAMWrite, ppu.h). */
uint8 S9xGSTileValid2[4096], S9xGSTileValid4[2048], S9xGSTileValid8[1024];

struct Atlas
{
	int tbp, tbw, psm, tw, th, cols;
	uint8 *valid;
	const uint16 *pos; /* texel position of each tile: x | y << 16 ... as two tables */
	u64 tex0;          /* TEX0 without the CLUT pointer */
};
static Atlas atlas[3]; /* by BG.TileShift - 4 */
/* Tile positions (no divisions per tile: the R5900 takes 37 cycles each). */
static uint16 pos_x127[4096], pos_y127[4096], pos_x63[2048], pos_y63[2048];

static void atlas_setup(void)
{
	static bool tables = false;
	if (!tables)
	{
		for (unsigned n = 0; n < 4096; n++)
			pos_x127[n] = (uint16)(8 + (n % 127) * 8), pos_y127[n] = (uint16)(8 + (n / 127) * 8);
		for (unsigned n = 0; n < 2048; n++)
			pos_x63[n] = (uint16)(8 + (n % 63) * 8), pos_y63[n] = (uint16)(8 + (n / 63) * 8);
		tables = true;
	}
	atlas[0] = { vram->atlas2 >> 6, 1024 / 64, GS_PSM_4, 10, 9, 127, S9xGSTileValid2, NULL, 0 };
	atlas[1] = { vram->atlas4 >> 6, 512 / 64, GS_PSM_4, 9, 9, 63, S9xGSTileValid4, NULL, 0 };
	atlas[2] = { vram->atlas8 >> 6, 512 / 64, GS_PSM_8, 9, 8, 63, S9xGSTileValid8, NULL, 0 };
	for (int i = 0; i < 3; i++)
		atlas[i].tex0 = GS_SET_TEX0(atlas[i].tbp, atlas[i].tbw, atlas[i].psm, atlas[i].tw, atlas[i].th, 1, 0, 0,
					    GS_PSM_16, 0, 0, 1);
}

static inline int tile_x(const Atlas &a, unsigned n)
{
	return a.cols == 127 ? pos_x127[n] : pos_x63[n];
}
static inline int tile_y(const Atlas &a, unsigned n)
{
	return a.cols == 127 ? pos_y127[n] : pos_y63[n];
}

static void tile_upload(const Atlas &a, unsigned n, const uint8 *pix)
{
	int x = tile_x(a, n), y = tile_y(a, n);
	if (a.psm == GS_PSM_4)
	{
		uint8 *d = (uint8 *)image(a.tbp, a.tbw, GS_PSM_4, x, y, 8, 8, 32);
		for (int i = 0; i < 32; i++)
			d[i] = (uint8)((pix[i * 2] & 15) | (pix[i * 2 + 1] << 4));
	}
	else
		memcpy(image(a.tbp, a.tbw, GS_PSM_8, x, y, 8, 8, 64), pix, 64);
	u64 *p = ad(1);
	AD(p, 0, GS_REG_TEXFLUSH);
	a.valid[n] = 1;
	ps2_gs_stats.uploads++;
}

/* ---- Mode 7 ---------------------------------------------------------- */

/* The Mode 7 plane is a 128x128 map of 8x8 characters, 256 colours: a
 * 1024x1024 8-bit texture here, built from VRAM (map cell = low byte of
 * a word, character data = high bytes) and kept up to date from the dirty
 * marks the core leaves on VRAM writes. Each scanline is then one
 * textured strip with the line's matrix worked into its texture
 * coordinates. */
extern struct SLineMatrixData LineMatrixData[240];

uint8 S9xGSMode7Cell[0x4000], S9xGSMode7Row[128], S9xGSMode7Char[256];
uint8 S9xGSMode7AnyDirty = 1;
static bool m7_valid = false;
static bool m7_tile0_valid = false;

/* Characters of map row cy, cells cx0..cx0+63, as a 512x8 image. */
static void m7_upload_half_row(unsigned cy, unsigned cx0)
{
	uint8 *d = (uint8 *)image(vram->m7 >> 6, 1024 / 64, GS_PSM_8, (int)cx0 * 8, (int)cy * 8, 512, 8, 512 * 8);
	const uint8 *map = Memory.VRAM + (cy * 128 + cx0) * 2;
	for (unsigned c = 0; c < 64; c++)
	{
		const uint8 *src = Memory.VRAM + 1 + (map[c * 2] << 7);
		uint8 *o = d + c * 8;
		for (unsigned py = 0; py < 8; py++, src += 16, o += 512)
			for (unsigned px = 0; px < 8; px++)
				o[px] = src[px * 2];
	}
}

static void m7_upload_cell(unsigned cell)
{
	uint8 *d = (uint8 *)image(vram->m7 >> 6, 1024 / 64, GS_PSM_8, (int)(cell & 127) * 8, (int)(cell >> 7) * 8, 8, 8, 64);
	const uint8 *src = Memory.VRAM + 1 + (Memory.VRAM[cell * 2] << 7);
	for (unsigned py = 0; py < 8; py++, src += 16, d += 8)
		for (unsigned px = 0; px < 8; px++)
			d[px] = src[px * 2];
}

static void m7_update(void)
{
	if (m7_valid && !S9xGSMode7AnyDirty)
		return;

	/* Which cells to send again: the written ones, and - only if any
	 * character data was written - every cell showing such a character.
	 * Nothing here walks the whole map unless it has to. */
	static uint16 list[2048];
	unsigned count = 0;
	bool all = !m7_valid;

	if (!all)
	{
		bool chars = false;
		for (unsigned c = 0; c < 256; c++)
			if (S9xGSMode7Char[c])
			{
				chars = true;
				break;
			}
		if (chars)
		{
			if (S9xGSMode7Char[0])
				m7_tile0_valid = false;
			for (unsigned cell = 0; cell < 0x4000 && !all; cell++)
				if (S9xGSMode7Char[Memory.VRAM[cell * 2]] || S9xGSMode7Cell[cell])
				{
					if (count < sizeof(list) / sizeof(list[0]))
						list[count++] = (uint16)cell;
					else
						all = true;
				}
		}
		else
		{
			for (unsigned row = 0; row < 128 && !all; row++)
				if (S9xGSMode7Row[row])
					for (unsigned cell = row * 128; cell < row * 128 + 128; cell++)
						if (S9xGSMode7Cell[cell])
						{
							if (count < sizeof(list) / sizeof(list[0]))
								list[count++] = (uint16)cell;
							else
								all = true;
						}
		}
	}

	if (all)
	{
		for (unsigned cy = 0; cy < 128; cy++)
		{
			m7_upload_half_row(cy, 0);
			m7_upload_half_row(cy, 64);
		}
		m7_tile0_valid = false;
		ps2_gs_stats.uploads += 256;
	}
	else
	{
		for (unsigned i = 0; i < count; i++)
			m7_upload_cell(list[i]);
		ps2_gs_stats.uploads += count;
	}

	if (!m7_tile0_valid)
	{
		/* Character 0 on its own, for "repeat character 0 outside the
		 * map": the unused corner of the 8bpp atlas. */
		uint8 *d = (uint8 *)image(vram->atlas8 >> 6, 512 / 64, GS_PSM_8, 0, 0, 8, 8, 64);
		const uint8 *src = Memory.VRAM + 1;
		for (unsigned py = 0; py < 8; py++, src += 16, d += 8)
			for (unsigned px = 0; px < 8; px++)
				d[px] = src[px * 2];
		m7_tile0_valid = true;
	}

	memset(S9xGSMode7Cell, 0, sizeof(S9xGSMode7Cell));
	memset(S9xGSMode7Row, 0, sizeof(S9xGSMode7Row));
	memset(S9xGSMode7Char, 0, sizeof(S9xGSMode7Char));
	S9xGSMode7AnyDirty = 0;
	m7_valid = true;
	u64 *p = ad(1);
	AD(p, 0, GS_REG_TEXFLUSH);
}

/* ---- band state ----------------------------------------------------- */

static bool band_active = false;  /* inside a GS band */
static bool screen_sub = false;   /* drawing the sub screen */
static bool sub_drawn = false;    /* this band has a sub screen */
static bool in_obj = false;       /* between OBJ begin and end */
static int z_test = -1;           /* ZTST the stream is set to */
static bool frame_has_gs = false;
static bool frame_open = false;

/* Sprite tiles are collected and emitted in reverse: the core draws them
 * first-wins (a depth trick the GS cannot do: test one value, write
 * another); drawn last-wins in reverse order, with no depth test, the
 * same sprite ends up on top carrying its own priority. */
struct TileRec
{
	u64 r[TILE_REGS];
};
#define OBJ_MAX 8192
static TileRec *obj_recs;
static unsigned obj_count;

/* Software bands of a frame that also has GS bands: uploaded at the end. */
static struct
{
	uint16 y0, y1;
} sw_bands[240];
static unsigned sw_band_count;

#define ZTST_ALWAYS 1
#define ZTST_GREATER 3

static void set_ztest(int method)
{
	if (z_test == method)
		return;
	z_test = method;
	u64 *p = ad(1);
	/* alpha test: texels with alpha 0 (transparent colour) are dropped */
	AD(p, GS_SET_TEST(1, 6, 0, 0, 0, 0, 1, method), GS_REG_TEST);
}

static inline u64 xyz(int x, int y, unsigned z)
{
	return GS_SET_XYZ((x + 2048) << 4, (y + 2048) << 4, z);
}

static inline unsigned rgb8(unsigned c15)
{
	return ((c15 & 31) << 3) | (((c15 >> 5) & 31) << 11) | (((c15 >> 10) & 31) << 19);
}

/* A flat rectangle (the stream must not be inside a tile run). */
static void flat_rect(int x0, int y0, int x1, int y1, unsigned z, unsigned rgb, unsigned alpha, int abe)
{
	u64 *p = ad(4);
	AD(p, GS_SET_PRIM(6, 0, 0, 0, abe, 0, 0, 0, 0), GS_REG_PRIM);
	AD(p, (u64)rgb | ((u64)alpha << 24) | ((u64)0x3f800000 << 32), GS_REG_RGBAQ);
	AD(p, xyz(x0, y0, z), GS_REG_XYZ2);
	AD(p, xyz(x1, y1, z), GS_REG_XYZ2);
}

/* Start drawing a screen (sub or main) of the current band: target,
 * scissor, cleared depth. */
void S9xGSScreen(bool8 sub)
{
	if (!band_active)
		return;
	screen_sub = sub;
	if (sub)
		sub_drawn = true;

	u64 *p = ad(14);
	AD(p, GS_SET_FRAME((sub ? vram->sub : vram->target) >> 11, 512 / 64, GS_PSM_16, 0), GS_REG_FRAME);
	AD(p, GS_SET_ZBUF(vram->z >> 11, GS_PSMZ_16 & 0xf, 0), GS_REG_ZBUF);
	AD(p, GS_SET_XYOFFSET(2048 << 4, 2048 << 4), GS_REG_XYOFFSET);
	AD(p, GS_SET_SCISSOR(0, 255, GFX.StartY, GFX.EndY), GS_REG_SCISSOR);
	AD(p, GS_SET_PRMODECONT(1), GS_REG_PRMODECONT);
	AD(p, GS_SET_COLCLAMP(1), GS_REG_COLCLAMP);
	AD(p, GS_SET_DTHE(0), GS_REG_DTHE);
	AD(p, GS_SET_FBA(0), GS_REG_FBA);
	AD(p, GS_SET_PABE(0), GS_REG_PABE);
	AD(p, GS_SET_TEXA(0, 0, 0x80), GS_REG_TEXA);
	AD(p, GS_SET_TEX1(1, 0, 0, 0, 0, 0, 0), GS_REG_TEX1);
	AD(p, GS_SET_CLAMP(1, 1, 0, 0, 0, 0), GS_REG_CLAMP);
	AD(p, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTST_ALWAYS), GS_REG_TEST);
	AD(p, 0, GS_REG_TEXFLUSH);
	z_test = -1;

	/* Depth 0 everywhere. The sub screen starts as the fixed colour with
	 * alpha clear ("not a sub-screen pixel"); the main screen's colour is
	 * covered completely by the layers and the backdrop. */
	flat_rect(0, GFX.StartY, 256, GFX.EndY + 1, 0, sub ? rgb8(GFX.FixedColour) : 0, 0, 0);
}

void S9xGSOBJBegin(void)
{
	if (band_active)
	{
		in_obj = true;
		obj_count = 0;
	}
}

static void tile_emit(const TileRec &t)
{
	if (!run_tag)
	{
		/* The tag's PRIM field only counts in PACKED mode: set the
		 * register (a textured sprite, texel coordinates) by hand. */
		need(3 + TILE_QW * 64);
		u64 *p = ad(1);
		AD(p, GS_SET_PRIM(6, 0, 1, 0, 0, 0, 1, 0, 0), GS_REG_PRIM);
		run_tag = sp;
		sp[1] = (u64)GIF_REG_TEX0 | ((u64)GIF_REG_RGBAQ << 4) | ((u64)GIF_REG_UV << 8) |
			((u64)GIF_REG_XYZ2 << 12) | ((u64)GIF_REG_UV << 16) | ((u64)GIF_REG_XYZ2 << 20);
		sp += 2;
		run_count = 0;
	}
	memcpy(sp, t.r, sizeof(t.r));
	sp += TILE_REGS;
	if (++run_count == 0x7FFF || sp > send_limit)
		run_close();
}

void S9xGSOBJEnd(void)
{
	if (!band_active || !in_obj)
		return;
	in_obj = false;
	if (!obj_count)
		return;
	set_ztest(ZTST_ALWAYS);
	for (unsigned i = obj_count; i-- > 0;)
		tile_emit(obj_recs[i]);
	run_close();
}

/* The tile functions the core calls in place of its own. */
static void draw_tile(uint32 Tile, uint32 Offset, uint32 StartPixel, uint32 Width, uint32 StartLine, uint32 LineCount,
		      bool math)
{
	/* Same lookup as TileImpl::CachedTile. */
	uint32 TileAddr = BG.TileAddress + ((Tile & 0x3ff) << BG.TileShift);
	if (Tile & 0x100)
		TileAddr += BG.NameSelect;
	TileAddr &= 0xffff;
	uint32 n = TileAddr >> BG.TileShift;
	uint8 *pix = &BG.Buffer[n << 6];
	if (!BG.Buffered[n])
		BG.Buffered[n] = BG.ConvertTile(pix, TileAddr, Tile & 0x3ff);
	if (BG.Buffered[n] == BLANK_TILE)
		return;

	const Atlas &a = atlas[BG.TileShift - 4];
	if (!a.valid[n])
		tile_upload(a, n, pix);

	/* Position: Offset is x + y * pitch, x from -7. The pitch is 512 in
	 * every mode drawn here (no interlace). */
	int y = (int)((Offset + 16) >> 9);
	int x = (int)(Offset - ((uint32)y << 9)) + (int)StartPixel;
	int line = (int)(StartLine >> 3);

	/* Texel range, 12.4 fixed with the usual half-texel bias; flips run
	 * the coordinates backwards. */
	int tx = tile_x(a, n), ty = tile_y(a, n);
	int u0, u1, v0, v1;
	if (Tile & H_FLIP)
	{
		u0 = (tx + 7 - (int)StartPixel) * 16 + 8;
		u1 = u0 - (int)Width * 16;
	}
	else
	{
		u0 = (tx + (int)StartPixel) * 16 + 8;
		u1 = u0 + (int)Width * 16;
	}
	if (Tile & V_FLIP)
	{
		v0 = (ty + 7 - line) * 16 + 8;
		v1 = v0 - (int)LineCount * 16;
	}
	else
	{
		v0 = (ty + line) * 16 + 8;
		v1 = v0 + (int)LineCount * 16;
	}

	/* Palette. */
	unsigned base = ((Tile >> BG.PaletteShift) & BG.PaletteMask) + BG.StartPalette;
	int cbp;
	if (BG.TileShift == 5)
		cbp = (vram->clut[0] >> 6) + ((base >> 4) & 15);
	else if (BG.TileShift == 6)
		cbp = vram->clut[1] >> 6;
	else
		cbp = (vram->clut[2 + ((base >> 6) & 1)] >> 6) + ((base >> 2) & 15);

	TileRec t;
	t.r[0] = a.tex0 | ((u64)cbp << 37);
	/* Vertex colour: 0x80 = texture unchanged, 0 = black (colour window).
	 * Vertex alpha: 0x80 leaves the texel's alpha alone, and so sets the
	 * frame buffer's alpha bit = "colour math applies here"; 0x40 clears
	 * it but still passes the alpha test. */
	unsigned c = GFX.ClipColors ? 0 : 0x808080;
	unsigned alpha = (screen_sub || math) ? 0x80 : 0x40;
	t.r[1] = (u64)c | ((u64)alpha << 24) | ((u64)0x3f800000 << 32);
	t.r[2] = GS_SET_UV(u0, v0);
	t.r[3] = xyz(x, y, GFX.Z2);
	t.r[4] = GS_SET_UV(u1, v1);
	t.r[5] = xyz(x + (int)Width, y + (int)LineCount, GFX.Z2);
	ps2_gs_stats.tiles++;

	if (in_obj)
	{
		if (obj_count < OBJ_MAX)
			obj_recs[obj_count++] = t;
		return;
	}
	set_ztest(ZTST_GREATER);
	tile_emit(t);
}

static void gs_tile_nomath(uint32 Tile, uint32 Offset, uint32 StartLine, uint32 LineCount)
{
	draw_tile(Tile, Offset, 0, 8, StartLine, LineCount, false);
}
static void gs_tile_math(uint32 Tile, uint32 Offset, uint32 StartLine, uint32 LineCount)
{
	draw_tile(Tile, Offset, 0, 8, StartLine, LineCount, true);
}
static void gs_clipped_nomath(uint32 Tile, uint32 Offset, uint32 StartPixel, uint32 Width, uint32 StartLine,
			      uint32 LineCount)
{
	draw_tile(Tile, Offset, StartPixel, Width, StartLine, LineCount, false);
}
static void gs_clipped_math(uint32 Tile, uint32 Offset, uint32 StartPixel, uint32 Width, uint32 StartLine,
			    uint32 LineCount)
{
	draw_tile(Tile, Offset, StartPixel, Width, StartLine, LineCount, true);
}

/* Mode 7, BG1. One strip per scanline and per piece of it: inside the
 * map, and (repeat mode 3) outside it, where character 0 repeats. */
#define M7_LIMIT (1024 << 8) /* map size in the 8.8 fixed point the PPU uses */

static inline u64 st(float s, float t)
{
	union { float f; u32 u; } a, b;
	a.f = s;
	b.f = t;
	return (u64)a.u | ((u64)b.u << 32);
}

/* The part [i0, i1) of [0, n) where c0 + d * i lies in [0, M7_LIMIT). */
static void m7_inside(int c0, int d, int n, int &i0, int &i1)
{
	if (d == 0)
	{
		if (c0 < 0 || c0 >= M7_LIMIT)
			i1 = i0; /* empty */
		return;
	}
	/* first i with c >= 0 (d > 0) or c < LIMIT (d < 0); first i past the other end */
	long long lo, hi;
	if (d > 0)
	{
		lo = c0 >= 0 ? 0 : (-(long long)c0 + d - 1) / d;
		hi = c0 >= M7_LIMIT ? 0 : ((long long)M7_LIMIT - c0 + d - 1) / d;
	}
	else
	{
		lo = c0 < M7_LIMIT ? 0 : ((long long)c0 - M7_LIMIT) / -d + 1;
		hi = c0 < 0 ? 0 : (long long)c0 / -d + 1;
	}
	if (lo > i0)
		i0 = (int)(lo > n ? n : lo);
	if (hi < i1)
		i1 = (int)(hi < 0 ? 0 : hi);
	if (i1 < i0)
		i1 = i0;
}

static void m7_strip(int x0, int x1, int y, unsigned z, float s0, float t0, float s1, float t1)
{
	u64 *p = ad(9);
	AD(p, GS_SET_PRIM(4, 0, 1, 0, 0, 0, 0, 0, 0), GS_REG_PRIM); /* triangle strip, textured, ST */
	AD(p, st(s0, t0), GS_REG_ST);
	AD(p, xyz(x0, y, z), GS_REG_XYZ2);
	AD(p, st(s1, t1), GS_REG_ST);
	AD(p, xyz(x1, y, z), GS_REG_XYZ2);
	AD(p, st(s0, t0), GS_REG_ST);
	AD(p, xyz(x0, y + 1, z), GS_REG_XYZ2);
	AD(p, st(s1, t1), GS_REG_ST);
	AD(p, xyz(x1, y + 1, z), GS_REG_XYZ2);
}

void S9xGSMode7(uint32 Left, uint32 Right, int D, bool8 math)
{
	if (!band_active || Right <= Left)
		return;
	set_ztest(ZTST_GREATER);

	unsigned c = GFX.ClipColors ? 0 : 0x808080;
	unsigned alpha = (screen_sub || math) ? 0x80 : 0x40;
	const int repeat = PPU.Mode7Repeat;
	const unsigned z = (unsigned)(D + 7);
	const int n = (int)(Right - Left);

	/* Map texture; coordinates wrap (they are kept inside the map by hand
	 * in the other repeat modes). Q = 1. */
	u64 *p = ad(3);
	AD(p, GS_SET_TEX0(vram->m7 >> 6, 1024 / 64, GS_PSM_8, 10, 10, 1, 0, vram->clut[1] >> 6, GS_PSM_16, 0, 0, 1),
	   GS_REG_TEX0);
	AD(p, GS_SET_CLAMP(0, 0, 0, 0, 0, 0), GS_REG_CLAMP);
	AD(p, (u64)c | ((u64)alpha << 24) | ((u64)0x3f800000 << 32), GS_REG_RGBAQ);
	bool on_map_texture = true;

	struct SLineMatrixData *l = &LineMatrixData[GFX.StartY];
	for (uint32 Line = GFX.StartY; Line <= GFX.EndY; Line++, l++)
	{
		/* The software renderer's arithmetic (tileimpl.h), up to the
		 * coordinates of the first pixel and the step per pixel. */
		int32 HOffset = ((int32)l->M7HOFS << 19) >> 19;
		int32 VOffset = ((int32)l->M7VOFS << 19) >> 19;
		int32 CentreX = ((int32)l->CentreX << 19) >> 19;
		int32 CentreY = ((int32)l->CentreY << 19) >> 19;

		int starty = PPU.Mode7VFlip ? 255 - (int)(Line + 1) : (int)(Line + 1);
		int yy = VOffset - CentreY;
		yy = (yy & 0x2000) ? (yy | ~0x3ff) : (yy & 0x3ff);

		int BB = ((l->MatrixB * starty) & ~63) + ((l->MatrixB * yy) & ~63) + (CentreX << 8);
		int DD = ((l->MatrixD * starty) & ~63) + ((l->MatrixD * yy) & ~63) + (CentreY << 8);

		int startx, aa, cc;
		if (PPU.Mode7HFlip)
		{
			startx = (int)Right - 1;
			aa = -l->MatrixA;
			cc = -l->MatrixC;
		}
		else
		{
			startx = (int)Left;
			aa = l->MatrixA;
			cc = l->MatrixC;
		}

		int xx = HOffset - CentreX;
		xx = (xx & 0x2000) ? (xx | ~0x3ff) : (xx & 0x3ff);
		int X0 = l->MatrixA * startx + ((l->MatrixA * xx) & ~63) + BB;
		int Y0 = l->MatrixC * startx + ((l->MatrixC * xx) & ~63) + DD;

		/* Pixel i of the span reads the texel at floor((X0 + aa * i) / 256).
		 * Half a 1/256 step is added so interpolation error cannot pull an
		 * exact value under its texel boundary. */
		const float scale = 1.0f / (float)M7_LIMIT;
		#define M7_S(i) (((float)(X0 + aa * (i)) + 0.5f - xbase) * scale)
		#define M7_T(i) (((float)(Y0 + cc * (i)) + 0.5f - ybase) * scale)

		if (!repeat)
		{
			/* Wrapping: bring the span near the origin (texture
			 * coordinates lose precision far from it). */
			int xmin = aa < 0 ? X0 + aa * n : X0, ymin = cc < 0 ? Y0 + cc * n : Y0;
			float xbase = (float)((xmin >> 18) << 18), ybase = (float)((ymin >> 18) << 18);
			m7_strip((int)Left, (int)Right, (int)Line, z, M7_S(0), M7_T(0), M7_S(n), M7_T(n));
			continue;
		}

		const float xbase = 0, ybase = 0;
		int i0 = 0, i1 = n;
		m7_inside(X0, aa, n, i0, i1);
		m7_inside(Y0, cc, n, i0, i1);
		if (i1 > i0)
		{
			if (!on_map_texture)
			{
				p = ad(2);
				AD(p, GS_SET_TEX0(vram->m7 >> 6, 1024 / 64, GS_PSM_8, 10, 10, 1, 0, vram->clut[1] >> 6, GS_PSM_16, 0, 0, 1),
				   GS_REG_TEX0);
				AD(p, GS_SET_CLAMP(0, 0, 0, 0, 0, 0), GS_REG_CLAMP);
				on_map_texture = true;
			}
			m7_strip((int)Left + i0, (int)Left + i1, (int)Line, z, M7_S(i0), M7_T(i0), M7_S(i1), M7_T(i1));
		}
		if (repeat == 3 && (i0 > 0 || i1 < n))
		{
			/* Outside the map: character 0, repeating. An 8x8 corner of
			 * the 8bpp atlas (512x256) with region repeat; coordinates
			 * reduced to one character first. */
			if (on_map_texture)
			{
				p = ad(2);
				AD(p, GS_SET_TEX0(vram->atlas8 >> 6, 512 / 64, GS_PSM_8, 9, 8, 1, 0, vram->clut[1] >> 6, GS_PSM_16, 0, 0, 1),
				   GS_REG_TEX0);
				AD(p, GS_SET_CLAMP(3, 3, 7, 0, 7, 0), GS_REG_CLAMP);
				on_map_texture = false;
			}
			for (int part = 0; part < 2; part++)
			{
				int a = part ? i1 : 0, b = part ? n : i0;
				if (i1 == i0)
					a = 0, b = part ? 0 : n; /* nothing inside: one piece */
				if (b <= a)
					continue;
				int xa = X0 + aa * a, ya = Y0 + cc * a;
				int xmin = aa < 0 ? xa + aa * (b - a) : xa, ymin = cc < 0 ? ya + cc * (b - a) : ya;
				int xb = (xmin >> 11) << 11, yb = (ymin >> 11) << 11; /* whole characters */
				m7_strip((int)Left + a, (int)Left + b, (int)Line, z,
					 ((float)(xa - xb) + 0.5f) / (512.0f * 256.0f), ((float)(ya - yb) + 0.5f) / (256.0f * 256.0f),
					 ((float)(xa + aa * (b - a) - xb) + 0.5f) / (512.0f * 256.0f),
					 ((float)(ya + cc * (b - a) - yb) + 0.5f) / (256.0f * 256.0f));
			}
		}
		#undef M7_S
		#undef M7_T
	}

	/* Back to what the tile sprites expect. */
	p = ad(1);
	AD(p, GS_SET_CLAMP(1, 1, 0, 0, 0, 0), GS_REG_CLAMP);
}

static void gs_mode7_nomath(uint32 Left, uint32 Right, int D)
{
	S9xGSMode7(Left, Right, D, FALSE);
}
static void gs_mode7_math(uint32 Left, uint32 Right, int D)
{
	S9xGSMode7(Left, Right, D, TRUE);
}

/* Backdrop: depth 1, colour 0, wherever no layer drew. */
static void backdrop(uint32 Left, uint32 Right, bool math)
{
	if (screen_sub)
		return; /* the sub screen's backdrop is the fixed colour it was cleared to */
	set_ztest(ZTST_GREATER);
	unsigned c = GFX.ClipColors ? 0 : rgb8(IPPU.ScreenColors[0]);
	flat_rect((int)Left, GFX.StartY, (int)Right, GFX.EndY + 1, 1, c, math ? 0x80 : 0x40, 0);
}
static void gs_backdrop_nomath(uint32, uint32 Left, uint32 Right)
{
	backdrop(Left, Right, false);
}
static void gs_backdrop_math(uint32, uint32 Left, uint32 Right)
{
	backdrop(Left, Right, true);
}

void S9xGSSelectRenderers(void)
{
	if (!band_active)
		return;
	GFX.DrawTileNomath = gs_tile_nomath;
	GFX.DrawTileMath = gs_tile_math;
	GFX.DrawClippedTileNomath = gs_clipped_nomath;
	GFX.DrawClippedTileMath = gs_clipped_math;
	GFX.DrawBackdropNomath = gs_backdrop_nomath;
	GFX.DrawBackdropMath = gs_backdrop_math;
	GFX.DrawMode7BG1Nomath = gs_mode7_nomath;
	GFX.DrawMode7BG1Math = gs_mode7_math;
}

/* ---- bands ---------------------------------------------------------- */

/* Can the GS draw a band in the current PPU state? */
static const char *unsupported(void)
{
	if (PPU.BGMode == 5 || PPU.BGMode == 6)
		return "hi-res mode";
	if (PPU.BGMode == 7)
	{
		if (vram->m7 <= 0)
			return "mode 7 (no GS memory for the map)";
		if (Memory.FillRAM[0x2133] & 0x40)
			return "mode 7 EXTBG";
		if (Memory.FillRAM[0x2130] & 1)
			return "direct colour";
	}
	if (IPPU.PseudoHires || IPPU.DoubleWidthPixels || IPPU.DoubleHeightPixels || IPPU.Interlace)
		return "hi-res / interlace";
	uint8 layers = Memory.FillRAM[0x212c] | Memory.FillRAM[0x212d];
	if (PPU.Mosaic > 1)
		for (int n = 0; n < 4; n++)
			if (PPU.BGMosaic[n] && (layers & (1 << n)))
				return "mosaic";
	if ((Memory.FillRAM[0x2130] & 1) && (PPU.BGMode == 3 || PPU.BGMode == 4))
		return "direct colour";
	bool math = (Memory.FillRAM[0x2130] & 0x30) != 0x30 && (Memory.FillRAM[0x2131] & 0x3f);
	if (math && IPPU.MaxBrightness != 0xf)
		return "colour math with brightness";
	if (math && (Memory.FillRAM[0x2131] & 0x40) && (Memory.FillRAM[0x2130] & 0xc0))
		return "half colour math with colour window";
	if (Settings.ForcedBackdrop)
		return "forced backdrop";
	if (GFX.PPL != 512)
		return "pitch";
	return NULL;
}

bool8 S9xGSBandBegin(void)
{
	PS2_STAGE("gs: band");
	if (!enabled || !vram || !frame_open)
		return FALSE;
	const char *why = unsupported();
	if (why)
	{
		ps2_gs_stats.sw_reason = why;
		return FALSE;
	}
	band_active = true;
	sub_drawn = false;
	in_obj = false;
	frame_has_gs = true;
	ps2_gs_stats.gs_bands++;
	clut_update();
	if (PPU.BGMode == 7)
		m7_update();
	return TRUE;
}

/* Colour math for the band just drawn: blend the sub screen (or the fixed
 * colour) onto the main-screen pixels whose alpha bit is set. */
static void band_math(void)
{
	uint8 r2130 = Memory.FillRAM[0x2130], r2131 = Memory.FillRAM[0x2131];
	if ((r2130 & 0x30) == 0x30 || !(r2131 & 0x3f))
		return;
	bool subtract = (r2131 & 0x80) != 0, half = (r2131 & 0x40) != 0, use_sub = (r2130 & 2) != 0;

	/* (Cs - 0) * F + Cd, or (Cd - Cs) * F + 0; F = 1 or 1/2. Halved
	 * addition is (Cs - Cd) / 2 + Cd. */
	u64 full = subtract ? GS_SET_ALPHA(1, 0, 2, 2, 0x80) : GS_SET_ALPHA(0, 2, 2, 1, 0x80);
	u64 halved = subtract ? GS_SET_ALPHA(1, 0, 2, 2, 0x40) : GS_SET_ALPHA(0, 1, 2, 1, 0x40);

	int y0 = GFX.StartY, y1 = GFX.EndY + 1;
	u64 *p = ad(3);
	AD(p, GS_SET_FRAME(vram->target >> 11, 512 / 64, GS_PSM_16, 0x80000000), GS_REG_FRAME); /* keep alpha */
	AD(p, GS_SET_ZBUF(vram->z >> 11, GS_PSMZ_16 & 0xf, 1), GS_REG_ZBUF);
	AD(p, GS_SET_SCISSOR(0, 255, GFX.StartY, GFX.EndY), GS_REG_SCISSOR);
	z_test = -1;

	if (!sub_drawn)
	{
		/* Fixed colour everywhere. Half applies only when the sub screen
		 * is not part of the math at all. */
		p = ad(2);
		AD(p, GS_SET_TEST(0, 0, 0, 0, 1, 1, 1, ZTST_ALWAYS), GS_REG_TEST);
		AD(p, (half && !use_sub) ? halved : full, GS_REG_ALPHA);
		flat_rect(0, y0, 256, y1, 0, rgb8(GFX.FixedColour), 0x80, 1);
		return;
	}

	/* The sub screen as a texture: its pixels where a sub layer drew
	 * (alpha set), the fixed colour elsewhere. Halving applies only to
	 * real sub-screen pixels. */
	p = ad(2);
	AD(p, GS_SET_TEX0(vram->sub >> 6, 512 / 64, GS_PSM_16, 9, 8, 1, 1, 0, 0, 0, 0, 0), GS_REG_TEX0);
	AD(p, 0, GS_REG_TEXFLUSH);
	for (int pass = 0; pass < (half ? 2 : 1); pass++)
	{
		p = ad(7);
		if (!half)
			AD(p, GS_SET_TEST(0, 0, 0, 0, 1, 1, 1, ZTST_ALWAYS), GS_REG_TEST);
		else if (pass == 0) /* sub-screen pixels: alpha >= 0x80 */
			AD(p, GS_SET_TEST(1, 5, 0x80, 0, 1, 1, 1, ZTST_ALWAYS), GS_REG_TEST);
		else /* fixed colour: alpha < 0x80 */
			AD(p, GS_SET_TEST(1, 2, 0x80, 0, 1, 1, 1, ZTST_ALWAYS), GS_REG_TEST);
		AD(p, (half && pass == 0) ? halved : full, GS_REG_ALPHA);
		AD(p, GS_SET_PRIM(6, 0, 1, 0, 1, 0, 1, 0, 0), GS_REG_PRIM);
		AD(p, GS_SET_UV(8, y0 * 16 + 8), GS_REG_UV);
		AD(p, xyz(0, y0, 0), GS_REG_XYZ2);
		AD(p, GS_SET_UV(256 * 16 + 8, y1 * 16 + 8), GS_REG_UV);
		AD(p, xyz(256, y1, 0), GS_REG_XYZ2);
	}
}

void S9xGSBandEnd(void)
{
	if (!band_active)
		return;
	run_close();
	band_math();
	band_active = false;
}

/* The band was rendered by the software renderer into GFX.Screen. */
void S9xGSBandSoftware(void)
{
	if (!enabled || !frame_open)
		return;
	ps2_gs_stats.sw_bands++;
	if (sw_band_count < sizeof(sw_bands) / sizeof(sw_bands[0]))
	{
		sw_bands[sw_band_count].y0 = (uint16)GFX.StartY;
		sw_bands[sw_band_count].y1 = (uint16)GFX.EndY;
		sw_band_count++;
	}
}

/* Compare mode: the band is drawn by the software renderer first, with
 * the GS renderers out of the way. */
bool8 S9xGSCompareMode(void)
{
	return enabled && ps2_gs_compare;
}

bool8 S9xGSActive(void)
{
	return enabled;
}

void S9xGSBandPause(bool8 pause)
{
	band_active = !pause;
}

/* ---- frames --------------------------------------------------------- */

/* Overlay text (profiler, frame rate, core messages) is drawn into
 * GFX.Screen, which a GS frame does not otherwise use: the strips it can
 * appear in are cleared when the frame starts and laid over the picture
 * at the end, black being transparent. */
#define OVERLAY_TOP 104
#define OVERLAY_BOTTOM 40
static bool frame_overlay = false;

static void overlay_clear(int y0, int y1)
{
	for (int y = y0; y < y1; y++)
		memset(GFX.Screen + y * GFX.RealPPL, 0, 256 * sizeof(uint16));
}

static void upload_rows(int dbp, int y0, int y1, int width);

/* Four pixels at a time when the row is 8-byte aligned (the R5900 traps
 * on a misaligned load, and GFX.Screen comes from a std::vector). */
static inline bool row_blank(int y)
{
	const uint16 *px = GFX.Screen + y * GFX.RealPPL;
	if (((unsigned)(uintptr_t)px & 7) == 0)
	{
		const u64 *row = (const u64 *)px;
		for (int i = 0; i < 64; i++)
			if (row[i])
				return false;
		return true;
	}
	for (int x = 0; x < 256; x++)
		if (px[x])
			return false;
	return true;
}

static void overlay_strip(int y0, int y1)
{
	/* only the rows that have something on them */
	while (y0 < y1 && row_blank(y0))
		y0++;
	while (y1 > y0 && row_blank(y1 - 1))
		y1--;
	if (y0 >= y1)
		return;

	upload_rows(vram->sub >> 6, y0, y1 - 1, 256);
	u64 *p = ad(13);
	AD(p, 0, GS_REG_TEXFLUSH);
	AD(p, GS_SET_FRAME(vram->target >> 11, 512 / 64, GS_PSM_16, 0), GS_REG_FRAME);
	AD(p, GS_SET_ZBUF(vram->z >> 11, GS_PSMZ_16 & 0xf, 1), GS_REG_ZBUF);
	AD(p, GS_SET_SCISSOR(0, 255, y0, y1 - 1), GS_REG_SCISSOR);
	AD(p, GS_SET_TEXA(0x80, 1, 0x80), GS_REG_TEXA); /* black: alpha 0 */
	AD(p, GS_SET_TEX1(1, 0, 0, 0, 0, 0, 0), GS_REG_TEX1);
	AD(p, GS_SET_TEX0(vram->sub >> 6, 512 / 64, GS_PSM_16, 9, 8, 1, 1, 0, 0, 0, 0, 0), GS_REG_TEX0);
	AD(p, GS_SET_TEST(1, 6, 0, 0, 0, 0, 1, ZTST_ALWAYS), GS_REG_TEST);
	AD(p, GS_SET_PRIM(6, 0, 1, 0, 0, 0, 1, 0, 0), GS_REG_PRIM);
	AD(p, GS_SET_UV(8, y0 * 16 + 8), GS_REG_UV);
	AD(p, xyz(0, y0, 0), GS_REG_XYZ2);
	AD(p, GS_SET_UV(256 * 16 + 8, y1 * 16 + 8), GS_REG_UV);
	AD(p, xyz(256, y1, 0), GS_REG_XYZ2);
	z_test = -1;
}

void ps2_gs_frame_begin(bool overlay)
{
	static int generation = -1;
	frame_open = enabled && (vram = ps2_video_gs_vram()) != NULL;
	if (frame_open && vram->generation != generation)
	{
		/* GS memory was laid out again (video mode change): all gone. */
		generation = vram->generation;
		ps2_gs_reset();
	}
	frame_has_gs = false;
	band_active = false;
	sw_band_count = 0;
	const char *reason = ps2_gs_stats.sw_reason;
	unsigned cmp_pixels = ps2_gs_stats.cmp_pixels, cmp_frames = ps2_gs_stats.cmp_frames;
	memset(&ps2_gs_stats, 0, sizeof(ps2_gs_stats));
	ps2_gs_stats.sw_reason = reason;
	ps2_gs_stats.cmp_pixels = cmp_pixels;
	ps2_gs_stats.cmp_frames = cmp_frames;
	frame_overlay = frame_open && overlay;
	if (frame_open)
	{
		atlas_setup();
		stream_begin();
		if (frame_overlay)
		{
			overlay_clear(0, OVERLAY_TOP);
			overlay_clear(PPU.ScreenHeight - OVERLAY_BOTTOM, PPU.ScreenHeight);
		}
	}
}

/* Rows y0..y1 of GFX.Screen into a 512-wide buffer. */
static void upload_rows(int dbp, int y0, int y1, int width)
{
	int w = (width + 7) & ~7;
	for (int y = y0; y <= y1;)
	{
		int rows = y1 - y + 1;
		if (rows > 8) /* 256 qwords at a time */
			rows = 8;
		uint16 *d = (uint16 *)image(dbp, 512 / 64, GS_PSM_16, 0, y, w, rows, (unsigned)(w * rows * 2));
		for (int r = 0; r < rows; r++)
			memcpy(d + r * w, GFX.Screen + (y + r) * GFX.RealPPL, (size_t)w * 2);
		y += rows;
	}
}

static void compare(int width, int height)
{
	/* The target is 512 pixels wide in GS memory: read it as such. */
	static uint16 *back = NULL;
	if (!back)
		back = (uint16 *)memalign(64, 512 * 240 * 2);
	if (!back || width != 256 || height > 240)
		return;

	stream_send();
	dma_wait_fast();
	ps2_video_settle();
	PS2_STAGE("gs: reading the picture back");
	if (!ps2_screenshot(back, (unsigned)(vram->target >> 6), 0, 0, 512, 240, GS_PSM_16))
		return;

	unsigned diff = 0, first_x = 0, first_y = 0, sw = 0, gs = 0, rows = 0;
	unsigned min_x = 255, max_x = 0, max_y = 0;
	for (int y = 0; y < height; y++)
	{
		const uint16 *a = GFX.Screen + y * GFX.RealPPL, *b = back + y * 512;
		unsigned row = 0;
		for (int x = 0; x < 256; x++)
			if ((a[x] ^ b[x]) & 0x7FFF)
			{
				if (!diff && !row)
					first_x = x, first_y = y, sw = a[x] & 0x7FFF, gs = b[x] & 0x7FFF;
				if ((unsigned)x < min_x)
					min_x = x;
				if ((unsigned)x > max_x)
					max_x = x;
				max_y = y;
				row++;
			}
		diff += row;
		rows += row != 0;
	}
	ps2_gs_stats.cmp_pixels = diff;
	ps2_gs_stats.cmp_frames++;

	static unsigned frame = 0, reported = 0;
	frame++;
	if (diff && (reported < 40 || frame % 60 == 0))
	{
		reported++;
		char line[200];
		snprintf(line, sizeof(line),
			 "GSCMP frame %u: %u pixels differ in %u rows, box x %u-%u y %u-%u; first at %u,%u software %04x gs %04x (mode %d, %u gs bands, %u sw)",
			 frame, diff, rows, min_x, max_x, first_y, max_y, first_x, first_y, sw, gs, PPU.BGMode,
			 ps2_gs_stats.gs_bands, ps2_gs_stats.sw_bands);
		printf("%s\n", line);
		ps2_net_log(line);
		if (reported <= 3)
		{
			/* the row of the first difference, 16 pixels from it */
			char *o = line;
			o += sprintf(o, "GSCMP row %u from x=%u sw:", first_y, first_x & ~7u);
			for (unsigned x = first_x & ~7u; x < (first_x & ~7u) + 12 && x < 256; x++)
				o += sprintf(o, " %04x", GFX.Screen[first_y * GFX.RealPPL + x] & 0x7FFF);
			printf("%s\n", line);
			o = line;
			o += sprintf(o, "GSCMP row %u from x=%u gs:", first_y, first_x & ~7u);
			for (unsigned x = first_x & ~7u; x < (first_x & ~7u) + 12 && x < 256; x++)
				o += sprintf(o, " %04x", back[first_y * 512 + x]);
			printf("%s\n", line);
		}
	}
	else if (frame % 300 == 0)
	{
		char line[120];
		snprintf(line, sizeof(line), "GSCMP frame %u: identical (%u gs bands, %u sw)", frame, ps2_gs_stats.gs_bands,
			 ps2_gs_stats.sw_bands);
		printf("%s\n", line);
		ps2_net_log(line);
	}
}

static bool frame_is_gs = false;

bool ps2_gs_frame_picture(int width, int height)
{
	frame_is_gs = false;
	if (!frame_open)
		return false;
	frame_open = false;
	if (!frame_has_gs || width != 256)
		return false; /* a software frame: GFX.Screen has all of it */
	frame_is_gs = true;

	for (unsigned i = 0; i < sw_band_count; i++)
		upload_rows(vram->target >> 6, sw_bands[i].y0, sw_bands[i].y1, width);
	if (ps2_gs_compare)
		compare(width, height);
	return true;
}

void ps2_gs_frame_end(int width, int height)
{
	if (!frame_is_gs)
		return;
	frame_is_gs = false;
	(void)width;
	if (frame_overlay)
	{
		overlay_strip(0, OVERLAY_TOP < height ? OVERLAY_TOP : height);
		if (height > OVERLAY_TOP + OVERLAY_BOTTOM)
			overlay_strip(height - OVERLAY_BOTTOM, height);
	}

	/* (ps2_video_present_target waits for this before it sends) */
	stream_send();
	PS2_STAGE("gs: frame sent");
}

void ps2_gs_reset(void)
{
	memset(S9xGSTileValid2, 0, sizeof(S9xGSTileValid2));
	memset(S9xGSTileValid4, 0, sizeof(S9xGSTileValid4));
	memset(S9xGSTileValid8, 0, sizeof(S9xGSTileValid8));
	clut_valid = false;
	m7_valid = false;
}

void ps2_gs_enable(bool on)
{
	if (on && !stream[0])
	{
		stream[0] = (u64 *)memalign(64, STREAM_QW * 16);
		stream[1] = (u64 *)memalign(64, STREAM_QW * 16);
		obj_recs = (TileRec *)memalign(64, OBJ_MAX * sizeof(TileRec));
		if (!stream[0] || !stream[1] || !obj_recs)
			on = false;
	}
	ps2_video_set_gs_layout(on ? 1 : 0);
	enabled = on && ps2_video_gs_vram() != NULL;
	frame_open = false;
	band_active = false;
	ps2_gs_reset();
}

bool ps2_gs_enabled(void)
{
	return enabled;
}

void ps2_gs_log_stats(void)
{
	static unsigned frames = 0;
	extern int ps2_prof_enabled;
	if (++frames % 60 || !ps2_prof_enabled)
		return;
	char line[200];
	snprintf(line, sizeof(line), "GS bands %u gs %u sw (%s) | tiles %u uploads %u | %u bytes | compare: %u px differ, %u frames",
		 ps2_gs_stats.gs_bands, ps2_gs_stats.sw_bands, ps2_gs_stats.sw_reason ? ps2_gs_stats.sw_reason : "-",
		 ps2_gs_stats.tiles, ps2_gs_stats.uploads, ps2_gs_stats.bytes, ps2_gs_stats.cmp_pixels,
		 ps2_gs_stats.cmp_frames);
	printf("%s\n", line);
	ps2_net_log(line);
}
