/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* spll_hw.h - low-level layer to the HW for the SoftPLL */

#ifndef __SPLL_HW_H
#define __SPLL_HW_H

void spll_enable_tagger(int channel, int enable);
void spll_debug_push(unsigned w);

void spll_ppsg_init(void);

/* Read n_chan_ref and n_chain_out from hw. */
void spll_init_n_chan(void);

/* Set or clear out_locked_o bit for channel */
void spll_set_channel_status(int channel, int locked);

/* True if external clock input is supported (for GM mode) */
int spll_is_ext_supported(void);

/* Return True if aux channel is locked (clk_ext_mul_locked_i) */
int spll_aux_locking_enabled(int channel);

/* Reset HW registers */
void spll_regs_init(void);

/* Set HW regs to enable spll tags for the main channel */
void spll_regs_enable(void);

/* Write value Y to DAC IDX (which is the output index, so 0 for the first
   output channel */
void spll_write_dac(unsigned idx, unsigned y);

/* Write value Y to the helper DAC */
void spll_write_helper_dac(unsigned y);

/* Purge tag buffer */
void spll_purge_tags(void);

#endif // __SPLL_HW_H
