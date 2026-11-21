#pragma once
/* Hang watchdog for real hardware, where a stuck console says nothing.
 * While armed, the main loop kicks it once per frame; the vblank interrupt
 * counts the frames since. After about 3 seconds without a kick it notes
 * the interrupted program counter and wakes a high-priority thread, which
 * reports it with the last stage marker on the network log ("WATCHDOG
 * ..."; resolve the address with addr2line on snes9x.elf). This catches
 * busy-waits and endless loops; after a CPU exception the kernel stops
 * everything and nothing is reported - which is information too. */

void ps2_watchdog_init(void);
void ps2_watchdog_arm(bool on); /* off in menus and while loading */
void ps2_watchdog_kick(void);   /* a frame was completed */
void ps2_watchdog_vblank(void); /* from the vblank interrupt handler */

/* Where the main thread is, for the report. A pointer store: cheap enough
 * to leave in. */
extern const char *volatile ps2_stage;
#define PS2_STAGE(s) (ps2_stage = (s))
