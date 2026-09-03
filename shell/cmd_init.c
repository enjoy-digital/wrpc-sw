/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <string.h>
#include "wrc.h"
#include "shell.h"
#include "storage.h"
#include "dev/syscon.h"
#include "dev/i2c.h"
#include "cmds.h"

static const char * const init_cmds[] =
{
	 [0] = "erase",
	 [1] = "add",
	 [2] = "show",
	 [3] = "boot",
};

/**
 * @shellcommand init
 * @shellusage init erase
 * Erases the initialization script.
 *
 * @shellusage init add <command>
 * Adds a command to the end of the initialization script.
 *
 * @shellusage init show
 * Displays the stored initialization script.
 *
 * @shellusage init boot
 * Executes the stored initialization script. The script is also executed
 * automatically when WRPC starts after a soft-core reset.
 */
int cmd_init(const char *args[])
{
	int icmd;

	icmd = sub_cmd(init_cmds, ARRAY_SIZE(init_cmds), args);

	switch (icmd) {
	case 0:
		if (storage_init_erase() < 0)
			pp_printf("Could not erase init script\n");
		return 0;
	case 1:
		if (!args[1])
			return -1;
		if (storage_init_add(args) < 0)
			pp_printf("Could not add the command\n");
		else
			pp_printf("OK.\n");
		return 0;
	case 2:
		shell_show_build_init();
		storage_init_show();
		return 0;
	case 3:
		shell_boot_script();
		return 0;
	default:
		return -1;
	}
}
