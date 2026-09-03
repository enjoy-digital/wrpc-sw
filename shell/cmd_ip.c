/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 GSI (www.gsi.de)
 * Author: Wesley W. Terpstra <w.terpstra@gsi.de>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "wrc.h"
#include "lib/ipv4.h"
#include "dev/netif.h"
#include "wrc_global.h"

#include "softpll_ng.h"
#include "shell.h"
#include "dev/etherbone.h"
#include "endianness.h"
#include "cmds.h"

void decode_ip(const char *str, unsigned char *ip)
{
	int i, x;

	/* Don't try to detect bad input; need small code */
	for (i = 0; i < 4; ++i) {
		str = fromdec(str, &x);
		ip[i] = x;
		if (*str == '.')
			++str;
	}
}

char *format_ip(char *s, const unsigned char *ip)
{
	pp_sprintf(s, "%d.%d.%d.%d",
		   ip[0], ip[1], ip[2], ip[3]);
	return s;
}

#ifdef CONFIG_CMD_IP_STAT
static void cmd_ip_stat(void)
{
	unsigned i;

	for (i = 0; i < NET_MAX_SOCKETS; i++) {
		struct wrpc_socket *s = net_get_sock(i);
		unsigned ethtyp;

		if (s == NULL)
			continue;
		ethtyp = s->bind_addr.ethertype;
		if (ethtyp == htons(0x0800))
			pp_printf("udp %-5u ", s->bind_addr.udpport);
		else
			pp_printf("eth %04x  ", ntohs(ethtyp));
		pp_printf(" rx: %-9u  tx: %-9u\n", s->rx_pkts, s->tx_pkts);
	}

	for (i = 0; i < netif_get_device_count(); i++) {
		struct wrc_netif_device *nif = netif_get_device(i);
		struct wr_minic *nic = nif->nic;

		pp_printf ("if %u       rx: %-9u  tx: %-9u  err: %-9u  unmatch: %-9u\n",
			   i, nic->rx_count, nic->tx_count, nic->rx_errors,
			   nic->rx_unmatch);
	}
}
#endif

/**
 * @shellcommand ip
 * @shellusage ip get
 * Displays the WRPC IPv4 address.
 *
 * @shellusage ip set <ip>
 * Sets the WRPC IPv4 address.
 *
 * @shellusage ip stat
 * Displays packet usage per client.
 */
int cmd_ip(const char *args[])
{
	unsigned char ip[4];
	char buf[20];

	if (!args[0] || !strcasecmp(args[0], "get")) {
		getIP(ip);
	} else if (!strcasecmp(args[0], "set") && args[1]) {
		ip_status = IP_OK_STATIC;
		decode_ip(args[1], ip);
		setIP(ip);
#if HAS_EB
		eb_setIP(ip);
#endif
#ifdef CONFIG_CMD_IP_STAT
	} else if (!strcmp(args[0], "stat")) {
		cmd_ip_stat();
		return 0;
#endif
	} else {
		return -EINVAL;
	}

	format_ip(buf, ip);
	pp_printf("IP-address: ");
	switch (ip_status) {
	case IP_TRAINING:
		pp_printf("in training\n");
		break;
	case IP_OK_BOOTP:
		pp_printf("%s (bootp)\n", buf);
		break;
	case IP_OK_STATIC:
		pp_printf("%s (static)\n", buf);
		break;
	}
	return 0;
}
