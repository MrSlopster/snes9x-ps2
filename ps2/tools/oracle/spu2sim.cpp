/* A small software SPU2, for checking the fast sound path on the host:
 * plays snesspu events (ps2/snesspu/snesspu.h) the way snesspu.irx and
 * the chip would, into 48 kHz stereo. Models what the path relies on -
 * ADPCM decoding with loop flags, pitch, voice and master volumes, the
 * key-off release ramp, noise, pitch modulation - with linear
 * interpolation in place of the chip's Gaussian filter. */

#include <stdio.h>
#include <string.h>
#include <vector>

#include "spu2sim.h"

static uint8_t ram[2 * 1024 * 1024];

struct SimVoice
{
	bool on, releasing;
	uint32_t addr, loop_addr; /* current block, loop target */
	int pos;                  /* next sample in blk */
	int16_t blk[28];
	int flags;
	int s1, s2;               /* ADPCM predictor */
	int counter;              /* 12-bit fraction */
	int prev, cur;            /* last two samples, for interpolation */
	int pitch, vol_l, vol_r;
	int env;                  /* 0..0x7FFF */
	int out;                  /* after envelope, for pitch modulation */
};
static SimVoice voices[SNESSPU_VOICES];
static int mvol_l, mvol_r, non, pmon, noise_clk;
static int noise_level, noise_timer;
static std::vector<snesspu_event> pending;
static size_t pending_pos;
static unsigned long long out_pos; /* 48 kHz samples rendered */
static FILE *wav;
static unsigned wav_frames;

static int sext15(int v)
{
	return (v & 0x4000) ? (v | ~0x7FFF) : (v & 0x7FFF);
}

static int clamp16(int s)
{
	return s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
}

static void decode_block(SimVoice &v)
{
	static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
	const uint8_t *b = &ram[v.addr & (sizeof(ram) - 1)];
	int shift = b[0] & 15, filter = b[0] >> 4;
	if (filter > 4)
		filter = 4;
	if (shift > 12)
		shift = 9;
	v.flags = b[1];
	if (v.flags & 4)
		v.loop_addr = v.addr;
	for (int i = 0; i < 28; i++)
	{
		int nyb = (b[2 + (i >> 1)] >> ((i & 1) ? 4 : 0)) & 15;
		int s = (int16_t)(nyb << 12) >> shift;
		s += (v.s1 * f0[filter] + v.s2 * f1[filter] + 32) >> 6;
		s = clamp16(s);
		v.blk[i] = (int16_t)s;
		v.s2 = v.s1;
		v.s1 = s;
	}
	v.pos = 0;
}

static void next_sample(SimVoice &v)
{
	if (v.pos >= 28)
	{
		if (v.flags & 1)
		{
			v.addr = v.loop_addr;
			if (!(v.flags & 2))
			{
				v.env = 0; /* end without repeat: muted */
				v.releasing = true;
			}
		}
		else
			v.addr += 16;
		decode_block(v);
	}
	v.prev = v.cur;
	v.cur = v.blk[v.pos++];
}

static void apply(const snesspu_event &e)
{
	SimVoice &v = voices[e.voice & (SNESSPU_VOICES - 1)];
	switch (e.type)
	{
	case SNESSPU_EV_KON:
		v.on = true;
		v.releasing = false;
		v.addr = v.loop_addr = (uint32_t)e.value * 16;
		v.s1 = v.s2 = 0;
		v.counter = 0;
		v.prev = v.cur = 0;
		v.env = 0x7FFF;
		decode_block(v);
		break;
	case SNESSPU_EV_KOFF:
		v.releasing = true;
		break;
	case SNESSPU_EV_VOLL:
		v.vol_l = sext15(e.value);
		break;
	case SNESSPU_EV_VOLR:
		v.vol_r = sext15(e.value);
		break;
	case SNESSPU_EV_PITCH:
		v.pitch = e.value & 0x3FFF;
		break;
	case SNESSPU_EV_MVOLL:
		mvol_l = sext15(e.value);
		break;
	case SNESSPU_EV_MVOLR:
		mvol_r = sext15(e.value);
		break;
	case SNESSPU_EV_NON:
		non = e.value;
		break;
	case SNESSPU_EV_PMON:
		pmon = e.value;
		break;
	case SNESSPU_EV_NOISE:
		noise_clk = e.value & 63;
		break;
	case SNESSPU_EV_ALL_OFF:
		for (int i = 0; i < SNESSPU_VOICES; i++)
		{
			voices[i].on = false;
			voices[i].env = 0;
			voices[i].vol_l = voices[i].vol_r = 0;
		}
		break;
	}
}

