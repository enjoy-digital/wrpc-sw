/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* spll_hw.c - low-level layer to the HW for the SoftPLL */

#include <wrc.h>
#include "softpll_ng.h"
#include "spll_hw.h"
#include "irq.h"

#include "hw/softpll_regs.h"
#include "hw/pps_gen_regs.h"

#define SPLL ((volatile struct SPLL_WB*) (BASE_SOFTPLL))
#define PPSG ((volatile struct PPSG_WB*) (BASE_PPS_GEN))


/* Enables/disables DDMTD tag generation on a given (channel).

Channels (0 ... splL_n_chan_ref - 1) are the reference channels
	(e.g. transceivers' RX clocks or a local reference)

Channels (spll_n_chan_ref ... spll_n_chan_out + spll_n_chan_ref-1) are the output
	channels (local voltage controlled oscillators). One output
	(usually the first one) is always used to drive the oscillator
	which produces the reference clock for the transceiver. Other
	outputs can be used to discipline external oscillators
	(e.g. on FMCs).
*/

void spll_enable_tagger(int channel, int enable)
{
	if (channel >= spll_n_chan_ref) {
		/* Output channel? */
		if (enable)
			SPLL->OCER |= 1 << (channel - spll_n_chan_ref);
		else
			SPLL->OCER &= ~(1 << (channel - spll_n_chan_ref));
	} else {
		/* Reference channel */
		if (enable)
			SPLL->RCER |= 1 << channel;
		else
			SPLL->RCER &= ~(1 << channel);
	}

	pll_verbose("%s: ch %d, OCER 0x%x, RCER 0x%x\n", __FUNCTION__, channel, SPLL->OCER, SPLL->RCER);
}

void spll_debug_push(unsigned w)
{
	/* Push to HW fifo */
	SPLL->DFR_SPLL = w;
}

void spll_write_dac(unsigned idx, unsigned y)
{
	SPLL->DAC_MAIN =
		SPLL_DAC_MAIN_VALUE_W(y) | SPLL_DAC_MAIN_DAC_SEL_W(idx);
}

void spll_write_helper_dac(unsigned y)
{
	SPLL->DAC_HPLL = y;
}

void spll_ppsg_init(void)
{
	PPSG->ESCR = 0;
	PPSG->CR = PPSG_CR_CNT_EN | PPSG_CR_CNT_RST | PPSG_CR_PWIDTH_W(PPS_WIDTH);
}

void spll_regs_init(void)
{
	SPLL->OCER = 0;
	SPLL->RCER = 0;
	SPLL->ECCR = 0;
	SPLL->EIC_IDR = 1;

	SPLL->DAC_HPLL = 0;

	SPLL->CSR = 0;
	SPLL->OCER = 0;
	SPLL->RCER = 0;
	SPLL->ECCR = 0;
	SPLL->OCCR = 0;
#ifndef CONFIG_SPLL_DEGLITCH_THR
#define CONFIG_SPLL_DEGLITCH_THR 1000
#endif
	SPLL->DEGLITCH_THR = CONFIG_SPLL_DEGLITCH_THR;

	PPSG->CR |= PPSG_CR_CNT_EN;
}

void spll_init_n_chan(void)
{
	uint32_t csr = SPLL->CSR;

	spll_n_chan_ref = SPLL_CSR_N_REF_R(csr);
	spll_n_chan_out = SPLL_CSR_N_OUT_R(csr);

	if( spll_n_chan_out > 3 ) // fixme: bug in HDL?
		spll_n_chan_out = 3;
}

void spll_set_channel_status(int channel, int locked)
{
	if(!locked)
		SPLL->OCCR &= ~(SPLL_OCCR_OUT_LOCK_W((1 << channel)));
	else
		SPLL->OCCR |= (SPLL_OCCR_OUT_LOCK_W((1 << channel)));
}

int spll_aux_locking_enabled(int channel)
{
	uint32_t occr_aux_en = SPLL_OCCR_OUT_EN_R(SPLL->OCCR);

	return occr_aux_en & (1 << channel);
}

int spll_is_ext_supported(void)
{
	return (SPLL->ECCR & SPLL_ECCR_EXT_SUPPORTED) ? 1 : 0;
}

void spll_purge_tags(void)
{
	/* Purge tag buffer */
	while (!(SPLL->TRR_CSR & SPLL_TRR_CSR_EMPTY))
	{
		unsigned dummy = SPLL->TRR_R0;
		(void) dummy;
	}
}

void spll_regs_enable(void)
{
	SPLL->EIC_IER = 1;
	SPLL->OCER |= 1;
}

void spll_irq_entry(void)
{
	/* check if there are more tags in the FIFO, and log them if so configured to */
	while (!(SPLL->TRR_CSR & SPLL_TRR_CSR_EMPTY)) {
		uint32_t trr;
		int tag_source, tag_value;

		trr = SPLL->TRR_R0;

		/* And process the values */
		tag_source = SPLL_TRR_R0_CHAN_ID_R(trr);
		tag_value  = SPLL_TRR_R0_VALUE_R(trr);

		spll_handle_tags(tag_source, tag_value);
	}

	softpll.irq_count++;
	clear_irq();
}
