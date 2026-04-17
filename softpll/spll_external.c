/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2010 - 2013 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* spll_external.h - implementation of SoftPLL servo for the 
   external (10 MHz - Grandmaster mode) reference channel */

#include "wrc.h"
#include "softpll_ng.h"
#include "irq.h"

#include <hw/softpll_regs.h>
#include <hw/pps_gen_regs.h>

#define SPLL ((volatile struct SPLL_WB*) (BASE_SOFTPLL))
#define PPSG ((volatile struct PPSG_WB*) (BASE_PPS_GEN))

/* The aligner produces a sample at 100Hz, so every 10_000_000 ns */
#define ALIGN_SAMPLE_PERIOD 10000000

/* External clock frequency is 10Mhz, so its period is 100ns */
#define EXT_PERIOD_NS 100
#define EXT_FREQ_HZ 10000000

static const int ext_pps_latency[] = {
	37300, // PERIPH_WRS_STD_NO_LJ
	-4500, // PERIPH_WRS_STD_WITH_LJD
	111395,// PERIPH_WRS_FL_SYNCTECH
	16000  // PERIPH_WRS_LJ_SAFRAN
};

void external_init(volatile struct spll_external_state *s, int ext_ref)
{
	int idx = spll_n_chan_ref + spll_n_chan_out;

	/* If the LJD board is present, use the 10Mhz from it */
	if (scb_ljd_present_global)
		idx++;

	/* Helper now tracks the external clock */
	helper_init(s->helper, idx);
	/* So does main clock */
	mpll_init(s->main, idx, spll_n_chan_ref);

	s->align_state = ALIGN_STATE_EXT_OFF;
	s->enabled = 0;
}

void external_start(struct spll_external_state *s)
{
	helper_start(s->helper);

	SPLL->ECCR = SPLL_ECCR_EXT_EN;

	s->align_state = ALIGN_STATE_WAIT_CLKIN;
	s->enabled = 1;

	spll_debug ( SPLL_DBG_SRC_EXT, SPLL_DBG_SIGNAL_EVENT, SPLL_DBG_EVT_START, 1 );
}

int external_locked(volatile struct spll_external_state *s)
{
	if (!s->helper->ld.locked || !s->main->locked
	    || !(SPLL->ECCR & SPLL_ECCR_EXT_REF_LOCKED)  // ext PLL became unlocked
	    || (SPLL->ECCR & SPLL_ECCR_EXT_REF_STOPPED))   // 10MHz unplugged (only SPEC)
		return 0;
	
	switch(s->align_state) {
		case ALIGN_STATE_EXT_OFF:
		case ALIGN_STATE_WAIT_CLKIN:
		case ALIGN_STATE_WAIT_PLOCK:
		case ALIGN_STATE_START:
		case ALIGN_STATE_START_MAIN:
		case ALIGN_STATE_INIT_CSYNC:
		case ALIGN_STATE_WAIT_CSYNC:
			return 0;
		default:
			return 1;
	}
}

/* 100 times per second, the spll_aligner produce a sample, which is the
   tick number of the 10Mhz input since its pps.
   Return the time internal since the pps input (in NS).
   The remainder by ALIGN_SAMPLE_PERIOD of the result is computed by the caller,
   to get the offset between the ref_clock and the ext_clock
*/
static int align_sample(int channel, int *v)
{
	int mask = (1 << channel);

	if(SPLL->AL_CR & mask) {
		/* Got a sample */
		SPLL->AL_CR = mask; // ack
		int ci = SPLL->AL_CIN;
		if(ci > 100 && ci < (EXT_FREQ_HZ - 100) ) { // give some metastability margin, when the counter transitions from EXT_FREQ_HZ-1 to 0
			*v = ci * EXT_PERIOD_NS;
			return 1;
		}
	}
	return 0; // sample not valid
}

static inline int get_pps_latency(int sel)
{
	if (sel >= PERIPH_END)
		return ext_pps_latency[PERIPH_WRS_STD_NO_LJ];
	else
		return ext_pps_latency[sel];
}

