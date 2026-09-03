/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
/* Command: sfp
 * Arguments: subcommand [subcommand-specific args]
 *
 * Description: SFP detection/database manipulation.
 * Subcommands:
 * add <product_number> <delta_tx> <delta_rx> <alpha> - adds an SFP to
 *                      the database, with given alpha/delta_rx/delta_tx values
 * show - shows the SFP database
 * match - detects the transceiver type and tries to get calibration parameters
 *         from DB for a detected SFP
 * erase - cleans the SFP database
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "wrc.h"

#include "shell.h"
#include "storage.h"
#include "dev/syscon.h"
#include "dev/endpoint.h"
#include "cmds.h"

#include "sfp.h"
#include "libwr/sfp_lib.h"

#ifdef CONFIG_CMD_SFP_TUNABLE
#define HAS_CMD_SFP_TUNABLE 1
#else
#define HAS_CMD_SFP_TUNABLE 0
#endif

#ifdef CONFIG_CMD_SFP_INFO
static void print_info(void)
{
	uint16_t tmp;
	struct shw_sfp_header *sfp_header;

	sfp_header = sfp_info.sfp_header;
	/* to save the code, print only the most important parameters */
	pp_printf("Nominal Bit Rate: %d Mbits/s\n", sfp_header->br_nom * 100);
	pp_printf("Vendor Name: %.16s\n", sfp_header->vendor_name);
	pp_printf("Vendor PN: %.16s\n", sfp_header->vendor_pn);
	pp_printf("Vendor serial: %.16s\n", sfp_header->vendor_serial);
	pp_printf("TX Wavelength: %d\n", (sfp_header->tx_wavelength[0] << 8)
					 + sfp_header->tx_wavelength[1]);

	if (HAS_SFP_DOM) {
		struct shw_sfp_dom *sfp_dom;
		if (!(sfp_header->diagnostic_monitoring_type & SFP_DIAG_IMPLEMENTED)){
			/* no DOM supported */
			pp_printf("No DOM support\n");
			return;
		}

		sfp_dom = sfp_info.sfp_dom;
		tmp = (sfp_dom->temp[0] << 8) + sfp_dom->temp[1];
		pp_printf("Temperature: %d.%02d C\n", tmp/256, ((tmp*100)/256)%100);
		tmp = (sfp_dom->vcc[0] << 8) + sfp_dom->vcc[1];
		pp_printf("Voltage: %d.%04d V\n", tmp/10000, tmp%10000);
		tmp = ((sfp_dom->tx_bias[0] << 8) + sfp_dom->tx_bias[1])/5;
		pp_printf("Bias Current: %d.%02d mA\n", tmp / 100, tmp % 100);
		tmp = ((sfp_dom->tx_pow[0] << 8) + sfp_dom->tx_pow[1]);
		pp_printf("TX power: %d.%04d mW\n", tmp / 10000, tmp % 10000);
		tmp = ((sfp_dom->rx_pow[0] << 8) + sfp_dom->rx_pow[1]);
		pp_printf("RX power: %d.%04d mW\n", tmp / 10000, tmp % 10000);
	}

	if (HAS_CMD_SFP_TUNABLE) {
		uint8_t tmp8;
		uint16_t tmp16;
		uint32_t first_freq_in_01;
		uint32_t last_freq_in_01;

		pp_printf("SFP tunable: %s\n",
			  sfp_header->options[1] & SFP_OPTION_TUNABLE
				? "true" : "false");
		sfp_a2_select_page(SFP_A2_PAGE_CONTROL_FUNC);
		tmp8 = sfp_a2_read_u8(SFP_A2_CTRL_DITHERING_REG);
		pp_printf("Dither TX: %s\n",
			  tmp8 & SFP_DITHER_TX ? "true" : "false");
		pp_printf("Tunable by channel: %s\n",
			  tmp8 & SFP_TUNABLE_CHANNEL ? "true" : "false");
		pp_printf("Tunable by wavelength: %s\n",
			  tmp8 & SFP_TUNABLE_WAVELENGTH ? "true" : "false");
		first_freq_in_01 = sfp_a2_read_u16(SFP_A2_CTRL_LFL1_REG) * 10000
				   + sfp_a2_read_u16(SFP_A2_CTRL_LFL2_REG);
		pp_printf("Laser first freq: %"PRIu32".%04"PRIu32" THz\n",
			  first_freq_in_01 / 10000,
			  first_freq_in_01 % 10000);
		last_freq_in_01 = sfp_a2_read_u16(SFP_A2_CTRL_LFH1_REG) * 10000
				   + sfp_a2_read_u16(SFP_A2_CTRL_LFH2_REG);
		pp_printf("Laser last freq: %"PRIu32".%04"PRIu32" THz\n",
			  last_freq_in_01 / 10000,
			  last_freq_in_01 % 10000);
		tmp16 = sfp_a2_read_u16(SFP_A2_CTRL_LGRID_REG);
		pp_printf("Laser's grid support: %d.%d GHz\n",
			  tmp16 / 10, tmp16 % 10);
		pp_printf("Channels supported: 1..%"PRIu32"\n",
			  1 + (last_freq_in_01 - first_freq_in_01) / tmp16);
		tmp16 = sfp_a2_read_u16(SFP_A2_CTRL_CHNO_SET_REG);
		pp_printf("Current channel number: %d\n", tmp16);
		tmp16 = sfp_a2_read_u16(SFP_A2_CTRL_WL_SET_REG);
		pp_printf("Current wavelength: %d.%02d nm\n", tmp16 / 20,
			  (tmp16 % 20) * 5);
	}
}
#endif /* CONFIG_CMD_SFP_INFO */

