/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 - 2015 CERN (www.cern.ch)
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <string.h>
#include <errno.h>
#include "shell.h"
#include "dev/endpoint.h"
#include "wrc.h"
#include "cmds.h"

int wrc_stat_running;

/**
 * @shellcommand stat
 * @shellusage stat
 * Displays or toggles loggable statistics reporting.
 *
 * @shellusage stat <on|off>
 * Enables or disables loggable statistics reporting.
 *
 * @shellusage stat bts
 * Displays the bitslide value for the established WR link.
 */
int cmd_stat(const char *args[])
{
	/* no arguments: invert */
	if (!args[0]) {
		wrc_stat_running = !wrc_stat_running;
		wrc_stats_last--; /* force a line to be printed */
		if (!wrc_stat_running)
			pp_printf("statistics now off\n");
		return 0;
	}

	/* arguments: bts, on, off */
	if (!strcasecmp(args[0], "bts")) {
		pp_printf("%d ps\n", ep_get_bitslide(&wrc_endpoint_dev));
	} else if (!strcasecmp(args[0], "on")) {
		wrc_stat_running = 1;
		wrc_stats_last--; /* force a line to be printed */
	} else if (!strcasecmp(args[0], "off")) {
		wrc_stat_running = 0;
		pp_printf("statistics now off\n");
	} else if (!strcasecmp(args[0], "1")) {
		wrc_stat_running = -1; /* Special meaning... (only one) */
		wrc_stats_last--; /* force a line to be printed */
	} else
		return -EINVAL;
	return 0;

}
