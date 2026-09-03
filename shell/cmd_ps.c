/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2016 GSI (www.gsi.de)
 * Author: Alessandro Rubini <a.rubini@gsi.de>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <string.h>
#include "wrc.h"
#include "shell.h"
#include "wrc-task.h"
#include "cmds.h"

extern uint32_t print_task_time_threshold;

/**
 * @shellcommand ps
 * @shellusage ps
 * Lists running CPU tasks, including iterations, maximum execution time, and
 * CPU time accumulated since boot or the last reset.
 *
 * @shellusage ps reset
 * Clears task profiling information.
 *
 * @shellusage ps max <milliseconds>
 * Reports tasks whose execution time exceeds the specified threshold and
 * reports when a task exceeds its previous maximum. Use zero to stop these
 * threshold reports.
 */
int cmd_ps(const char *args[])
{
	struct wrc_task_usage *t;
	int i;

	if (args[0]) {
		if(!strcasecmp(args[0], "reset")) {
			for(i = 0; i < wrc_task_nbr() ; i++) {
				t = wrc_task_get_usage(i);
				t->nrun = t->seconds = t->nanos = t->max_run_ticks = 0;
			}
			return 0;
		} else if (!strcasecmp(args[0], "max")) {
			if (args[1])
				print_task_time_threshold = atoi(args[1]);
			pp_printf("print_task_time_threshold %d\n",
				  (unsigned int) print_task_time_threshold);
			return 0;
		}
	}
	pp_printf(" iterations     seconds.micros    max_ms name\n");
	for(i = 0; i < wrc_task_nbr() ; i++)
	{
		t = wrc_task_get_usage(i);
		pp_printf("%11lu   %9lu.%06lu %9lu %s\n", t->nrun,
			  t->seconds, t->nanos/1000, t->max_run_ticks,
			  wrc_task_get_name(i));
	}
	return 0;
}
