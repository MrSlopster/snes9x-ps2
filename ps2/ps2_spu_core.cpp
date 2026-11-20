/* Fast sound, portable half (see ps2_spu_core.h). */

#include <string.h>
#include <vector>

#include "ps2_spu_core.h"
#include "ps2_brr_adpcm.h"

SpuCoreStats spu_core_stats;

static const uint8_t *apuram;
static const SpuBackend *backend;
static std::vector<snesspu_event> queue;

/* ---- samples -------------------------------------------------------- */

/* A sample is identified by its two directory words and the BRR bytes
 * they lead to: programs reuse directory slots and addresses for new
 * data. */
struct LoadedSample
{
	uint16_t start, loop;
	uint32_t brr_hash;
	uint16_t block; /* SPU2 RAM address / 16 */
};
static std::vector<LoadedSample> loaded;
static uint32_t ram_next = SNESSPU_RAM_BASE;

void spu_core_forget_samples(void)
{
	loaded.clear();
	ram_next = SNESSPU_RAM_BASE;
}

/* Hash of the BRR data of a sample, without decoding it. False if the
 * data does not end. */
static bool brr_hash(uint16_t start, uint16_t loop, uint32_t *out)
{
	uint32_t h = 2166136261u;
	uint16_t addr = start;
	for (int pass = 0; pass < 2; pass++)
	{
		unsigned n;
		int header = 0;
		for (n = 0; n < 8192; n++)
		{
			header = apuram[addr];
			for (int i = 0; i < 9; i++)
				h = (h ^ apuram[(uint16_t)(addr + i)]) * 16777619u;
			if (header & 1)
				break;
			addr = (uint16_t)(addr + 9);
		}
		if (n == 8192)
			return false;
		if (!(header & 2))
			break;
		addr = loop;
	}
	uint8_t dir[4] = { (uint8_t)start, (uint8_t)(start >> 8), (uint8_t)loop, (uint8_t)(loop >> 8) };
	for (int i = 0; i < 4; i++)
		h = (h ^ dir[i]) * 16777619u;
	*out = h;
	return true;
}

/* SPU2 block number of the sample, transcoding and uploading it on first
 * use. -1 if the data is not a sample. */
static int sample_block(uint16_t start, uint16_t loop)
{
	uint32_t hash;
	if (!brr_hash(start, loop, &hash))
		return -1;
	for (size_t i = 0; i < loaded.size(); i++)
		if (loaded[i].brr_hash == hash && loaded[i].start == start && loaded[i].loop == loop)
			return loaded[i].block;

	SpuSample s;
	if (!brr_to_spu_adpcm(apuram, start, loop, s))
		return -1;

	/* Whole 64-byte units (the transfer size of the SPU2 DMA); the
	 * padding after the end block is never played. */
	while (s.adpcm.size() % 64)
		s.adpcm.push_back(0);
	uint32_t size = (uint32_t)s.adpcm.size();
	if (size > SNESSPU_RAM_END - SNESSPU_RAM_BASE)
		return -1;
	if (ram_next + size > SNESSPU_RAM_END)
	{
		/* Full: start over. Voices still playing old samples will play
		 * whatever replaces them until their next key-on. */
		spu_core_forget_samples();
		spu_core_stats.ram_resets++;
	}

	uint32_t addr = ram_next;
	ram_next += size;
	for (uint32_t off = 0; off < size; off += SNESSPU_UPLOAD_MAX)
	{
		uint32_t n = size - off < SNESSPU_UPLOAD_MAX ? size - off : SNESSPU_UPLOAD_MAX;
		backend->upload(addr + off, &s.adpcm[off], n);
	}
	spu_core_stats.samples++;
	spu_core_stats.sample_bytes += size;

	LoadedSample l;
	l.start = start;
	l.loop = loop;
	l.brr_hash = hash;
	l.block = (uint16_t)(addr / 16);
	loaded.push_back(l);
	return l.block;
}

/* ---- events --------------------------------------------------------- */

static void push(unsigned time, int type, int voice, int value)
{
	snesspu_event e;
	e.time = time;
	e.type = (unsigned char)type;
	e.voice = (unsigned char)voice;
	e.value = (unsigned short)value;
	queue.push_back(e);
	spu_core_stats.events++;
}

/* What the SPU2 was last told, to send only changes. */
static struct
{
	int vol_l, vol_r, pitch;
} sent[SNESSPU_VOICES];
static int sent_mvol_l, sent_mvol_r, sent_non, sent_pmon, sent_noise;

/* SPU2 noise clock for each SNES noise rate (FLG & 0x1F). The SPU2
 * steps its noise generator at 48000 * step / (0x20000 >> shift) Hz with
 * clock = shift * 4 + (step - 4); pick the nearest to 32000 / period. */
static unsigned char noise_clock[32];

