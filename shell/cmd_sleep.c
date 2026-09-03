/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2013 CERN (www.cern.ch)
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include "wrc.h"
#include "shell.h"
#include "cmds.h"

/**
 * @shellcommand sleep
 * @shellusage sleep
 * Delays shell execution for the specified number of seconds. The optional
 * argument defaults to one second.
 */
int cmd_sleep(const char *args[])
{
	int sec = 1;

	if (args[0])
		sec = atoi(args[0]);
	while (sec--)
		usleep(1000 * 1000);
	return 0;
}
