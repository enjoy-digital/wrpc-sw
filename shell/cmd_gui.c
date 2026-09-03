/*
 * This work is part of the White Rabbit project
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include "wrc.h"
#include "shell.h"
#include "cmds.h"

/**
 * @shellcommand gui
 * @shellusage gui
 * Starts the WRPC graphical monitor.
 */
int cmd_gui(const char *args[])
{
	redraw_gui();
	shell_activate_ui_command( wrc_mon_gui );
	return 0;
}