static void init_noise_clocks(void)
{
	static const unsigned short period[32] = { 0,   2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256,
						   192, 160,  128,  96,   80,   64,  48,  40,  32,  24,  20,
						   16,  12,   10,   8,    6,    5,   4,   3,   2,   1 };
	for (int r = 1; r < 32; r++)
	{
		double want = 32000.0 / period[r], best = 1e30;
		for (int clk = 0; clk < 64; clk++)
		{
			double hz = 48000.0 * (4 + (clk & 3)) / (double)(0x20000 >> (clk >> 2));
			double ratio = hz > want ? hz / want : want / hz;
			if (ratio < best)
			{
				best = ratio;
				noise_clock[r] = (unsigned char)clk;
			}
		}
	}
	noise_clock[0] = 0; /* rate 0 never steps; slowest is the closest */
}

void spu_core_init(const uint8_t *ram, const SpuBackend *b)
{
	apuram = ram;
	backend = b;
	queue.clear();
	spu_core_forget_samples();
	init_noise_clocks();
	memset(&spu_core_stats, 0, sizeof(spu_core_stats));
	spu_core_reset(0);
}

void spu_core_reset(unsigned time)
{
	for (int v = 0; v < SNESSPU_VOICES; v++)
	{
		sent[v].vol_l = sent[v].vol_r = 0;
		sent[v].pitch = -1;
	}
	sent_mvol_l = sent_mvol_r = sent_non = sent_pmon = sent_noise = -1;
	queue.clear(); /* events of the state that was replaced */
	push(time, SNESSPU_EV_ALL_OFF, 0, 0);
}

/* SNES pitch 0x1000 = 32 kHz, SPU2 0x1000 = 48 kHz. */
static inline int spu_pitch(int pitch)
{
	return (pitch * 2 + 1) / 3;
}

/* VOL * envelope (+-127 * 0x7FF) to an SPU2 voice volume (+-0x3FFF). */
static inline int spu_volume(int vol_env)
{
	return (vol_env >> 4) & 0x7FFF;
}

static void send_voice(unsigned time, int voice, int vol_l, int vol_r, int pitch)
{
	int l = spu_volume(vol_l), r = spu_volume(vol_r), p = spu_pitch(pitch);
	if (p != sent[voice].pitch)
		push(time, SNESSPU_EV_PITCH, voice, sent[voice].pitch = p);
	if (l != sent[voice].vol_l)
		push(time, SNESSPU_EV_VOLL, voice, sent[voice].vol_l = l);
	if (r != sent[voice].vol_r)
		push(time, SNESSPU_EV_VOLR, voice, sent[voice].vol_r = r);
}

void spu_core_key_on(unsigned time, int voice, unsigned start, unsigned loop)
{
	spu_core_stats.key_ons++;
	int block = sample_block((uint16_t)start, (uint16_t)loop);
	if (block < 0)
	{
		/* Not a sample: the real thing would play noise from whatever
		 * is there. Silence instead. */
		spu_core_stats.bad_samples++;
		push(time, SNESSPU_EV_KOFF, voice, 0);
		return;
	}
	push(time, SNESSPU_EV_KON, voice, block);
}

void spu_core_key_off(unsigned time, int voice)
{
	push(time, SNESSPU_EV_KOFF, voice, 0);
}

void spu_core_voice(unsigned time, int voice, int vol_l, int vol_r, int pitch)
{
	send_voice(time, voice, vol_l, vol_r, pitch);
}

void spu_core_global(unsigned time, int mvol_l, int mvol_r, int flg, int non, int pmon)
{
	/* FLG: bit 6 mutes the output, bit 7 is reset (voices are silenced
	 * by the core), bits 0-4 the noise rate. MVOL -128..127 to the
	 * SPU2's +-0x3FFF. */
	bool mute = (flg & 0x40) != 0;
	int l = mute ? 0 : (mvol_l * 0x80) & 0x7FFF;
	int r = mute ? 0 : (mvol_r * 0x80) & 0x7FFF;
	int noise = noise_clock[flg & 0x1F];
	if (l != sent_mvol_l)
		push(time, SNESSPU_EV_MVOLL, 0, sent_mvol_l = l);
	if (r != sent_mvol_r)
		push(time, SNESSPU_EV_MVOLR, 0, sent_mvol_r = r);
	if (noise != sent_noise)
		push(time, SNESSPU_EV_NOISE, 0, sent_noise = noise);
	if (non != sent_non)
		push(time, SNESSPU_EV_NON, 0, sent_non = non);
	if (pmon != sent_pmon)
		push(time, SNESSPU_EV_PMON, 0, sent_pmon = pmon);
}

void spu_core_flush(unsigned now)
{
	size_t off = 0;
	do
	{
		size_t n = queue.size() - off;
		if (n > SNESSPU_BATCH_MAX)
			n = SNESSPU_BATCH_MAX;
		backend->events(n ? &queue[off] : NULL, (unsigned)n, now);
		off += n;
	} while (off < queue.size());
	queue.clear();
}
