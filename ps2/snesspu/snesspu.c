/* snesspu.irx - plays the SNES sound unit's eight voices on SPU2 hardware
 * voices (core 1, voices 0-7), driven by timestamped events from the
 * emulator on the EE. See snesspu.h for the protocol.
 *
 * Two threads: the RPC server takes sample uploads (DMA into SPU2 RAM)
 * and queues events; a 1 ms ticker applies the events whose time has
 * come. libsd must be loaded and initialised (audsrv does both). */

#include <irx.h>
#include <types.h>
#include <loadcore.h>
#include <stdio.h>
#include <sifcmd.h>
#include <sifrpc.h>
#include <sysclib.h>
#include <thbase.h>
#include <libsd.h>

#include "snesspu.h"

IRX_ID("snesspu", 1, 0);

#define CORE 1 /* SPU2 core 1: the one whose output is the final mix */
#define TICK_USEC 1000
#define QUEUE_SIZE 4096 /* events, power of two */

static int rpc_buffer[(SNESSPU_UPLOAD_MAX + 64) / 4] __attribute__((aligned(64)));
static unsigned int rpc_result[4];
static snesspu_status st;
static u8 last_upload_head[64] __attribute__((aligned(64)));
static u8 readback_buf[64] __attribute__((aligned(64)));
static unsigned int last_upload_addr;
static SifRpcDataQueue_t rpc_queue;
static SifRpcServerData_t rpc_server;

/* Event ring: written by the RPC thread, read by the ticker. */
static snesspu_event queue[QUEUE_SIZE];
static volatile unsigned int q_write, q_read;

/* Playback clock, in 32 kHz samples of emulated time. */
static volatile unsigned int newest;    /* emulated time of the last batch */
static volatile unsigned int play_pos;  /* events up to here are applied */
static volatile int clock_valid;
static volatile int stop_request;
static unsigned int last_usec, frac;

static unsigned int koff_deferred;

/* Core attribute register. libsd's sceSdSetCoreAttr(SD_CORE_NOISE_CLK)
 * subtracts where it should mask and clears the output-enable and
 * external-input bits on the way (silence on real hardware), so the noise
 * clock field is written here instead. */
#define CORE_ATTR ((volatile u16 *)(0xBF90019A + (CORE << 10)))
#define ATTR_ON 0x8000
#define ATTR_UNMUTE 0x4000
#define ATTR_NOISE_CLK 0x3F00
#define ATTR_EX_INPUT 0x0001

static void set_noise_clock(unsigned int clk)
{
	/* (the low bits 4-5 are the transfer mode libsd manages) */
	*CORE_ATTR = (*CORE_ATTR & ~ATTR_NOISE_CLK) | ATTR_ON | ATTR_UNMUTE | ATTR_EX_INPUT | ((clk & 0x3F) << 8);
}

static unsigned int now_usec(void)
{
	iop_sys_clock_t c;
	u32 sec, usec;
	GetSystemTime(&c);
	SysClock2USec(&c, &sec, &usec);
	return sec * 1000000u + usec; /* wraps; only differences are used */
}

static void all_off(void)
{
	int v;
	sceSdSetSwitch(CORE | SD_SWITCH_KOFF, (1 << SNESSPU_VOICES) - 1);
	for (v = 0; v < SNESSPU_VOICES; v++)
	{
		sceSdSetParam(CORE | (v << 1) | SD_VPARAM_VOLL, 0);
		sceSdSetParam(CORE | (v << 1) | SD_VPARAM_VOLR, 0);
	}
	sceSdSetSwitch(CORE | SD_SWITCH_NON, 0);
	sceSdSetSwitch(CORE | SD_SWITCH_PMON, 0);
	koff_deferred = 0;
}

static void setup_voices(void)
{
	int v;
	for (v = 0; v < SNESSPU_VOICES; v++)
	{
		sceSdSetParam(CORE | (v << 1) | SD_VPARAM_ADSR1, SNESSPU_ADSR1);
		sceSdSetParam(CORE | (v << 1) | SD_VPARAM_ADSR2, SNESSPU_ADSR2);
		sceSdSetParam(CORE | (v << 1) | SD_VPARAM_PITCH, 0);
	}
	set_noise_clock(0);
	all_off();
}

/* Apply everything due. Key-ons and key-offs of one tick go to the chip
 * together: a key-off followed by a key-on of the same voice is just the
 * key-on (it restarts the voice); a key-off after a key-on waits for the
 * next tick so the note is not lost. */