static void render_one(int16_t *lr)
{
	/* Noise generator (as documented for the PS1 SPU). */
	noise_timer -= 4 + (noise_clk & 3);
	if (noise_timer < 0)
	{
		int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
		noise_level = (noise_level * 2 + parity) & 0xFFFF;
		noise_timer += 0x20000 >> (noise_clk >> 2);
		if (noise_timer < 0)
			noise_timer += 0x20000 >> (noise_clk >> 2);
	}

	int l = 0, r = 0, prev_out = 0;
	for (int i = 0; i < SNESSPU_VOICES; i++)
	{
		SimVoice &v = voices[i];
		if (!v.on)
		{
			v.out = prev_out = 0;
			continue;
		}
		int step = v.pitch;
		if (i && (pmon & (1 << i)))
			step = (step * (prev_out + 0x8000)) >> 15;
		if (step > 0x3FFF)
			step = 0x3FFF;
		v.counter += step;
		while (v.counter >= 0x1000)
		{
			v.counter -= 0x1000;
			next_sample(v);
		}
		int s = v.prev + (((v.cur - v.prev) * v.counter) >> 12);
		if (non & (1 << i))
			s = (int16_t)noise_level;

		if (v.releasing)
		{
			v.env -= 8 << 3; /* release rate 8, linear */
			if (v.env <= 0)
			{
				v.env = 0;
				v.on = false;
			}
		}
		s = (s * v.env) >> 15;
		v.out = prev_out = s;
		l += (s * v.vol_l) >> 14;
		r += (s * v.vol_r) >> 14;
	}
	lr[0] = (int16_t)clamp16((clamp16(l) * mvol_l) >> 14);
	lr[1] = (int16_t)clamp16((clamp16(r) * mvol_r) >> 14);
}

static void wav_header(unsigned frames)
{
	unsigned bytes = frames * 4;
	unsigned char h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
				0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
	unsigned riff = bytes + 36;
	memcpy(h + 4, &riff, 4);
	memcpy(h + 40, &bytes, 4);
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
}

bool spu2sim_open(const char *wav_path)
{
	memset(voices, 0, sizeof(voices));
	memset(ram, 0, sizeof(ram));
	noise_level = 1;
	wav = fopen(wav_path, "wb");
	if (!wav)
		return false;
	wav_header(0);
	return true;
}

void spu2sim_upload(uint32_t addr, const uint8_t *data, uint32_t size)
{
	if (addr % 64 || size % 64 || addr < SNESSPU_RAM_BASE || addr + size > SNESSPU_RAM_END)
	{
		fprintf(stderr, "spu2sim: bad upload %x + %x\n", addr, size);
		return;
	}
	memcpy(&ram[addr], data, size);
}

void spu2sim_events(const snesspu_event *ev, unsigned count, uint32_t now)
{
	for (unsigned i = 0; i < count; i++)
	{
		if (!pending.empty() && (int)(ev[i].time - pending.back().time) < 0)
			fprintf(stderr, "spu2sim: events out of order (%u after %u)\n", ev[i].time, pending.back().time);
		pending.push_back(ev[i]);
	}

	/* Render up to 'now' (32 kHz samples -> 48 kHz). */
	unsigned long long until = (unsigned long long)now * 3 / 2;
	while (out_pos < until)
	{
		while (pending_pos < pending.size() &&
		       (unsigned long long)pending[pending_pos].time * 3 / 2 <= out_pos)
			apply(pending[pending_pos++]);
		int16_t lr[2];
		render_one(lr);
		fwrite(lr, 2, 2, wav);
		wav_frames++;
		out_pos++;
	}
	if (pending_pos == pending.size())
	{
		pending.clear();
		pending_pos = 0;
	}
}

void spu2sim_close(void)
{
	if (!wav)
		return;
	wav_header(wav_frames);
	fclose(wav);
	wav = NULL;
}
