#pragma once
/* UDP log over the PS2 network adapter, for profiling/debugging on real
 * hardware. IP settings come from OPL's conf_network.cfg (or the common
 * SYS-CONF/IPCONFIG.DAT), else DHCP. Lines are broadcast to the local
 * subnet on UDP port PS2_NETLOG_PORT unless ps2.cfg sets netlog_host.
 * Receive with ps2/tools/netlog.py. */

#define PS2_NETLOG_PORT 18195

/* Bring the network up (shows progress on screen). Safe to call again;
 * returns false when there is no adapter / link / address. */
bool ps2_net_start(void);
bool ps2_net_ready(void);

/* Send one line (no-op when the network is not up). */
void ps2_net_log(const char *line);
