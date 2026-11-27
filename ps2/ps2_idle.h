#pragma once
/* Exact 65816 idle-loop skipping (see ps2_idle.cpp). The hook is called
 * from the core's branch macro (cpumacro.h, _EE) on taken short backward
 * branches. */

#include "port.h"

extern int ps2_idle_enabled;
extern uint32 ps2_idle_skipped_cycles; /* master cycles since last reset */
extern uint32 ps2_idle_plain_skips, ps2_idle_handshake_skips, ps2_idle_handshake_passes;

void S9xIdleLoopCheck(uint16 target, uint16 after_branch);
/* Reset, ROM load, state load (called from the core). */
void S9xIdleLoopReset(void);

#ifdef ORACLE_IDLE_STATS
extern uint32 ps2_idle_shadow_checked, ps2_idle_shadow_wrong;
#endif