static void apply_due(void)
{
	unsigned int kon = 0, koff = koff_deferred;
	koff_deferred = 0;

	while (q_read != q_write)
	{
		const snesspu_event *e = &queue[q_read & (QUEUE_SIZE - 1)];
		int v = e->voice & (SNESSPU_VOICES - 1);
		unsigned int bit = 1u << v;

		if ((int)(e->time - play_pos) > 0)
			break;

		switch (e->type)
		{
		case SNESSPU_EV_KON:
			sceSdSetAddr(CORE | (v << 1) | SD_VADDR_SSA, (unsigned int)e->value * 16);
			st.key_ons++;
			kon |= bit;
			koff &= ~bit;
			break;
		case SNESSPU_EV_KOFF:
			if (kon & bit)
				koff_deferred |= bit;
			else
				koff |= bit;
			break;
		case SNESSPU_EV_VOLL:
			sceSdSetParam(CORE | (v << 1) | SD_VPARAM_VOLL, e->value);
			break;
		case SNESSPU_EV_VOLR:
			sceSdSetParam(CORE | (v << 1) | SD_VPARAM_VOLR, e->value);
			break;
		case SNESSPU_EV_PITCH:
			sceSdSetParam(CORE | (v << 1) | SD_VPARAM_PITCH, e->value);
			break;
		case SNESSPU_EV_MVOLL:
			sceSdSetParam(CORE | SD_PARAM_MVOLL, e->value);
			break;
		case SNESSPU_EV_MVOLR:
			sceSdSetParam(CORE | SD_PARAM_MVOLR, e->value);
			break;
		case SNESSPU_EV_NON:
			sceSdSetSwitch(CORE | SD_SWITCH_NON, e->value & 0xFF);
			break;
		case SNESSPU_EV_PMON:
			sceSdSetSwitch(CORE | SD_SWITCH_PMON, e->value & 0xFE);
			break;
		case SNESSPU_EV_NOISE:
			set_noise_clock(e->value);
			break;
		case SNESSPU_EV_ALL_OFF:
			all_off();
			kon = koff = 0;
			break;
		}
		q_read++;
		st.events_applied++;
	}

	if (koff)
		sceSdSetSwitch(CORE | SD_SWITCH_KOFF, koff);
	if (kon)
		sceSdSetSwitch(CORE | SD_SWITCH_KON, kon);
}

static void ticker(void *arg)
{
	(void)arg;
	last_usec = now_usec();
	for (;;)
	{
		unsigned int t, dt, adv;

		DelayThread(TICK_USEC);

		if (stop_request)
		{
			q_read = q_write;
			clock_valid = 0;
			all_off();
			sceSdSetParam(CORE | SD_PARAM_MVOLL, 0x3FFF);
			sceSdSetParam(CORE | SD_PARAM_MVOLR, 0x3FFF);
			stop_request = 0;
		}

		t = now_usec();
		dt = t - last_usec;
		last_usec = t;
		if (!clock_valid)
			continue;
		if (dt > 100000)
			dt = 100000;

		/* 32 samples per millisecond of real time, but never past what
		 * the emulator has produced: when it runs slow the music slows
		 * with it instead of running out of events. */
		frac += dt * 32;
		adv = frac / 1000;
		frac -= adv * 1000;
		if ((int)(newest - (play_pos + adv)) < 0)
			adv = (int)(newest - play_pos) > 0 ? newest - play_pos : 0;
		play_pos += adv;

		apply_due();
	}
}

static void take_events(const snesspu_batch *b, const snesspu_event *ev)
{
	unsigned int i, count = b->count;
	int err;

	if (count > SNESSPU_BATCH_MAX)
		count = SNESSPU_BATCH_MAX;
	for (i = 0; i < count; i++)
	{
		if (q_write - q_read >= QUEUE_SIZE)
		{
			st.events_dropped += count - i;
			break; /* full: the ticker is not keeping up; drop */
		}
		st.events_in++;
		queue[q_write & (QUEUE_SIZE - 1)] = ev[i];
		q_write++;
	}

	/* Keep the play position SNESSPU_DELAY behind the emulator. Small
	 * errors (the two clocks drifting, the emulator's frame pacing) are
	 * steered out; a large one means a pause or a time jump. */
	newest = b->now;
	err = (int)((b->now - SNESSPU_DELAY) - play_pos);
	if (!clock_valid || err > SNESSPU_DELAY || err < -2 * SNESSPU_DELAY)
	{
		play_pos = b->now - SNESSPU_DELAY;
		clock_valid = 1;
	}
	else
		play_pos += err / 8;
}

