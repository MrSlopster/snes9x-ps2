/* SNES BRR -> SPU2 ADPCM (see ps2_brr_adpcm.h). */

#include <stdlib.h>
#include <string.h>

#include "ps2_brr_adpcm.h"

#define SPU_BLOCK_SAMPLES 28
#define SPU_BLOCK_BYTES 16

/* SPU ADPCM block flags (byte 1). */
#define SPU_FLAG_END 0x01    /* last block: stop, or jump to the loop start */
#define SPU_FLAG_REPEAT 0x02 /* with END: loop instead of stopping */
#define SPU_FLAG_START 0x04  /* loop start */

/* Predictor coefficients, /64. BRR filters 1, 2, 3 are entries 1, 4, 2. */
static const int spu_f0[5] = { 0, 60, 115, 98, 122 };
static const int spu_f1[5] = { 0, 0, -52, -55, -60 };

static inline int clamp16(int s)
{
	return s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
}

/* ---- BRR decode: same arithmetic as SPC_DSP::decode_brr ------------- */

/* Decode one 9-byte block; p1/p2 are the two previous outputs. */
static void brr_decode_block(const uint8_t *apuram, uint16_t addr, int &p1, int &p2, std::vector<int16_t> &out)
{
	int header = apuram[addr];
	int shift = header >> 4;
	int filter = header & 0x0C;

	for (int i = 0; i < 16; i++)
	{
		int byte = apuram[(uint16_t)(addr + 1 + (i >> 1))];
		int nyb = (i & 1) ? (byte & 0x0F) : (byte >> 4);
		int s = (nyb ^ 8) - 8; /* sign-extend 4 bits */

		s = (s << shift) >> 1;
		if (shift >= 0xD)
			s = (s >> 25) << 11; /* invalid shift: -2048 or 0 */

		int q1 = p1, q2 = p2 >> 1;
		if (filter >= 8)
		{
			s += q1;
			s -= q2;
			if (filter == 8)
			{
				s += q2 >> 4;
				s += (q1 * -3) >> 6;
			}
			else
			{
				s += (q1 * -13) >> 7;
				s += (q2 * 3) >> 4;
			}
		}
		else if (filter)
		{
			s += q1 >> 1;
			s += (-q1) >> 5;
		}

		s = clamp16(s);
		s = (int16_t)(s * 2);
		out.push_back((int16_t)s);
		p2 = p1;
		p1 = s;
	}
}

