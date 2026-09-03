/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2013 CERN (www.cern.ch)
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include "wrc.h"
#include "shell.h"
#include "storage.h"
#include "dev/endpoint.h"
#include "ppsi/ppsi.h"
#include "cmds.h"

extern struct pp_globals *ppg;

/**
 * @shellcommand devmem
 * @shellusage devmem <address>
 * Reads a 32-bit word from memory or an FPGA register.
 *
 * @shellusage devmem <address> <value>
 * Writes a 32-bit word to memory or an FPGA register.
 */
int cmd_devmem(const char *args[])
{
	uint32_t *addr, value;

	if (!args[0]) {
		pp_printf("devmem: use: \"devmem <address> [<value>]\"\n");
		return 0;
	}
	fromhex(args[0], (void *)&addr);
	if (args[1]) {
		fromhex(args[1], (void *)&value);
		*addr = value;
	} else {
		pp_printf("%08x = %08x\n", (int) addr, (unsigned int) *addr);
	}
	return 0;
}

/**
 * @shellcommand delays
 * @shellusage delays
 * Displays the configured frame transmission delays.
 *
 * @shellusage delays <tx> <rx>
 * Delays are expressed in picoseconds. This command changes rx and tx
 * deltas for the currently matched SFP and both, delta_rxm and delta_txm. For
 * master mode, command has to be executed before the peer enters CALIBRATED
 * state. Otherwise it makes no effect on slave side. For slave mode, if the command
 * is executed before CALIBRATED is reached, then both slave deltas are changed
 * delta_rxs (with added bitslide) and delta_txs. After the state CALIBRATED is
 * reached, at every executrion of delays command both master deltas (delta_rxm
 * and delta_txm) are be updated in realtime. If the SFP match is performed (e.g.
 * link restart) the delays command has to be executed once more.
 */
int cmd_delays(const char *args[])
{
	wrh_servo_t * wr_servo;
	wr_servo_ext_t * wr_servo_ext = NULL;

	int tx, rx;

	if (!ppg || !ppg->pp_instances)
		return -1;
	wr_servo = (ppg->pp_instances->protocol_extension == PPSI_EXT_WR
		    && ppg->pp_instances->extState == PP_EXSTATE_ACTIVE) ?
			(wrh_servo_t*) ppg->pp_instances->ext_data : NULL;
	if (!wr_servo) {
		return -2;
	}
	wr_servo_ext = &((struct wr_data *)wr_servo)->servo_ext;

	if (args[0] && !args[1]) {
		pp_printf("delays: use: \"delays [<txdelay> <rxdelay>]\"\n");
		return 0;
	}
	if (args[1]) {
		tx = atoi(args[0]);
		rx = atoi(args[1]);
		/* Overwrite SFP parameters, in case PPSI is restarted */
		sfp_info.sfp_params.dTx = tx;
		sfp_info.sfp_params.dRx = rx;
		/* Change the active value too (add bislide here) */
		picos_to_pp_time(rx, &wr_servo_ext->delta_rxm);
		picos_to_pp_time(tx, &wr_servo_ext->delta_txm);
	} else {
		pp_printf("tx: %i   rx: %i\n", (int) sfp_info.sfp_params.dTx,
			  (int) sfp_info.sfp_params.dRx);
	}
	return 0;
}
