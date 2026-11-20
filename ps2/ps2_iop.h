#pragma once
/* IOP bring-up: reset the IOP (unless booted from host:) and load the
 * ps2sdk IRX drivers embedded in the ELF.
 *
 * Several EE RPC clients (audsrv_init, padInit, mtapInit, fileXioInit)
 * spin forever binding to a server that was never loaded, so callers must
 * check the matching ps2_iop_*_ok() before initializing a client. */

void ps2_iop_init(const char *argv0);

bool ps2_iop_pad_ok(void);   /* sio2man + padman */
bool ps2_iop_mtap_ok(void);  /* mtapman */
bool ps2_iop_audio_ok(void); /* libsd + audsrv */
