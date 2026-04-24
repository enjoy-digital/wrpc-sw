/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "softpll/softpll_ng.h"
#include "hw.h"

int main(int argc, char *argv[])
{
	unsigned t;
	unsigned nstep = 300000;
	unsigned ref_cfreq = 62500000;	/* Central frequency */
	unsigned out_cfreq = 62500300;
	unsigned out_ppm = 10;		/* +/- 20ppm */
	enum { TRACE_PLL_STAT, TRACE_MAIN, TRACE_HELPER} trace = TRACE_PLL_STAT;

	for (unsigned i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			nstep = atoi(argv[i+1]);
			i++;
		}
		else if (strcmp(argv[i], "--out-phase") == 0 && i + 1 < argc) {
			phase_out = atoi(argv[i+1]);
			i++;
		}
		else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "pll-stat") == 0)
				trace = TRACE_PLL_STAT;
			else if (strcmp(argv[i], "main") == 0)
				trace = TRACE_MAIN;
			else if (strcmp(argv[i], "helper") == 0)
				trace = TRACE_HELPER;
			else {
				printf ("unhandled -T %s\n", argv[i]);
				return 1;
			}
		}
		else {
			printf ("unknown option %s\n", argv[i]);
			return 1;
		}
	}
	
	spll_very_init();

	spll_init(SPLL_MODE_SLAVE, 0, 0);

	for (t = 0; t < nstep; t++) {
		tics++;

		spll_handle_tags(ID_REF, phase_ref);
		spll_handle_tags(ID_OUT, phase_out);

		/* Update phase_out. */
		/* Relative DAC value between -1;+1 */
		double out_dac =
			(((double)(2 * out_y)) / (1 << BOARD_SPLL_DAC_BITS)) - 1.0;
		/* Delta freq */
		double out_dfreq = out_dac * out_cfreq * out_ppm / 1000000;
		/* Frequency */
		double out_freq = out_cfreq + out_dfreq;

		double phase_out_inc = ref_cfreq / out_freq;

		phase_out += (1 << HPLL_N)
			+ (1 << (HPLL_N * 2)) * (phase_out_inc - 1);

		phase_ref += (1 << HPLL_N);

		phase_ref &= (1 << TAG_BITS) - 1;
		phase_out &= (1 << TAG_BITS) - 1;

		switch (trace) {
		case TRACE_PLL_STAT:
			spll_show_stats();
			break;
		case TRACE_MAIN:
			printf ("t:%-6u out-tag:%-7d ref-tag:%-7d MY:%-5d MFL:%u MPL:%u dtag:%u\n",
				t,
				phase_out,
				phase_ref,
				out_y,
				softpll.mpll.freq_ld.locked,
				softpll.mpll.phase_ld.locked,
				dbg_dtag);
			break;
		case TRACE_HELPER:
			printf ("HL:%u h_y:%u setpoint:%u tag_d0:%u err:%d lock_cnt:%u\n",
				softpll.helper.ld.locked,
				softpll.helper.pi.y,
				softpll.helper.p_setpoint,
				softpll.helper.tag_d0,
				softpll.helper.tag_d0 + softpll.helper.p_adder - softpll.helper.p_setpoint,
				softpll.helper.ld.lock_cnt);
			break;
		}
	}

	return 0;
}