static uint32_t fnv1a(uint32_t h, const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* Decode blocks from 'addr' through the first END block. Returns the
 * end block's header, or -1 if no END within max_blocks. */
static int brr_decode_run(const uint8_t *apuram, uint16_t addr, int &p1, int &p2, std::vector<int16_t> &out,
			  unsigned max_blocks, uint32_t *bytes, uint32_t *hash)
{
	for (unsigned n = 0; n < max_blocks; n++)
	{
		int header = apuram[addr];
		brr_decode_block(apuram, addr, p1, p2, out);
		if (bytes)
			*bytes += 9;
		if (hash)
		{
			uint8_t blk[9];
			for (int i = 0; i < 9; i++)
				blk[i] = apuram[(uint16_t)(addr + i)];
			*hash = fnv1a(*hash, blk, 9);
		}
		if (header & 1)
			return header;
		addr = (uint16_t)(addr + 9);
	}
	return -1;
}

bool brr_decode(const uint8_t *apuram, uint16_t start, uint16_t loop, std::vector<int16_t> &pcm,
		std::vector<int16_t> &loop_pcm, uint32_t *brr_bytes, uint32_t *brr_hash, unsigned max_blocks)
{
	pcm.clear();
	loop_pcm.clear();
	uint32_t bytes = 0, hash = 2166136261u;
	int p1 = 0, p2 = 0;

	int end_header = brr_decode_run(apuram, start, p1, p2, pcm, max_blocks, &bytes, &hash);
	if (end_header < 0)
		return false;

	if (!(end_header & 2))
	{
		/* One-shot. The DSP silences the voice as soon as it reaches a
		 * block marked "end, no loop", so that block is never heard. */
		pcm.resize(pcm.size() - 16);
	}
	else
	{
		/* Looping: the predictor state carries across the loop point, so
		 * the body as heard from the second pass on is decoded with the
		 * state the first pass ended in. */
		if (brr_decode_run(apuram, loop, p1, p2, loop_pcm, max_blocks, &bytes, &hash) < 0)
			return false;
	}

	/* The directory words are part of the identity of a sample. */
	uint8_t dir[4] = { (uint8_t)start, (uint8_t)(start >> 8), (uint8_t)loop, (uint8_t)(loop >> 8) };
	hash = fnv1a(hash, dir, 4);

	if (brr_bytes)
		*brr_bytes = bytes;
	if (brr_hash)
		*brr_hash = hash;
	return true;
}

/* ---- SPU ADPCM ----------------------------------------------------- */

void spu_adpcm_decode(const uint8_t *adpcm, unsigned blocks, std::vector<int16_t> &pcm)
{
	int s1 = 0, s2 = 0;
	for (unsigned b = 0; b < blocks; b++)
	{
		const uint8_t *blk = adpcm + b * SPU_BLOCK_BYTES;
		int shift = blk[0] & 0x0F, filter = blk[0] >> 4;
		if (filter > 4)
			filter = 0;
		for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
		{
			int byte = blk[2 + (i >> 1)];
			int nyb = (i & 1) ? (byte >> 4) : (byte & 0x0F);
			int s = (int16_t)(nyb << 12) >> shift;
			s += (s1 * spu_f0[filter] + s2 * spu_f1[filter] + 32) >> 6;
			s = clamp16(s);
			pcm.push_back((int16_t)s);
			s2 = s1;
			s1 = s;
		}
	}
}

/* Shift for a block from the largest prediction residual (predicting
 * from the true samples): the 4-bit code covers +-8 << (12 - shift). */
static int guess_shift(const int16_t *in, int filter, int s1, int s2)
{
	int max_res = 0;
	for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
	{
		int pred = (s1 * spu_f0[filter] + s2 * spu_f1[filter] + 32) >> 6;
		int res = abs(in[i] - pred);
		if (res > max_res)
			max_res = res;
		s2 = s1;
		s1 = in[i];
	}
	int shift = 12;
	while (shift > 0 && (max_res >> (12 - shift)) > 7)
		shift--;
	return shift;
}

/* Encode one block with a given filter and shift, decoder in the loop.
 * Returns the squared error; writes the block and the decoder state
 * after it. */
static uint64_t encode_block(const int16_t *in, int filter, int shift, int s1, int s2, uint8_t *blk, int *out_s1,
			     int *out_s2)
{
	uint64_t err = 0;
	int step = 1 << (12 - shift);
	memset(blk, 0, SPU_BLOCK_BYTES);
	blk[0] = (uint8_t)((filter << 4) | shift);
	for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
	{
		int pred = (s1 * spu_f0[filter] + s2 * spu_f1[filter] + 32) >> 6;
		int res = in[i] - pred;
		int mag = (abs(res) + step / 2) >> (12 - shift); /* round to nearest */
		int code = res >= 0 ? mag : -mag;
		if (code > 7)
			code = 7;
		if (code < -8)
			code = -8;
		int s = clamp16(pred + code * step);
		int d = in[i] - s;
		err += (uint64_t)((int64_t)d * d);
		blk[2 + (i >> 1)] |= (uint8_t)((code & 0x0F) << ((i & 1) ? 4 : 0));
		s2 = s1;
		s1 = s;
	}
	*out_s1 = s1;
	*out_s2 = s2;
	return err;
}

/* Encode whole blocks of pcm[first .. first + count*28), appending to
 * 'adpcm' (flags left 0). s1/s2 are the decoder state on entry and are
 * updated to the state on exit. With pcm_first_block, the first block
 * uses filter 0 (no prediction), which makes everything from there on
 * independent of the entry state. */
static void encode_blocks(const int16_t *pcm, size_t count, int &s1, int &s2, std::vector<uint8_t> &adpcm,
			  bool pcm_first_block = false)
{
	for (size_t b = 0; b < count; b++)
	{
		const int16_t *in = pcm + b * SPU_BLOCK_SAMPLES;
		uint8_t best[SPU_BLOCK_BYTES] = { 0 }, trial[SPU_BLOCK_BYTES];
		uint64_t best_err = ~0ull;
		int best_s1 = 0, best_s2 = 0;
		int filters = (pcm_first_block && b == 0) ? 1 : 5;
		for (int f = 0; f < filters && best_err; f++)
		{
			/* The open-loop guess is usually right or one too fine. */
			int guess = guess_shift(in, f, s1, s2);
			for (int shift = guess; shift >= 0 && shift >= guess - 1; shift--)
			{
				int o1, o2;
				uint64_t e = encode_block(in, f, shift, s1, s2, trial, &o1, &o2);
				if (e < best_err)
				{
					best_err = e;
					best_s1 = o1;
					best_s2 = o2;
					memcpy(best, trial, sizeof(best));
				}
			}
		}
		adpcm.insert(adpcm.end(), best, best + SPU_BLOCK_BYTES);
		s1 = best_s1;
		s2 = best_s2;
	}
}

/* Extra copies of a loop body are only stored while the copies together
 * stay under this many blocks (16 bytes each). */
#define LOOP_COPY_BUDGET 96
#define LOOP_MAX_COPIES 12

void spu_adpcm_encode(const std::vector<int16_t> &pcm_in, const std::vector<int16_t> &loop_in, SpuSample &out)
{
	std::vector<int16_t> lead = pcm_in;
	out.looped = !loop_in.empty();
	out.pcm_samples = (uint32_t)pcm_in.size();
	out.loop_start_block = 0;
	out.adpcm.clear();

	int s1 = 0, s2 = 0;
	if (!out.looped)
	{
		while (lead.empty() || lead.size() % SPU_BLOCK_SAMPLES)
			lead.push_back(0);
		encode_blocks(lead.data(), lead.size() / SPU_BLOCK_SAMPLES, s1, s2, out.adpcm);
		out.adpcm[out.adpcm.size() - SPU_BLOCK_BYTES + 1] = SPU_FLAG_END;
		return;
	}

	/* Loop start on a block boundary: play 'rot' samples of the loop in
	 * the lead-in, and start the stored loop body that far in. */
	size_t L = loop_in.size();
	size_t rot = (SPU_BLOCK_SAMPLES - lead.size() % SPU_BLOCK_SAMPLES) % SPU_BLOCK_SAMPLES;
	for (size_t i = 0; i < rot; i++)
		lead.push_back(loop_in[i % L]);

	/* Whole blocks: repeat the body until its length divides by 28. */
	size_t reps = 1;
	while ((L * reps) % SPU_BLOCK_SAMPLES)
		reps++;
	std::vector<int16_t> body(L * reps);
	for (size_t i = 0; i < body.size(); i++)
		body[i] = loop_in[(rot + i) % L];
	size_t body_blocks = body.size() / SPU_BLOCK_SAMPLES;

	encode_blocks(lead.data(), lead.size() / SPU_BLOCK_SAMPLES, s1, s2, out.adpcm);

	/* The SPU carries its predictor state across the loop jump, and a
	 * body encoded for one entry state rings audibly through the
	 * resonant filters when entered with another. So follow what the
	 * hardware will do: encode the body for the state the lead-in
	 * leaves, again for the state that copy ends in, and so on until a
	 * state comes round again. The copies before that point extend the
	 * lead-in; the ones after it are the loop, and every pass is then
	 * decoded exactly as it was encoded. */
	struct Copy
	{
		int s1, s2; /* entry state */
		std::vector<uint8_t> data;
	};
	std::vector<Copy> copies;
	size_t max_copies = LOOP_COPY_BUDGET / body_blocks;
	if (max_copies > LOOP_MAX_COPIES)
		max_copies = LOOP_MAX_COPIES;
	if (max_copies < 1)
		max_copies = 1;

	int cycle_from = -1;
	for (;;)
	{
		for (size_t j = 0; j < copies.size(); j++)
			if (copies[j].s1 == s1 && copies[j].s2 == s2)
				cycle_from = (int)j;
		if (cycle_from >= 0 || copies.size() >= max_copies)
			break;
		Copy c;
		c.s1 = s1;
		c.s2 = s2;
		encode_blocks(body.data(), body_blocks, s1, s2, c.data);
		copies.push_back(c);
	}

	if (cycle_from < 0)
	{
		/* No luck within the budget (long bodies get one try): start
		 * the loop with an unpredicted block instead, which forgets the
		 * entry state. One coarser block in a long loop. */
		copies.clear();
		Copy c;
		c.s1 = s1;
		c.s2 = s2;
		encode_blocks(body.data(), body_blocks, s1, s2, c.data, true);
		copies.push_back(c);
		cycle_from = 0;
	}

	for (size_t j = 0; j < copies.size(); j++)
	{
		if ((int)j == cycle_from)
			out.loop_start_block = (uint32_t)(out.adpcm.size() / SPU_BLOCK_BYTES);
		out.adpcm.insert(out.adpcm.end(), copies[j].data.begin(), copies[j].data.end());
	}

	size_t blocks = out.adpcm.size() / SPU_BLOCK_BYTES;
	for (size_t b = out.loop_start_block; b < blocks; b++)
		out.adpcm[b * SPU_BLOCK_BYTES + 1] = SPU_FLAG_REPEAT;
	out.adpcm[out.loop_start_block * SPU_BLOCK_BYTES + 1] |= SPU_FLAG_START;
	out.adpcm[(blocks - 1) * SPU_BLOCK_BYTES + 1] |= SPU_FLAG_END;
}

bool brr_to_spu_adpcm(const uint8_t *apuram, uint16_t start, uint16_t loop, SpuSample &out)
{
	std::vector<int16_t> pcm, loop_pcm;
	if (!brr_decode(apuram, start, loop, pcm, loop_pcm, &out.brr_bytes, &out.brr_hash))
		return false;
	spu_adpcm_encode(pcm, loop_pcm, out);
	return true;
}