static int upload(const snesspu_upload *u, u8 *data)
{
	if (u->size == 0 || u->size > SNESSPU_UPLOAD_MAX || (u->size & 63) || (u->addr & 63) ||
	    u->addr < SNESSPU_RAM_BASE || u->addr + u->size > SNESSPU_RAM_END)
		return -1;
	if (sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, data, (u32 *)u->addr, u->size) < 0)
		return -2;
	sceSdVoiceTransStatus(0, 1); /* wait */
	memcpy(last_upload_head, data, 64);
	last_upload_addr = u->addr;
	return 0;
}

static void fill_status(void)
{
	int v;
	st.play_pos = play_pos;
	st.newest = newest;
	st.mvol = ((unsigned int)sceSdGetParam(CORE | SD_PARAM_MVOLL) << 16) | (sceSdGetParam(CORE | SD_PARAM_MVOLR) & 0xFFFF);
	st.core_attr = *CORE_ATTR;
	for (v = 0; v < 8; v++)
		st.envx[v] = sceSdGetParam(CORE | (v << 1) | SD_VPARAM_ENVX);
	st.nax[0] = sceSdGetAddr(CORE | (0 << 1) | SD_VADDR_NAX);
	st.nax[1] = sceSdGetAddr(CORE | (1 << 1) | SD_VADDR_NAX);
	st.endx = sceSdGetSwitch(CORE | SD_SWITCH_ENDX);

	/* Is the sample data really in SPU2 RAM? */
	if (last_upload_addr)
	{
		memset(readback_buf, 0xEE, 64);
		if (sceSdVoiceTrans(0, SD_TRANS_READ | SD_TRANS_MODE_DMA, readback_buf, (u32 *)last_upload_addr, 64) < 0)
			st.readback = 3;
		else
		{
			sceSdVoiceTransStatus(0, 1);
			st.readback = memcmp(readback_buf, last_upload_head, 64) ? 2 : 1;
		}
	}
}

static void *rpc_command(int fno, void *buffer, int length)
{
	(void)length;
	rpc_result[0] = 0;
	switch (fno)
	{
	case SNESSPU_CMD_INIT:
		stop_request = 1;
		while (stop_request)
			DelayThread(TICK_USEC);
		setup_voices();
		rpc_result[0] = SNESSPU_VERSION;
		break;
	case SNESSPU_CMD_UPLOAD:
	{
		int res = upload((const snesspu_upload *)buffer, (u8 *)buffer + sizeof(snesspu_upload));
		if (res)
		{
			st.uploads_failed++;
			st.last_upload_error = res;
		}
		else
			st.uploads_ok++;
		rpc_result[0] = (unsigned int)res;
		break;
	}
	case SNESSPU_CMD_STATUS:
		fill_status();
		return &st;
	case SNESSPU_CMD_EVENTS:
		take_events((const snesspu_batch *)buffer, (const snesspu_event *)((u8 *)buffer + sizeof(snesspu_batch)));
		break;
	case SNESSPU_CMD_STOP:
		stop_request = 1;
		while (stop_request)
			DelayThread(TICK_USEC);
		break;
	}
	return rpc_result;
}

static void rpc_thread(void *arg)
{
	(void)arg;
	sceSifInitRpc(0);
	sceSifSetRpcQueue(&rpc_queue, GetThreadId());
	sceSifRegisterRpc(&rpc_server, SNESSPU_RPC_ID, rpc_command, rpc_buffer, NULL, NULL, &rpc_queue);
	sceSifRpcLoop(&rpc_queue);
}

static int start_thread(void (*entry)(void *), int priority)
{
	iop_thread_t t;
	int id;
	t.attr = TH_C;
	t.option = 0;
	t.thread = entry;
	t.stacksize = 0x1000;
	t.priority = priority;
	id = CreateThread(&t);
	if (id < 0)
		return id;
	StartThread(id, NULL);
	return id;
}

int _start(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	/* The ticker runs above the RPC thread (which busy-waits on sample
	 * transfers) and above audsrv's threads. */
	if (start_thread(ticker, 30) < 0 || start_thread(rpc_thread, 40) < 0)
		return MODULE_NO_RESIDENT_END;
	return MODULE_RESIDENT_END;
}
