/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2010 - 2013 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* spll_helper.c - implmentation of the Helper PLL servo algorithm. */

#include "softpll_ng.h"
#include "spll_hw.h"

#if defined(CONFIG_TARGET_WR_SWITCH)
/* All not defined WRS versions */
#define HPLL_KP_DEFAULT		150
#define HPLL_KI_DEFAULT		2
/* WRS LJD version from SyncTech */
#define HPLL_LJD_KP_SYNCTECH	-150
#define HPLL_LJD_KI_SYNCTECH	-2

int helper_pll_kp __attribute__((section(".sdata.main_pll_kp"))) = 0;
int helper_pll_ki __attribute__((section(".sdata.main_pll_kp"))) = 0;
#endif

extern int reverse_spll;

void helper_very_init( struct spll_helper_state *s )
{
/* Phase branch PI controller */
	s->pi.y_min = (5 << BOARD_SPLL_DIV_BITS);
	s->pi.y_max = (1 << BOARD_SPLL_DAC_BITS) - (5 << BOARD_SPLL_DIV_BITS);
#if defined(CONFIG_WR_NODE)
	s->pi.kp = -150;
	s->pi.ki = -2;
#elif defined(CONFIG_TARGET_WR_SWITCH)
	static int init = 1;
	if (init) { /* Avoid overwriting pi values when e.g change timing mode */
		s->pi.kp = helper_pll_kp;
		s->pi.ki = helper_pll_ki;
		if (lj_periph_type_global == PERIPH_WRS_FL_SYNCTECH) {
			/* SyncTech */
			if (s->pi.kp == 0) /* 0 means not changed at load */
				s->pi.kp = HPLL_LJD_KP_SYNCTECH;
			if (s->pi.ki == 0) /* 0 means not changed at load */
				s->pi.ki = HPLL_LJD_KI_SYNCTECH;
		} else {
			/* Other versions */
			if (s->pi.kp == 0) /* 0 means not changed at load */
				s->pi.kp = HPLL_KP_DEFAULT;
			if (s->pi.ki == 0) /* 0 means not changed at load */
				s->pi.ki = HPLL_KI_DEFAULT;
		}
	}
	init = 0;
#else
#error "Please set CONFIG for wr switch or wr node"
#endif
	s->pi.shift = PI_FRACBITS - BOARD_SPLL_DIV_BITS;
	s->pi.anti_windup = 1;

	/* Phase branch lock detection */
	s->ld.threshold = 200;
	s->ld.lock_samples = 10000;
	s->ld.delock_samples = 100;
}

void helper_init(struct spll_helper_state *s, int ref_channel)
{
	s->ref_src = ref_channel;
}

void helper_update(struct spll_helper_state *s, int tag, int source)
{
#if defined(CONFIG_IGNORE_HPLL)
	s->ld.lock_changed = 1;
	s->ld.locked = 1;
#else
	int err, y;

	/* Helper pll tracks the ref clock */
	if (source != s->ref_src)
		return;
	
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_TAG, tag, 0);
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_REF, s->p_setpoint, 0);

	if (s->tag_d0 < 0) {
		/* First tag. */
		s->p_setpoint = tag;
		s->tag_d0 = tag;

		return;
	}

	/* Handle tag wraparound */
	if (s->tag_d0 > tag)
		s->p_adder += (1 << TAG_BITS);

	/* Compute the error */
	err = (tag + s->p_adder) - s->p_setpoint;

	/* And clamp */
	if (HELPER_ERROR_CLAMP) {
		if (err < -HELPER_ERROR_CLAMP)
			err = -HELPER_ERROR_CLAMP;
		if (err > HELPER_ERROR_CLAMP)
			err = HELPER_ERROR_CLAMP;
	}

	/* Handle wraparound */
	if ((tag + s->p_adder) > HELPER_TAG_WRAPAROUND
	    && s->p_setpoint > HELPER_TAG_WRAPAROUND) {
		s->p_adder -= HELPER_TAG_WRAPAROUND;
		s->p_setpoint -= HELPER_TAG_WRAPAROUND;
	}

	/* The next expected tag is the current plus one cycle */
	s->p_setpoint += (1 << HPLL_N);
	s->tag_d0 = tag;

	y = pi_update((spll_pi_t *)&s->pi, err);
	spll_write_helper_dac(y);

	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_TIME_MS, timer_get_tics(), 0);
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_SAMPLE_ID, s->sample_n++, 0);
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_Y, y, 0);
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_ERR, err, 1);

	ld_update((spll_lock_det_t *)&s->ld, err);

	if( s->ld.lock_changed && s->ld.locked )
	{
		s->last_lock_duration_ms = timer_get_tics() - s->lock_start_ms;
	}
#endif
}

void helper_start(struct spll_helper_state *s)
{
	/* Set the bias to the upper end of tuning range. This is to ensure that
	   the HPLL will always lock on positive frequency offset. */
	if (!reverse_spll)
		s->pi.bias = s->pi.y_max;
	else
		s->pi.bias = s->pi.y_min;

	s->p_setpoint = 0;
	s->p_adder = 0;
	s->sample_n = 0;
	s->tag_d0 = -1;
	s->last_lock_duration_ms = -1;

	pi_init((spll_pi_t *)&s->pi);
	ld_init((spll_lock_det_t *)&s->ld);

	s->lock_start_ms = timer_get_tics();

	spll_enable_tagger(s->ref_src, 1);
	//spll_debug(SPLL_DBG_SRC_HELPER, SPLL_DBG_SIGNAL_EVENT, SPLL_DBG_EVT_START, 1);
}

void helper_switch_reference(struct spll_helper_state *s, int new_ref)
{
#if 0
	disable_irq();
	s->ref_src = new_ref;
	s->tag_d0 = -1;
	s->p_adder = 0;
	enable_irq();
	spll_enable_tagger(s->ref_src, 1);
#endif
}
