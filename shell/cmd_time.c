/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
/*  Command: time
    Arguments:
    	set UTC NSEC - sets time
    	raw - dumps raw time
    	<none> - dumps pretty time

    Description: (re)starts/stops the PTP session. */

#include "wrc.h"
#include "wrpc.h"

#include "shell.h"
#include "util.h"
#include "dev/console.h"
#include "dev/pps_gen.h"
#include "cmds.h"

static const char * const time_cmds[] =
{
	 [0] = "set",
	 [1] = "setsec",
	 [2] = "setnsec",
	 [3] = "raw",
#ifdef CONFIG_CMD_TIME_MONITOR
	 [4] = "monitor"
#endif
};

#ifdef CONFIG_CMD_TIME_MONITOR
static int time_monitor(void)
{
	term_clear();

	pp_printf("Printing current timestamp (seconds only), cancel with <ESC>, <c> or <q>\n");

	uint64_t sec;
	uint64_t sec_prev = 0;
	uint32_t nsec;

	while (1) {
		char c = console_getc();
		if (c == 27 || c == 'q' || c == 'c') {
			break;
		}

		shw_pps_gen_get_time(&sec, &nsec);
		if (sec != sec_prev) {
			sec_prev = sec;

			// move cursor to line two, clear and print
			pp_printf("\e[%d;%df", 2, 0);
			term_clear_to_end();
			pp_printf("%llu (%s)", sec, format_time(sec, TIME_FORMAT_LEGACY));
		}
	}
	return 0;
}
#endif /* CONFIG_CMD_TIME_MONITOR */

/**
 * @shellcommand time
 * @shellusage time
 * Displays the current WRPC time.
 *
 * @shellusage time set <seconds> <nanoseconds>
 * Sets the WRPC time.
 *
 * @shellusage time setsec <seconds>
 * Sets only the seconds portion of the WRPC time. In Grandmaster mode this
 * is useful when nanoseconds remain aligned to external 1-PPS and 10 MHz.
 *
 * @shellusage time setnsec <nanoseconds>
 * Sets only the nanoseconds portion of the WRPC time.
 *
 * @shellusage time raw
 * Displays the time as seconds and nanoseconds.
 *
 * @shellusage time monitor
 * Continuously displays the current seconds value and exits on ESC, c, or q.
 * Available when CONFIG_CMD_TIME_MONITOR is enabled. This debug command
 * blocks execution of other shell commands while active.
 */
int cmd_time(const char *args[])
{
	int icmd;
	uint64_t sec;
	uint32_t nsec;

	shw_pps_gen_get_time(&sec, &nsec);

	if (!args[0]) {
		pp_printf("%s +%d nanoseconds.\n",
			  format_time(sec, TIME_FORMAT_LEGACY),
			  (unsigned int) nsec);
		/* fixme: clock freq is not always 125 MHz */
		return 0;
	}

	icmd = sub_cmd(time_cmds, ARRAY_SIZE(time_cmds), args);

	switch(icmd) {
	case 0:
		if (!args[2] || wrc_ptp_get_mode() == WRC_MODE_SLAVE)
			return -1;
		shw_pps_gen_set_time((uint64_t) atoi(args[1]),
				     atoi(args[2]), PPSG_SET_ALL);
		return 0;
	case 1:
		if (!args[1] || wrc_ptp_get_mode() == WRC_MODE_SLAVE)
			return -1;
		shw_pps_gen_set_time((int64_t) atoi(args[1]), 0, PPSG_SET_SEC);
		return 0;
	case 2:
		if (!args[1] || wrc_ptp_get_mode() == WRC_MODE_SLAVE)
			return -1;
		shw_pps_gen_set_time(0, atoi(args[1]), PPSG_SET_NSEC);
		return 0;
	case 3:
		pp_printf("%d %d\n", (unsigned int) sec, (unsigned int) nsec);
		return 0;
#ifdef CONFIG_CMD_TIME_MONITOR
	case 4:
		time_monitor();
		return 0;
#endif
	}


	return 0;
}
