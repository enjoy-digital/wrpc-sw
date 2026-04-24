/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* Hardware layer for spll tests */

#define ID_REF 0
#define ID_OUT 1

extern unsigned out_locked;
extern unsigned long tics;
extern unsigned out_chan_en, ref_chan_en;
extern unsigned out_y, helper_y;
extern unsigned dbg_dtag, dbg_dref;
extern signed dbg_err;

extern unsigned phase_out, phase_ref;
