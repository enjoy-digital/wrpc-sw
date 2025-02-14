/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 - 2025 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *         Arthur Finkelmann <arthur.finkelmann@missinglinkelectronics.com>
 *
 * Released according to the GNU GPL, version 2 or any later version,
 * OR according to the GNU LGPL, version 3.0 or any later version
 */

/*
 * Command: gnss
 * 	Description: Enables time of day retrieval from a GNSS device
 *	Arguments:
 *		status - displays latest fix status, date and time parsed from GNSS device
 *		raw - displays raw data received from GNSS device
 *		sync - synchronizes local time to GNSS time (requires node to be in GM mode)
 */

#include "dev/console.h"
#include "dev/gnss_time.h"
#include "shell.h"
#include "util.h"
#include "wrc.h"
#include "wrpc.h"


static const char * const gnss_cmds[] = {
	[0] = "status",
	[1] = "raw",
	[2] = "sync"
};

/**
 * @brief Shell command: print fix status, date and time received from GNSS module
 * interface
 *
 * @return int 0 on success (always)
 */
static int gnss_status(void)
{
	int64_t utc_timestamp = gnss_get_utc();
	int hasfix = gnss_get_fix();

	pp_printf("GNSS fix: %s \n", (hasfix > 0 ? "valid" : "invalid"));

	if (utc_timestamp < 0) {
		pp_printf("GNSS UTC: invalid \n");
	} else {
		pp_printf("GNSS UTC: %s \n", format_time(utc_timestamp, TIME_FORMAT_SORTED));
	}

	return 0;
}

static int cmd_gnss(const char *args[])
{
	int icmd;
	icmd = sub_cmd(gnss_cmds, ARRAY_SIZE(gnss_cmds), args);

	switch(icmd) {
	case 0:
		return gnss_status();
	case 1:
		gnss_raw(!!atoi(args[1]));
		break;
	case 2:
		gnss_sync();
		break;
	}
	return 0;
}

DEFINE_WRC_COMMAND(gnss) = {
	.name = "gnss",
	.exec = cmd_gnss,
};
