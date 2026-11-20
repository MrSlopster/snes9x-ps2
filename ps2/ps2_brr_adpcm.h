#pragma once
/* SNES BRR -> PS2 SPU2 ADPCM transcoding for the SPU2 sound path.
 *
 * The SPU2 is a descendant of the SNES S-DSP: 4-bit ADPCM with the same
 * predictor filters (BRR filters 1/2/3 == SPU ADPCM filters 1/4/2), but
 * 28 samples per 16-byte block instead of 16 per 9 bytes, so samples are
 * decoded and re-encoded. Portable C++ (host-testable, see
 * tests/test_brr_adpcm.cpp). */

#include <stdint.h>
#include <vector>

struct SpuSample
{
	std::vector<uint8_t> adpcm; /* 16-byte SPU ADPCM blocks, flags set */
	bool looped;
	uint32_t loop_start_block;  /* block index carrying the loop-start flag */
	uint32_t pcm_samples;       /* samples before any loop repeat (info) */
	uint32_t brr_bytes;         /* BRR bytes the sample was built from */
	uint32_t brr_hash;          /* FNV-1a of those bytes (cache key) */
};

/* Decode a BRR sample from 64 KB of APU RAM. 'start' / 'loop' are the two
 * directory words. Returns the 16-bit PCM: 'pcm' holds the one-shot part
 * (start to the end block; without the end block itself when the sample
 * does not loop, as the DSP never plays that), 'loop_pcm' the loop body
 * as played on the second pass (empty when the sample does not loop). False if the data is
 * not a terminated sample (runs past max_blocks). */
bool brr_decode(const uint8_t *apuram, uint16_t start, uint16_t loop, std::vector<int16_t> &pcm,
		std::vector<int16_t> &loop_pcm, uint32_t *brr_bytes, uint32_t *brr_hash, unsigned max_blocks = 8192);

/* Encode PCM as SPU ADPCM. With a non-empty loop body, the loop start is
 * moved to a 28-sample block boundary (rotating into the loop), the body
 * repeated until its length is a multiple of 28, and encoded so that the
 * SPU's carried-over predictor state is right on every pass. */
void spu_adpcm_encode(const std::vector<int16_t> &pcm, const std::vector<int16_t> &loop_pcm, SpuSample &out);

/* Reference decoder (tests): decode 'blocks' 16-byte blocks. */
void spu_adpcm_decode(const uint8_t *adpcm, unsigned blocks, std::vector<int16_t> &pcm);

/* Convenience: decode + encode. */
bool brr_to_spu_adpcm(const uint8_t *apuram, uint16_t start, uint16_t loop, SpuSample &out);
