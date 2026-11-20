/* IOP bring-up for the PS2 port.
 *
 * Everything the EE side uses is an RPC client of an IOP module: audsrv
 * (audio), libpad/libmtap (pads), fileXio (iomanX devices such as USB
 * mass:), plus the memory card drivers behind mc0:. None of these are
 * resident after a normal ELF launch, so they are embedded in the ELF
 * (bin2c, see Makefile) and loaded here.
 *
 * The IOP is reset first for a known module set, except when booted from
 * host: (ps2link): a reset would drop ps2link's host: device. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Only fileXioInit() is used from fileXio: it switches newlib's POSIX
 * file calls over to fileXio. All file access stays POSIX. */
#define NEWLIB_PORT_AWARE

extern "C" {
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <fileXio_rpc.h>
}

#include "ps2_iop.h"

#define DECLARE_IRX(name)                     \
	extern "C" unsigned char name##_irx[]; \
	extern "C" unsigned int size_##name##_irx;

DECLARE_IRX(iomanX)
DECLARE_IRX(fileXio)
DECLARE_IRX(sio2man)
DECLARE_IRX(mcman)
DECLARE_IRX(mcserv)
DECLARE_IRX(padman)
DECLARE_IRX(mtapman)
DECLARE_IRX(libsd)
DECLARE_IRX(audsrv)
DECLARE_IRX(usbd)
DECLARE_IRX(bdm)
DECLARE_IRX(bdmfs_fatfs)
DECLARE_IRX(usbmass_bd)
DECLARE_IRX(ps2dev9)
DECLARE_IRX(netman)
DECLARE_IRX(smap)

#define LOAD_IRX(name) load_irx(#name, name##_irx, size_##name##_irx)

static bool pad_ok = false;
static bool mtap_ok = false;
static bool audio_ok = false;

/* A module that returns NO_RESIDENT_END (1) from _start has unloaded
 * itself: its RPC server does not exist. */
static bool load_irx(const char *name, unsigned char *buf, unsigned int size)
{
	int mod_res = 0;
	int id = SifExecModuleBuffer(buf, size, 0, NULL, &mod_res);
	bool ok = id >= 0 && mod_res != 1;

	printf("IOP: %-12s id=%d res=%d %s\n", name, id, mod_res, ok ? "ok" : "FAILED");
	return ok;
}

static bool booted_from_host(const char *argv0)
{
	return argv0 && strncmp(argv0, "host", 4) == 0;
}

void ps2_iop_init(const char *argv0)
{
	SifInitRpc(0);

	if (!booted_from_host(argv0))
	{
		while (!SifIopReset("", 0))
			;
		while (!SifIopSync())
			;
		SifInitRpc(0);
	}
	else
		printf("IOP: booted from %s, keeping the IOP (no reset)\n", argv0);

	/* Allow SifExecModuleBuffer (LoadModuleBuffer) and loading from
	 * any device on retail BIOSes. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	/* File I/O: iomanX + fileXio, so devices registered with iomanX
	 * (bdm's mass:) are reachable through newlib's POSIX calls. */
	if (LOAD_IRX(iomanX) && LOAD_IRX(fileXio))
		fileXioInit();

	bool sio2 = LOAD_IRX(sio2man);

	/* mc0: */
	if (sio2 && LOAD_IRX(mcman))
		LOAD_IRX(mcserv);

	/* Pads (mtapman before padman: padman uses the multitap ports). */
	mtap_ok = sio2 && LOAD_IRX(mtapman);
	pad_ok = sio2 && LOAD_IRX(padman);

	/* Audio */
	audio_ok = LOAD_IRX(libsd) && LOAD_IRX(audsrv);

	/* USB mass storage -> mass: */
	bool usb = LOAD_IRX(usbd) && LOAD_IRX(bdm) && LOAD_IRX(bdmfs_fatfs) &&
		   LOAD_IRX(usbmass_bd);

	/* USB enumeration and the FAT mount are asynchronous: give a stick up
	 * to ~2 s to show up before the base-dir probe runs. */
	if (usb)
	{
		struct stat st;
		for (int i = 0; i < 20; i++)
		{
			if (stat("mass:/", &st) == 0)
			{
				printf("IOP: mass: ready after %d ms\n", i * 100);
				break;
			}
			DelayThread(100 * 1000);
		}
	}
}

bool ps2_iop_pad_ok(void)
{
	return pad_ok;
}

bool ps2_iop_mtap_ok(void)
{
	return mtap_ok;
}

bool ps2_iop_audio_ok(void)
{
	return audio_ok;
}

/* Network adapter drivers for the UDP log: loaded on demand (only when
 * the log is enabled), once. ps2dev9 fails without an adapter. */
bool ps2_iop_load_network(void)
{
	static int state = 0; /* 0 untried, 1 ok, -1 failed */
	if (state == 0)
		state = (LOAD_IRX(ps2dev9) && LOAD_IRX(netman) && LOAD_IRX(smap)) ? 1 : -1;
	return state > 0;
}
