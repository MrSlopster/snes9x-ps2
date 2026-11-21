/* Hang watchdog (see ps2_watchdog.h). */

#include <stdio.h>
#include <string.h>

extern "C" {
#include <kernel.h>
}

#include "ps2_watchdog.h"
#include "ps2_net.h"
#include "ps2_menu.h"

const char *volatile ps2_stage = "start";

#define STALL_VBLANKS 150 /* 3 s at 50 Hz, 2.5 s at 60 Hz */

static volatile int armed = 0;
static volatile unsigned since_kick = 0;
static volatile unsigned stall_epc = 0;
/* A few more samples after the first, to see the extent of a loop. */
static volatile unsigned extra_epc[6];
static volatile unsigned extra_n = 0;
static volatile unsigned reports = 0;
static int sema = -1;
static unsigned char stack[16 * 1024] __attribute__((aligned(16)));

void ps2_watchdog_kick(void)
{
	since_kick = 0;
}

void ps2_watchdog_arm(bool on)
{
	since_kick = 0;
	armed = on ? 1 : 0;
}

/* Interrupt context: the exception program counter is where the main
 * thread was when the vblank came. */
void ps2_watchdog_vblank(void)
{
	if (!armed || sema < 0)
		return;
	unsigned n = ++since_kick;
	unsigned epc;
	__asm__ volatile("mfc0 %0, $14" : "=r"(epc));
	if (n >= STALL_VBLANKS - 6 && n < STALL_VBLANKS)
		extra_epc[n - (STALL_VBLANKS - 6)] = epc, extra_n = n - (STALL_VBLANKS - 6) + 1;
	if (n == STALL_VBLANKS || (n > STALL_VBLANKS && n % (STALL_VBLANKS * 2) == 0))
	{
		stall_epc = epc;
		iSignalSema(sema);
	}
}

static void watchdog_thread(void *)
{
	for (;;)
	{
		WaitSema(sema);
		char line[160];
		snprintf(line, sizeof(line), "WATCHDOG no frame for %u vblanks: pc %08x stage \"%s\" (report %u)", since_kick,
			 stall_epc, ps2_stage ? ps2_stage : "?", ++reports);
		printf("%s\n", line);
		char more[160];
		snprintf(more, sizeof(more), "WATCHDOG earlier pcs: %08x %08x %08x %08x %08x %08x", extra_epc[0], extra_epc[1],
			 extra_epc[2], extra_epc[3], extra_epc[4], extra_epc[5]);
		printf("%s\n", more);
		if (ps2_net_ready())
		{
			/* UDP: say it more than once */
			for (int i = 0; i < 3; i++)
				ps2_net_log(line);
			ps2_net_log(more);
		}
		else
		{
			/* No network (yet): the screen is the only way out. */
			char a[80], b[80];
			snprintf(a, sizeof(a), "pc %08x  stage: %s", stall_epc, ps2_stage ? ps2_stage : "?");
			snprintf(b, sizeof(b), "%08x %08x %08x %08x %08x", extra_epc[1], extra_epc[2], extra_epc[3],
				 extra_epc[4], extra_epc[5]);
			ps2_menu_status("Hang watchdog (please report these)", a, b);
		}
	}
}

void ps2_watchdog_init(void)
{
	if (sema >= 0)
		return;
	ee_sema_t s;
	memset(&s, 0, sizeof(s));
	s.init_count = 0;
	s.max_count = 8;
	sema = CreateSema(&s);
	if (sema < 0)
		return;

	ee_thread_t t;
	memset(&t, 0, sizeof(t));
	t.func = (void *)watchdog_thread;
	t.stack = stack;
	t.stack_size = sizeof(stack);
	t.gp_reg = &_gp;
	t.initial_priority = 8; /* above the main thread and the network stack */
	int id = CreateThread(&t);
	if (id >= 0)
		StartThread(id, NULL);
}
