/*
 * This work is part of the White Rabbit project
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include "wrc.h"
#include "shell.h"
#include "ppsi/ppsi.h"
#include "cmds.h"

#if CONFIG_VERBOSE_DETAIL
static const char diag_names[8][8] = {
  "fsm    ",  /* 'verbose 3' */
  "time   ",  /* 'verbose 03' */
  "frames ",  /* 'verbose 003' */
  "servo  ",  /* 'verbose 0003' */
  "bmc    ",
  "ext    ",
  "config ",
  "---    "
};
#endif

/**
 * @shellcommand verbose
 * @shellusage verbose <digits>
 * Sets the PPSI verbosity level. The digits control diagnostic output; for
 * example, verbose 1111 is useful when inspecting PTP operation.
 */
int cmd_verbose(const char *args[])
{
	if (args[0])
		pp_global_d_flags = pp_diag_parse(args[0]);
	pp_printf("PPSI verbosity: %08lx\n", pp_global_d_flags);
#if CONFIG_VERBOSE_DETAIL
	for (unsigned i = 0; i < 8; i++)
		pp_printf("%s %x\n", diag_names[i],
			  (unsigned)(pp_global_d_flags >> ((7 - i) * 4)) & 0x0f);
#endif
	return 0;
}
