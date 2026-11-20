/* Fast sound transport: EE side of snesspu.irx (see ps2_spu.h). */

#include <stdio.h>
#include <string.h>

extern "C" {
#include <kernel.h>
#include <sifrpc.h>
}

#include "snes9x.h"
#include "apu.h"

#include "ps2_spu.h"
#include "ps2_spu_core.h"
#include "ps2_iop.h"
#include "ps2_prof.h"
#include "ps2_net.h"

static SifRpcClientData_t client __attribute__((aligned(64)));
static unsigned char upload_buf[sizeof(snesspu_upload) + SNESSPU_UPLOAD_MAX + 64] __attribute__((aligned(64)));
static unsigned char event_buf[sizeof(snesspu_batch) + SNESSPU_BATCH_MAX * sizeof(snesspu_event) + 64]
	__attribute__((aligned(64)));
static unsigned int result[16] __attribute__((aligned(64)));

static bool bound = false;
static bool core_ready = false;
static bool enabled = false;
static char status[64] = "not tried";

const char *ps2_spu_status(void)
{
	return status;
}

bool ps2_spu_available(void)
{
	return ps2_iop_spu_ok();
}

bool ps2_spu_enabled(void)
{
	return enabled;
}

/* The event batches go out without waiting for the IOP; anything else
 * has to let the one in flight finish first (one call per client). */
static void wait_idle(void)
{
	while (sceSifCheckStatRpc(&client))
		;
}

static bool bind(void)
{
	if (bound)
		return true;
	if (!ps2_iop_spu_ok())
		return false;
	/* The server registers from its own thread: give it a moment. */
	for (int tries = 0; tries < 200; tries++)
	{
		if (sceSifBindRpc(&client, SNESSPU_RPC_ID, 0) >= 0 && client.server)
		{
			bound = true;
			return true;
		}
		for (volatile int i = 0; i < 100000; i++)
			;
	}
	printf("snesspu: RPC bind failed\n");
	return false;
}

static void call(int cmd, void *send, int size)
{
	wait_idle();
	sceSifCallRpc(&client, cmd, 0, send, size, result, sizeof(result), NULL, NULL);
}

static void backend_upload(uint32_t addr, const uint8_t *data, uint32_t size)
{
	PROF_SCOPE(PROF_IOP);
	snesspu_upload *u = (snesspu_upload *)upload_buf;
	u->addr = addr;
	u->size = size;
	memcpy(upload_buf + sizeof(*u), data, size);
	call(SNESSPU_CMD_UPLOAD, upload_buf, sizeof(*u) + size);
	if (result[0])
		printf("snesspu: upload %x+%x failed (%d)\n", (unsigned)addr, (unsigned)size, (int)result[0]);
}

static void backend_events(const snesspu_event *ev, unsigned count, uint32_t now)
{
	PROF_SCOPE(PROF_IOP);
	wait_idle(); /* the buffer is still the previous batch until then */
	snesspu_batch *b = (snesspu_batch *)event_buf;
	b->count = count;
	b->now = now;
	if (count)
		memcpy(event_buf + sizeof(*b), ev, count * sizeof(snesspu_event));
	sceSifCallRpc(&client, SNESSPU_CMD_EVENTS, SIF_RPC_M_NOWAIT, event_buf, sizeof(*b) + count * sizeof(snesspu_event),
		      NULL, 0, NULL, NULL);
}

static const SpuBackend backend = { backend_upload, backend_events };
static const S9xFastSoundSink sink = { spu_core_reset, spu_core_key_on, spu_core_key_off, spu_core_voice,
				       spu_core_global };

void ps2_spu_enable(bool on)
{
	if (on)
	{
		if (!bind())
		{
			snprintf(status, sizeof(status), ps2_iop_spu_ok() ? "RPC bind failed" : "module not loaded");
			return;
		}
		if (!enabled)
		{
			result[0] = 0;
			call(SNESSPU_CMD_INIT, NULL, 0);
			printf("snesspu: version %u\n", result[0]);
			snprintf(status, sizeof(status), "init ok, version %u", result[0]);
		}
		if (!core_ready)
		{
			spu_core_init(S9xAPUGetRAM(), &backend);
			core_ready = true;
		}
		/* (Re)attaching makes the DSP report its whole state again. */
		S9xAPUSetFastSound(&sink);
		enabled = true;
	}
	else if (enabled)
	{
		S9xAPUSetFastSound(NULL);
		call(SNESSPU_CMD_STOP, NULL, 0);
		enabled = false;
	}
}

/* Once a second on the network log: what the IOP side and the chip say. */
static void log_status(unsigned now)
{
	/* SIF transfers whole 16-byte units into the receive buffer: give it
	 * room, or the tail lands on whatever the linker put next. */
	static union
	{
		snesspu_status st;
		unsigned char pad[128];
	} reply __attribute__((aligned(64)));
	snesspu_status &st = reply.st;
	wait_idle();
	memset(&reply, 0, sizeof(reply));
	sceSifCallRpc(&client, SNESSPU_CMD_STATUS, 0, NULL, 0, &reply, sizeof(st), NULL, NULL);

	/* Two short lines: PCSX2's log drops long ones. */
	char line[200];
	snprintf(line, sizeof(line), "SPU now %u play %u newest %u | ev sent %u in %u applied %u dropped %u | kon %u/%u", now,
		 st.play_pos, st.newest, spu_core_stats.events, st.events_in, st.events_applied, st.events_dropped,
		 st.key_ons, spu_core_stats.key_ons);
	printf("%s\n", line);
	ps2_net_log(line);
	snprintf(line, sizeof(line),
		 "SPU2 samples %u/%ub bad %u | upload ok %u fail %u err %d readback %u | mvol %08x attr %04x endx %06x nax %x %x "
		 "envx %x %x %x %x %x %x %x %x",
		 spu_core_stats.samples, spu_core_stats.sample_bytes, spu_core_stats.bad_samples, st.uploads_ok,
		 st.uploads_failed, st.last_upload_error, st.readback, st.mvol, st.core_attr, st.endx, st.nax[0], st.nax[1],
		 st.envx[0], st.envx[1], st.envx[2], st.envx[3], st.envx[4], st.envx[5], st.envx[6], st.envx[7]);
	printf("%s\n", line);
	ps2_net_log(line);
}

void ps2_spu_frame(void)
{
	if (!enabled)
		return;
	unsigned now;
	{
		PROF_SCOPE(PROF_APU);
		now = S9xAPUFastSoundSync();
	}
	spu_core_flush(now);

	static unsigned frames = 0;
	if (++frames % 60 == 0 && ps2_prof_enabled)
		log_status(now);
}

void ps2_spu_silence(void)
{
	if (enabled)
		call(SNESSPU_CMD_STOP, NULL, 0);
}
