/* audsrv sound driver: pushes interleaved int16 stereo samples into
 * audsrv's SPU2 ring buffer. audsrv_wait_audio() blocks until there is
 * room, which doubles as the emulator's sound-sync throttle (no
 * busy-spinning anywhere in this driver). */

#include "s9x_sound_driver_audsrv.h"

#include <cstdio>

extern "C" {
#include <audsrv.h>
}

static audsrv_fmt_t fmt = { 48000, 16, 2 };

void S9xAudsrvSoundDriver::init()
{
	audsrv_init();
}

void S9xAudsrvSoundDriver::deinit()
{
	audsrv_stop_audio();
	audsrv_quit();
}

bool S9xAudsrvSoundDriver::open_device(int playback_rate, int buffer_size)
{
	/* buffer_size is in milliseconds (96 ms chosen by the port); audsrv
	 * manages its own internal ring, so we only record the rate. */
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

	printf("OK\n");
	return true;
}

void S9xAudsrvSoundDriver::start()
{
	audsrv_set_volume(MAX_VOLUME);
}

void S9xAudsrvSoundDriver::stop()
{
	audsrv_stop_audio();
}

bool S9xAudsrvSoundDriver::write_samples(int16_t *data, int samples)
{
	/* 'samples' counts individual int16 values (frames * 2, interleaved
	 * stereo), matching S9xSDLSoundDriver::write_samples semantics. */
	int bytes = samples * (int)sizeof(int16_t);

	audsrv_wait_audio(bytes);

	return audsrv_play_audio((const char *)data, bytes) >= 0;
}

int S9xAudsrvSoundDriver::space_free()
{
	return audsrv_available();
}

std::pair<int, int> S9xAudsrvSoundDriver::buffer_level()
{
	/* (queued, max) in bytes; max = 96 ms of 16-bit stereo. */
	int max_bytes = rate * 96 * 4 / 1000;
	if (max_bytes <= 0)
		max_bytes = 48000 * 96 * 4 / 1000;
	return { audsrv_queued(), max_bytes };
}