int external_align_fsm(volatile struct spll_external_state *s)
{
	int v, done_sth = 0;
	static int timeout;
	static int old_ext_pps_latency_ps;

	switch(s->align_state) {
		case ALIGN_STATE_EXT_OFF:
			break;

		case ALIGN_STATE_WAIT_CLKIN:
			if(!scb_ljd_present_global && !(SPLL->ECCR & SPLL_ECCR_EXT_REF_STOPPED) ) {
				/* If 10Mhz is present, reset PLL */
				SPLL->ECCR |= SPLL_ECCR_EXT_REF_PLLRST;
				s->align_state = ALIGN_STATE_WAIT_PLOCK;
				done_sth++;
			}
#if defined(CONFIG_TARGET_WR_SWITCH)
			else if (scb_ljd_present_global) {
				uint32_t f_ext;
				int ljd_ad9516_stat;
				/* reset ljd ad9516 */
				SPLL->ECCR |= SPLL_ECCR_EXT_REF_PLLRST;
				timer_delay(10);
				SPLL->ECCR &= (~SPLL_ECCR_EXT_REF_PLLRST);
				timer_delay(10);
				ljd_ad9516_stat = ljd_ad9516_init();
				/* TODO: measure the OSC-EXT freq */
				f_ext = 10000000;
				if (!ljd_ad9516_stat && (f_ext > 9999000) && (f_ext < 10001000)) {
					s->align_state = ALIGN_STATE_WAIT_PLOCK;
					pp_printf("External AD9516 locked\n");
				}
			}
#endif
			break;

		case ALIGN_STATE_WAIT_PLOCK:
			/* Wait for 10Mhz PLL lock */
			SPLL->ECCR &= (~SPLL_ECCR_EXT_REF_PLLRST);
			if(SPLL->ECCR & SPLL_ECCR_EXT_REF_STOPPED )
				s->align_state = ALIGN_STATE_WAIT_CLKIN;
			else if(SPLL->ECCR & SPLL_ECCR_EXT_REF_LOCKED)
				s->align_state = ALIGN_STATE_START;
			done_sth++;
			break;

		case ALIGN_STATE_START:
			/* Wait for helper PLL lock */
			if(s->helper->ld.locked) {
				/* Start mpll */
				disable_irq();
				mpll_start(s->main);
				enable_irq();
				s->align_state = ALIGN_STATE_START_MAIN;
				done_sth++;
			} else if (time_after(timer_get_tics(), timeout + 5*TICS_PER_SECOND)) {
				pll_verbose("EXT: timeout, restarting\n");
				s->align_state = ALIGN_STATE_WAIT_CLKIN;
			}
			break;

		case ALIGN_STATE_START_MAIN:
			SPLL->AL_CR = 2;
			if(s->helper->ld.locked && s->main->locked) {
				/* Start PPS output generation */
				PPSG->CR = PPSG_CR_CNT_EN | PPSG_CR_PWIDTH_W(10);
				PPSG->ADJ_NSEC = 5;
				/* Synchronize with PPS input */
				PPSG->ESCR = PPSG_ESCR_SYNC;
				s->align_state = ALIGN_STATE_INIT_CSYNC;
				pll_verbose("EXT: DMTD locked.\n");
				done_sth++;
			} else if (time_after(timer_get_tics(), timeout + 5*TICS_PER_SECOND)) {
				pll_verbose("EXT: timeout, restarting\n");
				s->align_state = ALIGN_STATE_WAIT_CLKIN;
			}
			break;

		case ALIGN_STATE_INIT_CSYNC:
			if (PPSG->ESCR & PPSG_ESCR_SYNC) {
				/* Synchronized with PPS */
				s->align_timer = timer_get_tics() + 2 * TICS_PER_SECOND;
				s->align_state = ALIGN_STATE_WAIT_CSYNC;
				done_sth++;
			}
			break;

		case ALIGN_STATE_WAIT_CSYNC:
			if(time_after_eq(timer_get_tics(), s->align_timer)) {
				s->align_state = ALIGN_STATE_START_ALIGNMENT;
				s->align_shift = 0;
				pll_verbose("EXT: CSync complete.\n");
				done_sth++;
			}
			break;

		case ALIGN_STATE_START_ALIGNMENT:
			if(align_sample(1, &v)) {
				v %= ALIGN_SAMPLE_PERIOD;
				if(v == 0 || v >= ALIGN_SAMPLE_PERIOD / 2) {
					s->align_target = 0;
					s->align_step = -100;					
				} else if (s > 0) { /* FIXME ?? */
					s->align_target = ALIGN_SAMPLE_PERIOD-EXT_PERIOD_NS;
					s->align_step = 100;
				}

				pll_verbose("EXT: Align target %d, step %d.\n", s->align_target, s->align_step);
				s->align_state = ALIGN_STATE_WAIT_SAMPLE;
				done_sth++;
			}
			break;

		case ALIGN_STATE_WAIT_SAMPLE:
			if(!mpll_shifter_busy(s->main) && align_sample(1, &v)) {
				v %= ALIGN_SAMPLE_PERIOD;
				if(v != s->align_target) {
					/* Continue to adjust the phase */
					s->align_shift += s->align_step;
					mpll_set_phase_shift(s->main, s->align_shift);
				} else {
					/* Constant latency depending on a WRS type */
					s->align_shift += get_pps_latency(scb_ljd_present_global);
					/* Latency tuned by WRS ARM software */
					s->align_shift += s->pps_latency_ps;
					old_ext_pps_latency_ps = s->pps_latency_ps;
					mpll_set_phase_shift(s->main, s->align_shift);
					s->align_state = ALIGN_STATE_COMPENSATE_DELAY;
				}
				done_sth++;
			}
			break;

		case ALIGN_STATE_COMPENSATE_DELAY:
			if(!mpll_shifter_busy(s->main)) {
				pll_verbose("EXT: Align done.\n");
				s->align_state = ALIGN_STATE_LOCKED;
				done_sth++;
			}
			break;

		case ALIGN_STATE_LOCKED:
			if(!external_locked(s)) {
				s->align_state = ALIGN_STATE_WAIT_CLKIN;
				done_sth++;
			}

			if (old_ext_pps_latency_ps != s->pps_latency_ps) {
				pp_printf("EXT: Align changed old %d new %d\n",
					  old_ext_pps_latency_ps,
					  s->pps_latency_ps);
				s->align_shift -= old_ext_pps_latency_ps;
				s->align_shift += s->pps_latency_ps;
				old_ext_pps_latency_ps = s->pps_latency_ps;
				mpll_set_phase_shift(s->main, s->align_shift);

				/* Go back to ALIGN_STATE_COMPENSATE_DELAY
				 * to make sure that shifting is finished */
				s->align_state = ALIGN_STATE_COMPENSATE_DELAY;
				done_sth++;
			}

			break;

		default:
			break;
	}
	if (done_sth > 0) {
		timeout = timer_get_tics();
	}
	return done_sth != 0;
}