static const char * const sfp_cmds[] =
{
	 [0] = "erase",
	 [1] = "add",
	 [2] = "show",
	 [3] = "match",
	 [4] = "ena",
#ifdef CONFIG_CMD_SFP_INFO
	 [5] = "info",
#endif
#ifdef CONFIG_CMD_SFP_TUNABLE
	 [6] = "tune",
#endif
};

/**
 * @shellcommand sfp
 * @shellusage sfp erase
 * Erases the SFP calibration database.
 *
 * @shellusage sfp add <PN> <deltaTx> <deltaRx> <alphaH> <alphaL>
 * Stores calibration parameters for an SFP. alphaH and alphaL are the higher
 * and lower nine decimal digits of alpha; alphaH carries the sign.
 *
 * @shellusage sfp show
 * Displays all SFPs stored in the database.
 *
 * @shellusage sfp match [force]
 * Prints the plugged SFP identifier, then matches it and loads its calibration
 * parameters. force requests a forced match.
 *
 * @shellusage sfp ena <0|1>
 * Enables or disables SFP transmission.
 *
 * @shellusage sfp info
 * Displays detailed information about the plugged SFP, including DOM data when
 * supported by the SFP and build configuration.
 *
 * @shellusage sfp tune <channel|wavelength>
 * Tunes a tunable SFP by SFP channel, not ITU channel, or by wavelength in nm.
 */
int cmd_sfp(const char *args[])
{
	int icmd;

	icmd = sub_cmd(sfp_cmds, ARRAY_SIZE(sfp_cmds), args);

	switch (icmd) {
	case 0:
		if (storage_sfpdb_erase() == EE_RET_I2CERR) {
			pp_printf("Could not erase DB\n");
			return -EIO;
		}
		return 0;
	case 1:
	{
		unsigned temp, i;
		struct s_sfpinfo sfp;

		if (!args[5])
			return -1;
		temp = strnlen(args[1], SFP_PN_LEN);
		for (i = 0; i < temp; ++i)
			sfp.pn[i] = args[1][i];
		while (i < SFP_PN_LEN)
			sfp.pn[i++] = ' ';	//padding
		sfp.dTx = atoi(args[2]);
		sfp.dRx = atoi(args[3]);
		sfp.alpha = atoi(args[4]);
		sfp.alpha = sfp.alpha * 1000000000;
		/* first value defines a sign */
		sfp.alpha += (sfp.alpha < 0?-1:1) * atoi(args[5]);
		temp = storage_get_sfp(&sfp, SFP_ADD, 0);
		if (temp == EE_RET_DBFULL) {
			pp_printf("SFP DB is full\n");
			return -ENOSPC;
		} else if (temp == EE_RET_I2CERR) {
			pp_printf("I2C error\n");
			return -EIO;
		} else if (temp < 0) {
			pp_printf("SFP database error (%d)\n", temp);
			return -EFAULT;
		}
		pp_printf("%d SFPs in DB\n", temp);
		return 0;
	}
	case 2:
	{
		unsigned i, temp;
		struct s_sfpinfo sfp;
		int sfpcount = 1;

		for (i = 0; i < sfpcount; ++i) {
			sfpcount = storage_get_sfp(&sfp, SFP_GET, i);
			if (sfpcount == 0) {
				pp_printf("SFP database empty\n");
				return 0;
			} else if (sfpcount < 0) {
				pp_printf("SFP database error (%d)\n",
					  sfpcount);
				return -EFAULT;
			}
			pp_printf("%d: PN:", i + 1);
			for (temp = 0; temp < SFP_PN_LEN; ++temp)
				pp_printf("%c", sfp.pn[temp]);
			pp_printf(" dTx: %8d dRx: %8d alpha: %19Ld\n",
				  (int) sfp.dTx, (int) sfp.dRx, sfp.alpha);
		}
		return 0;
	}
	case 3:
	{
		int ret;

		if (args[1] && !strcmp(args[1], "force")) {
			ret = sfp_match(1);
		} else {
			ret = sfp_match(0);
		}
		if (ret == -ENODEV) {
			pp_printf("No SFP.\n");
			return ret;
		}
		if (ret == -EIO) {
			pp_printf("SFP read error\n");
			return ret;
		}

		/* SFP read correctly */
		pp_printf("%.16s\n", sfp_info.sfp_params.pn);

		if (ret == -ENXIO) {
			pp_printf("Could not match to DB\n");
			return ret;
		}
		/* match successful */
		pp_printf("SFP matched, dTx=%d dRx=%d alpha=%Ld\n",
			  (int) sfp_info.sfp_params.dTx,
			  (int) sfp_info.sfp_params.dRx,
			  sfp_info.sfp_params.alpha);
		return ret;
	}
	case 4:
		if (!args[1])
			return -1;
		ep_sfp_enable(&wrc_endpoint_dev, atoi(args[1]));
		return 0;
#ifdef CONFIG_CMD_SFP_INFO
	case 5:
		/* DOM data is updated periodically by a task */
		print_info();
		return 0;
#endif
#ifdef CONFIG_CMD_SFP_TUNABLE
	case 6:
		struct shw_sfp_header *sfp_header;
		sfp_header = sfp_info.sfp_header;
		if (!(sfp_header->options[1] & SFP_OPTION_TUNABLE)) {
			pp_printf("SFP not tunable!\n");
			return -ENODEV;
		}
		if (!args[1]) {
		    pp_printf("tune args: %d\n", atoi(args[1]));
		    return -EINVAL;
		}
		sfp_tune_ch(args[1]);
		return 0;
#endif
	default:
		return -1;
	}
}
