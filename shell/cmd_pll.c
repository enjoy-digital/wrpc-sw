/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "wrc.h"

#include "hw/softpll_regs.h"
#include "softpll_ng.h"
#include "shell.h"
#include "cmds.h"

#define CMD_INIT 0
#define CMD_CL 1
#define CMD_STAT 2
#define CMD_SPS 3
#define CMD_GPS 4
#define CMD_START 5
#define CMD_STOP 6
#define CMD_SDAC 7
#define CMD_GDAC 8
#define CMD_GAIN 9
#if 0
#define CMD_REGS 10
#endif

/* The sub-commands.  */
static const char * const pll_menu[] =
{
	[CMD_INIT] = "init",
	[CMD_CL] = "cl",
	[CMD_STAT] = "stat",
	[CMD_SPS] = "sps",
	[CMD_GPS] = "gps",
	[CMD_START] = "start",
	[CMD_STOP] = "stop",
	[CMD_SDAC] = "sdac",
	[CMD_GDAC] = "gdac",
	[CMD_GAIN] = "gain",
#ifdef CMD_REGS
	[CMD_REGS] = "regs",
#endif
};

/* Number of arguments for the sub-commands.  Mind the order!  */
static const unsigned char nargs[] =
{
	[CMD_INIT] = 3,
	[CMD_CL] = 1,
	[CMD_STAT] = 0,
	[CMD_SPS] = 2,
	[CMD_GPS] = 1,
	[CMD_START] = 1,
	[CMD_STOP] = 1,
	[CMD_SDAC] = 2,
	[CMD_GDAC] = 1,
	[CMD_GAIN] = 5,
#ifdef CMD_REGS
	[CMD_REGS] = 0,
#endif
};

/**
 * @shellcommand pll
 * @shellusage pll init <mode> <ref_channel> <align_pps>
 * Initializes the software PLL.
 *
 * @shellusage pll cl <channel>
 * Checks whether the software PLL channel is locked.
 *
 * @shellusage pll stat
 * Displays software PLL statistics.
 *
 * @shellusage pll sps <channel> <picoseconds>
 * Sets the phase shift for a channel.
 *
 * @shellusage pll gps <channel>
 * Displays the current and target phase shift.
 *
 * @shellusage pll start <channel>
 * Starts the software PLL on a channel.
 *
 * @shellusage pll stop <channel>
 * Stops the software PLL on a channel.
 *
 * @shellusage pll sdac <index> <value>
 * Sets a software PLL DAC value.
 *
 * @shellusage pll gdac <index>
 * Reads a software PLL DAC value.
 *
 * @shellusage pll gain <loop> <sched> <kp> <ki> <shift>
 * Sets software PLL proportional-integral gains.
 */
int cmd_pll(const char *args[])
{
	unsigned narg;
	int vals[8];
	int icmd;

	icmd = sub_cmd(pll_menu, ARRAY_SIZE(pll_menu), args);

	/* Decode arguments.  */
	for (narg = 1; args[narg]; narg++)
		vals[narg] = atoi(args[narg]);

	/* Args from 1 to NARG.  */
	narg--;

	if (icmd < 0 || nargs[icmd] != narg)
		return -EINVAL;

	switch (icmd) {
	case CMD_INIT:
		spll_init(vals[1], vals[2], vals[3]);
		return 0;
	case CMD_CL:
		pp_printf("%d\n", spll_check_lock(vals[1]));
		return 0;
	case CMD_STAT:
		spll_show_stats();
		return 0;
	case CMD_SPS:
		spll_set_phase_shift(vals[1], vals[2]);
		return 0;
	case CMD_GPS:
	{
		int32_t cur, tgt;
		spll_get_phase_shift(vals[1], &cur, &tgt);
		pp_printf("%d %d\n", (int) cur, (int) tgt);
		return 0;
	}
	case CMD_START:
		spll_start_channel(vals[1]);
		return 0;
	case CMD_STOP:
		spll_stop_channel(vals[1]);
		return 0;
	case CMD_SDAC:
		spll_set_dac(vals[1], vals[2]);
		return 0;
	case CMD_GDAC:
		pp_printf("%d\n", spll_get_dac(vals[1]));
		return 0;
	case CMD_GAIN:
		/*                loop     sched    kp       ki       shift */
		spll_set_pi_gain( vals[1], vals[2], vals[3], vals[4], vals[5] );
		return 0;
#ifdef CMD_REGS
	case CMD_REGS:
		pp_printf("OCCR: %08x\n", SPLL->OCCR);
		pp_printf("OCER: %08x\n", SPLL->OCER);
		pp_printf("ECCR: %08x\n", SPLL->ECCR);
		pp_printf("RCER: %08x\n", SPLL->RCER);
		return 0;
#endif
	default:
		return 0;
	}
}
