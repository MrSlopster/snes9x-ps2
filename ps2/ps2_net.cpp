/* UDP network log (see ps2_net.h). EE-side lwIP (libps2ip) on top of the
 * IOP DEV9/NETMAN/SMAP drivers, following ps2sdk's tcpip_dhcp sample. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

extern "C" {
#include <kernel.h>
#include <delaythread.h>
#include <netman.h>
#include <ps2ip.h>
}

#include "ps2_net.h"
#include "ps2_iop.h"
#include "ps2_menu.h"
#include "ps2_config.h"

static bool ready = false;
static bool tried = false;
static int sock = -1;
static struct sockaddr_in dest;

/* Host-order IPv4 settings. */
struct NetSettings
{
	bool found;
	bool dhcp;
	unsigned ip, nm, gw;
	std::string source;
};

static bool parse_ip(const char *s, unsigned *out)
{
	unsigned v[4];
	int n = 0;
	const char *p = s;
	while (*p && n < 4)
	{
		while (*p && !isdigit((unsigned char)*p))
			p++;
		if (!*p)
			break;
		v[n++] = (unsigned)strtoul(p, (char **)&p, 10);
	}
	if (n != 4 || v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255)
		return false;
	*out = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
	return true;
}

static std::string ip_str(unsigned ip)
{
	char b[20];
	snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	return b;
}

/* OPL's conf_network.cfg: key=value lines; key names differ between OPL
 * versions, so match leniently (…dhcp…, …ip_addr / ps2_ip, …netmask,
 * …gateway). */
static bool read_opl_config(const char *path, NetSettings &s)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;

	bool any = false;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *key = line, *val = eq + 1;
		for (char *k = key; *k; k++)
			*k = tolower((unsigned char)*k);

		if (strstr(key, "dhcp"))
			s.dhcp = atoi(val) != 0, any = true;
		else if (strstr(key, "smb") || strstr(key, "dns"))
			continue;
		else if (strstr(key, "ip_addr") || !strcmp(key, "ps2_ip"))
			any |= parse_ip(val, &s.ip);
		else if (strstr(key, "netmask"))
			parse_ip(val, &s.nm);
		else if (strstr(key, "gateway"))
			parse_ip(val, &s.gw);
	}
	fclose(f);
	return any;
}

/* SYS-CONF/IPCONFIG.DAT: "ip netmask gateway" on one line. */
static bool read_ipconfig_dat(const char *path, NetSettings &s)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[128] = { 0 };
	bool ok = false;
	if (fgets(line, sizeof(line), f))
	{
		char a[32], b[32], c[32];
		if (sscanf(line, "%31s %31s %31s", a, b, c) == 3)
			ok = parse_ip(a, &s.ip) && parse_ip(b, &s.nm) && parse_ip(c, &s.gw);
	}
	fclose(f);
	if (ok)
		s.dhcp = false;
	return ok;
}

static NetSettings find_settings(void)
{
	NetSettings s;
	s.found = false;
	s.dhcp = true;
	s.ip = s.nm = s.gw = 0;

	/* First choice: the standard homebrew network config,
	 * "ip netmask gateway" on one line (same file ps2link uses). */
	static const char *dat[] = { "mc0:/SYS-CONF/IPCONFIG.DAT", "mc1:/SYS-CONF/IPCONFIG.DAT", NULL };
	for (int i = 0; dat[i]; i++)
	{
		NetSettings t = s;
		if (read_ipconfig_dat(dat[i], t) && t.ip && t.nm)
		{
			t.found = true;
			t.source = dat[i];
			return t;
		}
	}

	/* Then OPL's settings; only a complete static address is taken from
	 * them. */
	static const char *opl[] = { "mc0:/OPL/conf_network.cfg", "mc1:/OPL/conf_network.cfg",
				     "mass0:/OPL/conf_network.cfg", "mass:/OPL/conf_network.cfg", NULL };
	for (int i = 0; opl[i]; i++)
	{
		NetSettings t = s;
		if (read_opl_config(opl[i], t) && !t.dhcp && t.ip && t.nm)
		{
			t.found = true;
			t.source = opl[i];
			return t;
		}
	}

	s.source = "no config found, using DHCP";
	return s;
}

