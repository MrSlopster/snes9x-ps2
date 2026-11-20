/* Host test: SNES BRR -> SPU2 ADPCM transcoding (ps2_brr_adpcm.cpp).
 *
 * 1. Synthetic samples (one-shot and looped, every loop length mod 28):
 *    block flags are right and the SPU-side playback - decoded with the
 *    SPU's own arithmetic, following its loop flags - tracks the BRR
 *    playback.
 * 2. Optional: APU state dumps from the host oracle (--dump-apu) given as
 *    arguments. Checks our BRR decoder against the samples the real DSP
 *    core had decoded (voice BRR buffers in the dump), and reports the
 *    transcoding error for every sample in the directory. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include "../ps2_brr_adpcm.h"

static int failures = 0;
#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			if (failures < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } \
			failures++; \
		} \
	} while (0)

/* ---- a small BRR encoder, to make test input ------------------------ */

static int clamp16(int s)
{
	return s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
}

static int brr_step(int nyb, int shift, int filter, int p1, int p2)
{
	int s = (nyb << shift) >> 1;
	int q2 = p2 >> 1;
	if (filter == 1)
		s += (p1 >> 1) + ((-p1) >> 5);
	else if (filter == 2)
		s += p1 - q2 + (q2 >> 4) + ((p1 * -3) >> 6);
	else if (filter == 3)
		s += p1 - q2 + ((p1 * -13) >> 7) + ((q2 * 3) >> 4);
	return (int16_t)(clamp16(s) * 2);
}

/* Encode 16 samples at ram[addr..addr+8]; p1/p2 carried. */
static void brr_encode_block(uint8_t *blk, const int16_t *in, int &p1, int &p2, int flags, bool first)
{
	double best = 1e300;
	uint8_t best_blk[9] = { 0 };
	int best_p1 = 0, best_p2 = 0;
	for (int filter = 0; filter < (first ? 1 : 4); filter++)
		for (int shift = 0; shift <= 12; shift++)
		{
			uint8_t t[9] = { 0 };
			int a = p1, b = p2;
			double err = 0;
			for (int i = 0; i < 16; i++)
			{
				int bn = 0;
				double be = 1e300;
				for (int n = -8; n < 8; n++)
				{
					double d = (double)brr_step(n, shift, filter, a, b) - in[i];
					if (d * d < be)
						be = d * d, bn = n;
				}
				err += be;
				int s = brr_step(bn, shift, filter, a, b);
				b = a;
				a = s;
				t[1 + (i >> 1)] |= (uint8_t)((bn & 15) << ((i & 1) ? 0 : 4));
			}
			if (err < best)
			{
				best = err;
				t[0] = (uint8_t)((shift << 4) | (filter << 2) | flags);
				memcpy(best_blk, t, 9);
				best_p1 = a;
				best_p2 = b;
			}
		}
	memcpy(blk, best_blk, 9);
	p1 = best_p1;
	p2 = best_p2;
}

/* ---- playback models ------------------------------------------------ */

/* BRR playback of n samples: the one-shot part, then the loop body. */
static std::vector<int16_t> play_brr(const std::vector<int16_t> &pcm, const std::vector<int16_t> &loop, size_t n)
{
	std::vector<int16_t> out;
	for (size_t i = 0; i < n; i++)
	{
		if (i < pcm.size())
			out.push_back(pcm[i]);
		else if (!loop.empty())
			out.push_back(loop[(i - pcm.size()) % loop.size()]);
		else
			out.push_back(0);
	}
	return out;
}

/* SPU playback of n samples: decode block by block following the flags,
 * predictor state carried across the loop jump (as the hardware does). */
