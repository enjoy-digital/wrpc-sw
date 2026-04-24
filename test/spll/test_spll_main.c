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
	struct spll_main_state mpll;
	unsigned t;
	unsigned nstep = 300000;
	unsigned ref_cfreq = 62500000;	/* Central frequency */
	unsigned out_cfreq = 62500300;
	unsigned out_ppm = 10;		/* +/- 20ppm */
	unsigned verbose = 0;
	unsigned lock_count = 0;

	spll_n_chan_ref = 1;

	/* Random initial value. */
	phase_out = 15425;
	phase_ref = 10;

	for (unsigned i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			verbose++;
		else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			nstep = atoi(argv[i+1]);
			i++;
		}
		else if (strcmp(argv[i], "--out-phase") == 0 && i + 1 < argc) {
			phase_out = atoi(argv[i+1]);
			i++;
		}
		else {
			printf ("unknown option %s\n", argv[i]);
			return 1;
		}
	}
	
	mpll.gain_sched = NULL;

	mpll_init(&mpll, ID_REF, ID_OUT);

	mpll_start(&mpll);

	for (t = 0; t < nstep; t++) {
		mpll_update(&mpll, phase_ref, ID_REF);
		mpll_update(&mpll, phase_out, ID_OUT);
		
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

		if (verbose)
			printf ("t:%-6u out-tag:%-7d ref-tag:%-7d MY:%-5d MFL:%u MPL:%u dtag:%u\n",
				t,
				phase_out,
				phase_ref,
				out_y,
				mpll.freq_ld.locked,
				mpll.phase_ld.locked,
				dbg_dtag);

		if (mpll.locked) {
			lock_count++;
			if (lock_count > 100) {
				printf ("OK at %u\n", t);
				return 0;
			}
		}
		else
			lock_count = 0;
	}

	return 1;
}
