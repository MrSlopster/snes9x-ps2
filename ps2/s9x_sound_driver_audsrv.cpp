/* audsrv sound driver: pushes interleaved int16 stereo samples into
 * audsrv's SPU2 ring buffer on the IOP.
 *
 * Sample data goes out as an asynchronous RPC (SIF_RPC_M_NOWAIT) from a
 * double buffer, so the EE keeps emulating while the IOP copies it. The
 * stock path (audsrv_wait_audio + audsrv_play_audio) is two synchronous
 * RPCs per push with the EE idle during both: ~2-3 ms per frame.
 *
 * Pacing: the EE keeps an estimate of how full the IOP ring is (bytes
 * sent minus what 48 kHz playback has drained since). Only when that
 * estimate says the ring may be full - i.e. emulation is running at full
 * speed - does it block in audsrv_wait_audio, which is the sound-sync
 * throttle. When emulation runs slower than real time no RPC waits at
 * all. */

#include "s9x_sound_driver_audsrv.h"

#include <cstdio>
#include <cstring>
#include <time.h>

extern "C" {
#include <kernel.h>
#include <sifrpc.h>
#include <audsrv.h>
}

/* audsrv's RPC command for queueing samples (ee/rpc/audsrv/src/audsrv_rpc.h,
 * not installed as a public header). Packet: int byte_count, then data. */
#define AUDSRV_CMD_PLAY_AUDIO 0x0004

/* Largest push: the server's receive buffer is 18000 bytes. */
#define MAX_PUSH_BYTES 8192

static audsrv_fmt_t fmt = { 48000, 16, 2 };

static SifRpcClientData_t play_cd __attribute__((aligned(64)));
static unsigned char send_buf[2][MAX_PUSH_BYTES + 64] __attribute__((aligned(64)));
static int recv_buf[16] __attribute__((aligned(64)));
static int send_idx = 0;
static bool play_bound = false;

/* Ring estimate (bytes queued on the IOP). */
static int ring_bytes = 0;
static int queued_est = 0;     /* bytes */
static int bytes_per_sec = 0;  /* playback drain rate */
static struct timespec last_ts;

/* audsrv's server handles one request at a time from a single receive
 * buffer: the async push must finish before any other audsrv call. */
static void wait_push_done(void)
{
	if (play_bound)
		while (sceSifCheckStatRpc(&play_cd))
			;
}

void S9xAudsrvSoundDriver::init()
{
	audsrv_init();
}

void S9xAudsrvSoundDriver::deinit()
{
	wait_push_done();
	audsrv_stop_audio();
	audsrv_quit();
	play_bound = false;
}

bool S9xAudsrvSoundDriver::open_device(int playback_rate, int buffer_size)
{
	(void)buffer_size;
	rate = playback_rate;
	fmt.freq = playback_rate;
	fmt.bits = 16;
	fmt.channels = 2;

	printf("audsrv: initializing %d Hz 16-bit stereo... ", playback_rate);

	if (audsrv_set_format(&fmt) != AUDSRV_ERR_NOERROR)
	{
		printf("failed (error 0x%x)\n", audsrv_get_error());
		return false;
	}

	/* A second client on the same server for the async pushes. The
	 * server is known to be up: audsrv_init() bound to it. */
	memset(&play_cd, 0, sizeof(play_cd));
	while (1)
	{
		if (sceSifBindRpc(&play_cd, AUDSRV_IRX, 0) < 0)
		{
			printf("failed (async bind)\n");
			return false;
		}
		if (play_cd.server)
			break;
		nopdelay();
	}
	play_bound = true;

	/* audsrv's ring: 10 feeds of 512 frames (audsrv.c). */
	ring_bytes = 10 * ((512 * playback_rate) / 48000) * 4;
	bytes_per_sec = playback_rate * 4;
	queued_est = 0;
	clock_gettime(CLOCK_MONOTONIC, &last_ts);

	printf("OK (ring %d bytes)\n", ring_bytes);
	return true;
}

void S9xAudsrvSoundDriver::start()
{
	audsrv_set_volume(MAX_VOLUME);
}

void S9xAudsrvSoundDriver::stop()
{
	wait_push_done();
	audsrv_stop_audio();
}

bool S9xAudsrvSoundDriver::write_samples(int16_t *data, int samples)
{
	/* 'samples' counts individual int16 values (frames * 2, interleaved
	 * stereo), matching S9xSDLSoundDriver::write_samples semantics. */
	int bytes = samples * (int)sizeof(int16_t);
	if (bytes > MAX_PUSH_BYTES)
		bytes = MAX_PUSH_BYTES;

	/* Drain the estimate by what playback consumed since last time. */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long elapsed_us = (long long)(ts.tv_sec - last_ts.tv_sec) * 1000000 +
			       (ts.tv_nsec - last_ts.tv_nsec) / 1000;
	last_ts = ts;
	long long drained = elapsed_us * bytes_per_sec / 1000000;
	queued_est = drained >= queued_est ? 0 : queued_est - (int)drained; /* 0: ring ran dry */

	/* Possibly full: emulation is at full speed. Block for real (this is
	 * the sound-sync throttle) and resync the estimate. */
	if (queued_est + bytes > ring_bytes - 2048)
	{
		wait_push_done();
		audsrv_wait_audio(bytes);
		int q = audsrv_queued();
		queued_est = q >= 0 ? q : 0;
		clock_gettime(CLOCK_MONOTONIC, &last_ts);
	}

	wait_push_done();

	unsigned char *buf = send_buf[send_idx];
	send_idx ^= 1;
	*(int *)buf = bytes;
	memcpy(buf + sizeof(int), data, bytes);

	if (sceSifCallRpc(&play_cd, AUDSRV_CMD_PLAY_AUDIO, SIF_RPC_M_NOWAIT, buf, bytes + sizeof(int),
			  recv_buf, sizeof(int), NULL, NULL) < 0)
		return false;

	queued_est += bytes;
	return true;
}

int S9xAudsrvSoundDriver::space_free()
{
	wait_push_done();
	return audsrv_available();
}

std::pair<int, int> S9xAudsrvSoundDriver::buffer_level()
{
	wait_push_done();
	return { audsrv_queued(), ring_bytes };
}