static std::vector<int16_t> play_spu(const SpuSample &s, size_t n, bool *flags_ok)
{
	static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
	std::vector<int16_t> out;
	size_t blocks = s.adpcm.size() / 16, b = 0, loop_addr = 0;
	int s1 = 0, s2 = 0;
	bool stopped = false;
	*flags_ok = true;
	while (out.size() < n)
	{
		if (stopped || b >= blocks)
		{
			if (!stopped)
				*flags_ok = false; /* ran off the end of the sample */
			stopped = true;
			out.push_back(0);
			continue;
		}
		const uint8_t *blk = &s.adpcm[b * 16];
		int shift = blk[0] & 15, filter = blk[0] >> 4, flags = blk[1];
		if (filter > 4 || shift > 12)
			*flags_ok = false;
		if (flags & 4)
			loop_addr = b;
		for (int i = 0; i < 28; i++)
		{
			int nyb = (blk[2 + (i >> 1)] >> ((i & 1) ? 4 : 0)) & 15;
			int v = (int16_t)(nyb << 12) >> shift;
			v += (s1 * f0[filter] + s2 * f1[filter] + 32) >> 6;
			v = clamp16(v);
			out.push_back((int16_t)v);
			s2 = s1;
			s1 = v;
		}
		if (flags & 1)
		{
			if (flags & 2)
				b = loop_addr;
			else
				stopped = true;
		}
		else
			b++;
	}
	out.resize(n);
	return out;
}

static double snr_db(const std::vector<int16_t> &ref, const std::vector<int16_t> &got)
{
	double sig = 0, noise = 0;
	for (size_t i = 0; i < ref.size(); i++)
	{
		double d = (double)ref[i] - got[i];
		sig += (double)ref[i] * ref[i];
		noise += d * d;
	}
	if (noise == 0)
		return 999;
	if (sig == 0)
		return -999;
	return 10 * log10(sig / noise);
}

/* ---- 1. synthetic --------------------------------------------------- */

static void synthetic(void)
{
	unsigned cases = 0;
	double worst = 999;
	for (int looped = 0; looped < 2; looped++)
		for (int head_blocks = 0; head_blocks <= 3; head_blocks++)
			for (int loop_blocks = 1; loop_blocks <= 9; loop_blocks++)
			{
				if (!looped && loop_blocks > 2)
					continue;
				int total = head_blocks + loop_blocks;
				/* A tone that fits the loop exactly, with a decaying
				 * attack transient in the head. */
				std::vector<int16_t> src(total * 16);
				for (int i = 0; i < total * 16; i++)
				{
					double ph = 2 * M_PI * (i - head_blocks * 16) / (loop_blocks * 16.0);
					double v = 9000 * sin(ph * loop_blocks) + 3000 * sin(ph * 3 * loop_blocks + 1);
					if (i < head_blocks * 16)
						v += 6000 * sin(i * 1.3) * exp(-i / 12.0);
					src[i] = (int16_t)v;
				}

				static uint8_t ram[65536];
				memset(ram, 0, sizeof(ram));
				uint16_t start = 0x2000, loop = (uint16_t)(start + head_blocks * 9);
				int p1 = 0, p2 = 0;
				for (int b = 0; b < total; b++)
				{
					int flags = (b == total - 1) ? (looped ? 3 : 1) : 0;
					brr_encode_block(&ram[start + b * 9], &src[b * 16], p1, p2, flags, b == 0);
				}

				std::vector<int16_t> pcm, loop_pcm;
				uint32_t bytes, hash;
				bool ok = brr_decode(ram, start, loop, pcm, loop_pcm, &bytes, &hash);
				CHECK(ok, "decode looped=%d head=%d loop=%d", looped, head_blocks, loop_blocks);
				/* (the end block of a one-shot sample is not played) */
				CHECK(pcm.size() == (size_t)(total - (looped ? 0 : 1)) * 16, "pcm size %zu", pcm.size());
				CHECK(loop_pcm.size() == (looped ? (size_t)loop_blocks * 16 : 0), "loop size %zu", loop_pcm.size());

				SpuSample s;
				spu_adpcm_encode(pcm, loop_pcm, s);
				size_t blocks = s.adpcm.size() / 16;
				CHECK(s.adpcm.size() % 16 == 0 && blocks > 0, "adpcm size");
				CHECK(s.looped == (looped != 0), "looped flag");

				/* Flags: one END, on the last block; one START, on the
				 * loop block; REPEAT with END iff looped. */
				int ends = 0, starts = 0;
				for (size_t b = 0; b < blocks; b++)
				{
					int f = s.adpcm[b * 16 + 1];
					if (f & 1)
					{
						ends++;
						CHECK(b == blocks - 1, "END on block %zu of %zu", b, blocks);
					}
					if (f & 4)
					{
						starts++;
						CHECK(b == s.loop_start_block, "START on block %zu", b);
					}
				}
				CHECK(ends == 1, "%d END flags", ends);
				CHECK(starts == (looped ? 1 : 0), "%d START flags", starts);
				CHECK(((s.adpcm[(blocks - 1) * 16 + 1] & 2) != 0) == (looped != 0), "REPEAT flag");

				/* Play the stored sample and three more times round its loop. */
				size_t n = (blocks + 3 * (blocks - s.loop_start_block)) * 28 + 100;
				bool flags_ok;
				std::vector<int16_t> a = play_brr(pcm, loop_pcm, n), b = play_spu(s, n, &flags_ok);
				CHECK(flags_ok, "bad block header / ran off the end");
				double snr = snr_db(a, b);
				if (getenv("BRR_TEST_VERBOSE") && looped)
				{
					/* Error per stretch: lead-in, then each pass of the stored loop. */
					size_t lead = s.loop_start_block * 28, body = (blocks - s.loop_start_block) * 28;
					printf("head=%d loop=%d (lead %zu, body %zu):", head_blocks, loop_blocks, lead, body);
					for (size_t at = 0, len = lead; at + len <= n && len; at += len, len = body)
					{
						std::vector<int16_t> ra(a.begin() + at, a.begin() + at + len), rb(b.begin() + at, b.begin() + at + len);
						printf(" %.1f", snr_db(ra, rb));
					}
					printf("\n");
				}
				worst = std::min(worst, snr);
				CHECK(snr > 25, "SNR %.1f dB looped=%d head=%d loop=%d", snr, looped, head_blocks, loop_blocks);
				cases++;
			}
	printf("%u synthetic samples, worst SNR %.1f dB\n", cases, worst);
}