static void status(const char *line, const char *detail)
{
	printf("netlog: %s %s\n", line, detail ? detail : "");
	ps2_menu_status("Network log", line, detail);
}

static bool link_up(void)
{
	return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

static void set_ip4(struct ip4_addr *a, unsigned host_order)
{
	a->addr = PP_HTONL(host_order);
}

bool ps2_net_start(void)
{
	if (ready)
		return true;
	if (tried)
		return false; /* drivers/stack are one-shot */
	tried = true;

	status("Loading network drivers...", NULL);
	if (!ps2_iop_load_network())
	{
		status("No network adapter", "(DEV9/SMAP did not load)");
		DelayThread(1500 * 1000);
		return false;
	}
	NetManInit();

	NetSettings s = find_settings();
	{
		std::string detail = s.source;
		if (!s.dhcp)
			detail = ip_str(s.ip) + " / " + ip_str(s.nm) + " gw " + ip_str(s.gw) + "  (" + s.source + ")";
		status("Settings:", detail.c_str());
	}

	struct ip4_addr ip, nm, gw;
	if (s.dhcp)
	{
		/* ps2ip.h: init with a dummy link-local address, then enable DHCP. */
		set_ip4(&ip, (169u << 24) | (254u << 16) | 1);
		set_ip4(&nm, 0xFFFF0000u);
		set_ip4(&gw, 0);
	}
	else
	{
		set_ip4(&ip, s.ip);
		set_ip4(&nm, s.nm);
		set_ip4(&gw, s.gw);
	}
	ps2ipInit(&ip, &nm, &gw);

	t_ip_info info;
	if (s.dhcp && ps2ip_getconfig((char *)"sm0", &info) >= 0)
	{
		info.dhcp_enabled = 1;
		ps2ip_setconfig(&info);
	}

	status("Waiting for link...", NULL);
	for (int i = 0; i < 100 && !link_up(); i++)
		DelayThread(100 * 1000);
	if (!link_up())
	{
		status("No network link", "(cable / switch?)");
		DelayThread(1500 * 1000);
		return false;
	}

	if (s.dhcp)
	{
		status("Waiting for DHCP...", NULL);
		for (int i = 0; i < 100; i++)
		{
			if (ps2ip_getconfig((char *)"sm0", &info) >= 0 && info.dhcp_status == DHCP_STATE_BOUND)
				break;
			DelayThread(100 * 1000);
		}
	}

	if (ps2ip_getconfig((char *)"sm0", &info) < 0)
	{
		status("Network setup failed", NULL);
		DelayThread(1500 * 1000);
		return false;
	}
	unsigned my_ip = PP_NTOHL(info.ipaddr.s_addr);
	unsigned my_nm = PP_NTOHL(info.netmask.s_addr);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
	{
		status("UDP socket failed", NULL);
		DelayThread(1500 * 1000);
		return false;
	}
	int on = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

	/* Destination: netlog_host ("a.b.c.d[:port]") or subnet broadcast. */
	unsigned dst = my_ip | ~my_nm;
	unsigned port = PS2_NETLOG_PORT;
	if (!ps2_cfg.netlog_host.empty())
	{
		unsigned h;
		if (parse_ip(ps2_cfg.netlog_host.c_str(), &h))
			dst = h;
		const char *colon = strchr(ps2_cfg.netlog_host.c_str(), ':');
		if (colon && atoi(colon + 1) > 0)
			port = atoi(colon + 1);
	}
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = PP_HTONS(port);
	dest.sin_addr.s_addr = PP_HTONL(dst);

	ready = true;

	std::string detail = "PS2 " + ip_str(my_ip) + " -> " + ip_str(dst) + ":" + std::to_string(port);
	status("Network log ready", detail.c_str());
	std::string hello = "NETLOG hello " + detail + " (" + s.source + ")";
	ps2_net_log(hello.c_str());
	DelayThread(1000 * 1000);
	return true;
}

bool ps2_net_ready(void)
{
	return ready;
}

void ps2_net_log(const char *line)
{
	if (!ready)
		return;
	sendto(sock, line, strlen(line), 0, (struct sockaddr *)&dest, sizeof(dest));
}
