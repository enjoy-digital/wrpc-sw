/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* Hardware layer for spll tests */

#include <stdarg.h>
#include <stdio.h>
#include "softpll/softpll_ng.h"
#include "softpll/spll_hw.h"
#include "hw.h"

void spll_ppsg_init(void)
{
}

void spll_init_n_chan(void)
{
	spll_n_chan_ref = 1;
	spll_n_chan_out = 1;
}

unsigned out_locked;

void spll_set_channel_status(int channel, int locked)
{
	if (!locked)
		out_locked &= ~(1 << channel);
	else
		out_locked |= (1 << channel);
}

int spll_is_ext_supported(void)
{
	return 0;
}

int spll_aux_locking_enabled(int channel)
{
	return 0;
}

void spll_regs_init(void)
{
}

void spll_regs_enable(void)
{
}

void spll_purge_tags(void)
{
	/* No tag buffer, so nothing to purge */
}

void external_init(volatile struct spll_external_state *s, int ext_ref)
{
}

void external_start(struct spll_external_state *s)
{
}

int external_locked(volatile struct spll_external_state *s)
{
	return 0;
}

int external_align_fsm(volatile struct spll_external_state *s)
{
	return 0;
}

unsigned long tics;

unsigned long timer_get_tics(void)
{
	return tics;
}

unsigned out_chan_en, ref_chan_en;

void spll_enable_tagger(int channel, int enable)
{
	if (channel >= spll_n_chan_ref) {
		/* Output channel */
		if (enable)
			out_chan_en |= 1 << (channel - spll_n_chan_ref);
		else
			out_chan_en &= ~(1 << (channel - spll_n_chan_ref));
	} else {
		/* Reference channel */
		if (enable)
			ref_chan_en |= 1 << channel;
		else
			ref_chan_en &= ~(1 << channel);
	}
}

unsigned out_y, helper_y;

void spll_write_dac(unsigned idx, unsigned y)
{
	switch (idx) {
	case 0:
		out_y = y;
		break;
	default:
		printf("spll_hw_set_dac: unhandled index %u\n", idx);
		break;
	}
}

void spll_write_helper_dac(unsigned y)
{
	helper_y = y;
}

unsigned dbg_dtag, dbg_dref;
signed dbg_err;

void spll_debug_push(unsigned w)
{
	if (SPLL_DBG_EXTRACT_SOURCE(w) == ID_OUT) {
		unsigned val = SPLL_DBG_EXTRACT_VALUE(w);
		signed eval = ((signed)(val << 8)) >> 8;
		switch (SPLL_DBG_EXTRACT_SIGNAL(w)) {
		case SPLL_DBG_SIGNAL_ERR:
			dbg_err = eval;
			break;
		case SPLL_DBG_SIGNAL_TAG:
			dbg_dtag = val;
			break;
		case SPLL_DBG_SIGNAL_REF:
			dbg_dref = val;
			break;
		default:
			break;
		}
	}
}

void disable_irq(void)
{
}

void enable_irq(void)
{
}

int pp_printf(const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vprintf(fmt, args);
	va_end(args);

	return ret;
}

/* Phase of out, as measured by ddmtd, so on TAG_BITS */
unsigned phase_out, phase_ref;

