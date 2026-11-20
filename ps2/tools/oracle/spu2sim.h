#pragma once
/* Software SPU2 for the host oracle (see spu2sim.cpp). The two transport
 * functions have the signatures of SpuBackend (ps2/ps2_spu_core.h). */
#include <stdint.h>
#include "snesspu/snesspu.h"

bool spu2sim_open(const char *wav_path); /* 48 kHz, 16-bit stereo */
void spu2sim_upload(uint32_t addr, const uint8_t *data, uint32_t size);
void spu2sim_events(const snesspu_event *ev, unsigned count, uint32_t now);
void spu2sim_close(void);