/* ---- 2. real APU state dumps --------------------------------------- */

#define DSP_REGS_OFFSET (0x10000 + 41 * 4) /* after the SMP's RAM + 41 int32s */
#define VOICE_STATE_SIZE 38

static void real(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		printf("%s: cannot open (skipped)\n", path);
		return;
	}
	std::vector<uint8_t> st(DSP_REGS_OFFSET + 128 + 8 * VOICE_STATE_SIZE);
	size_t got = fread(st.data(), 1, st.size(), f);
	fclose(f);
	CHECK(got == st.size(), "%s: short file", path);
	if (got != st.size())
		return;

	const uint8_t *ram = st.data(), *regs = ram + DSP_REGS_OFFSET;
	unsigned dir = regs[0x5D] * 0x100u;

	/* a. Decoder vs the DSP core: each active voice's 12-sample BRR
	 * buffer must appear in our decode of its source, at the position
	 * the voice is at. */
	int voices = 0, matched = 0;
	for (int v = 0; v < 8; v++)
	{
		const uint8_t *vs = regs + 128 + v * VOICE_STATE_SIZE;
		unsigned env = vs[28] | (vs[29] << 8);
		unsigned brr_addr = vs[26] | (vs[27] << 8), buf_pos = vs[32], brr_offset = vs[33], kon_delay = vs[34];
		if (!env || kon_delay)
			continue;
		int16_t buf[12];
		for (int i = 0; i < 12; i++)
			buf[i] = (int16_t)(vs[i * 2] | (vs[i * 2 + 1] << 8));

		unsigned srcn = regs[v * 0x10 + 4];
		unsigned e = (dir + srcn * 4) & 0xFFFF;
		uint16_t start = (uint16_t)(ram[e] | (ram[(e + 1) & 0xFFFF] << 8));
		uint16_t loop = (uint16_t)(ram[(e + 2) & 0xFFFF] | (ram[(e + 3) & 0xFFFF] << 8));
		std::vector<int16_t> pcm, loop_pcm;
		if (!brr_decode(ram, start, loop, pcm, loop_pcm, NULL, NULL))
			continue;
		voices++;

		/* Position of the voice: samples decoded so far in its block. */
		unsigned in_block = (brr_offset - 1) * 2;
		bool hit = false;
		for (int pass = 0; pass < 2 && !hit; pass++)
		{
			const std::vector<int16_t> &p = pass ? loop_pcm : pcm;
			uint16_t base = pass ? loop : start;
			unsigned delta = (uint16_t)(brr_addr - base);
			if (delta % 9)
				continue;
			size_t pos = (size_t)(delta / 9) * 16 + in_block; /* samples decoded */
			if (pos < 12 || pos > p.size())
				continue;
			hit = true;
			for (int i = 0; i < 12; i++) /* newest sample is at buf_pos - 1 */
				if (buf[(buf_pos + 12 - 1 - i) % 12] != p[pos - 1 - i])
					hit = false;
		}
		if (hit)
			matched++;
	}

	/* b. Transcode every terminated sample in the directory. */
	std::vector<double> snrs;
	unsigned adpcm_bytes = 0, brr_bytes = 0, max_blocks = 0;
	std::vector<std::pair<uint16_t, uint16_t> > seen;
	for (unsigned n = 0; n < 256; n++)
	{
		unsigned e = (dir + n * 4) & 0xFFFF;
		uint16_t start = (uint16_t)(ram[e] | (ram[(e + 1) & 0xFFFF] << 8));
		uint16_t loop = (uint16_t)(ram[(e + 2) & 0xFFFF] | (ram[(e + 3) & 0xFFFF] << 8));
		std::pair<uint16_t, uint16_t> key(start, loop);
		if (std::find(seen.begin(), seen.end(), key) != seen.end())
			continue;
		seen.push_back(key);

		std::vector<int16_t> pcm, loop_pcm;
		uint32_t bytes, hash;
		if (!brr_decode(ram, start, loop, pcm, loop_pcm, &bytes, &hash, 4096))
			continue;
		bool silent = true;
		for (size_t i = 0; i < pcm.size(); i++)
			if (pcm[i])
				silent = false;
		if (silent)
			continue;

		SpuSample s;
		spu_adpcm_encode(pcm, loop_pcm, s);
		size_t sb = s.adpcm.size() / 16;
		size_t len = (sb + (s.looped ? 3 * (sb - s.loop_start_block) : 0)) * 28;
		bool flags_ok;
		std::vector<int16_t> a = play_brr(pcm, loop_pcm, len), b = play_spu(s, len, &flags_ok);
		CHECK(flags_ok, "%s: sample %u bad flags", path, n);
		snrs.push_back(snr_db(a, b));
		adpcm_bytes += (unsigned)s.adpcm.size();
		brr_bytes += bytes;
		max_blocks = std::max(max_blocks, (unsigned)(s.adpcm.size() / 16));
	}
	std::sort(snrs.begin(), snrs.end());
	printf("%s:\n  decoder vs DSP core: %d of %d playing voices match exactly\n", path, matched, voices);
	if (!snrs.empty())
		printf("  %zu samples: SNR min %.1f / median %.1f / max %.1f dB; BRR %u bytes -> ADPCM %u bytes (largest %u blocks)\n",
		       snrs.size(), snrs.front(), snrs[snrs.size() / 2], snrs.back(), brr_bytes, adpcm_bytes, max_blocks);
	CHECK(voices == 0 || matched > 0, "%s: BRR decoder disagrees with the DSP core", path);
}

int main(int argc, char **argv)
{
	synthetic();
	for (int i = 1; i < argc; i++)
		real(argv[i]);

	if (failures)
	{
		printf("RED/FAIL (%d failures)\n", failures);
		return 1;
	}
	printf("GREEN/PASS (0 failures)\n");
	return 0;
}
