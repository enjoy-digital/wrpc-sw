/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "wrc.h"

#include "shell.h"
#include "dev/syscon.h"
#include "dev/endpoint.h"
#include "hw/endpoint_regs.h"
#include "hw/ep_mdio_regs.h"
#include "hw/lpdc_mdio_regs.h"
#include "cmds.h"

struct reg_desc {
	const char *name;
	unsigned addr;
};

static const struct reg_desc ep_regs[] = {
	{ "msr", EP_MDIO_MSR },
	{ "mcr", EP_MDIO_MCR },
	{ "lpdc-ctrl", EP_MDIO_PHY_SPECIFIC_REGS + LPDC_MDIO_CTRL },
	{ "lpdc-stat", EP_MDIO_PHY_SPECIFIC_REGS + LPDC_MDIO_STAT },
	{ NULL, 0 }
};

static const char * const ep_cmds[] =
{
	 [0] = "link",
	 [1] = "rd",
	 [2] = "autoneg",
	 [3] = "stat",
	 [4] = "tx",
	 [5] = "reset",
	 [6] = "pd",
	 [7] = "en",
};

/**
 * @shellcommand ep
 * @shellusage ep link
 * Displays Ethernet link status.
 *
 * @shellusage ep rd <register>
 * Reads one of the endpoint registers msr, mcr, lpdc-ctrl, or lpdc-stat.
 *
 * @shellusage ep autoneg <restart|off|on>
 * Controls endpoint autonegotiation.
 *
 * @shellusage ep stat
 * Displays endpoint status.
 *
 * @shellusage ep tx [0|1]
 * Enables or disables SFP transmission. With no value, transmission is
 * enabled.
 *
 * @shellusage ep reset
 * Resets the endpoint.
 *
 * @shellusage ep pd
 * Puts the endpoint into powerdown; software may not observe the loss of
 * synchronization.
 *
 * @shellusage ep en [0|1]
 * Enables or disables the endpoint. With no value, it enables the endpoint
 * and brings it out of reset.
 */
int cmd_ep(const char *args[])
{
	struct wr_endpoint_device* dev = &wrc_endpoint_dev;
	int icmd;

	icmd = sub_cmd(ep_cmds, ARRAY_SIZE(ep_cmds), args);

	switch (icmd) {
	case 0:
		/* "link" status */
		pp_printf("%d\n", ep_link_up(dev, NULL));
		return 0;
	case 1:	{
		/* "rd" read a register */
		const struct reg_desc *r;
		for (r = ep_regs; r->name; r++)
			if (!strcmp(r->name, args[1]))
				break;
		if (r->name == NULL)
			return -1;
		pp_printf("%s (@%x): %04x\n",
			  r->name, r->addr, ep_pcs_read(dev, r->addr));
		return 0;
	}
	case 2: {
		/* "autoneg" */
		unsigned mcr = ep_pcs_read(dev, EP_MDIO_MCR);
		if (!strcmp("restart", args[1])) {
			mcr |= EP_MDIO_MCR_ANRESTART;
		}
		else if (!strcmp("off", args[1])) {
			mcr &= ~EP_MDIO_MCR_ANENABLE;
		}
		else if (!strcmp("on", args[1])) {
			mcr |= EP_MDIO_MCR_ANENABLE;
		}
		ep_pcs_write(dev, EP_MDIO_MCR, mcr);
		return 0;
	}
	case 3: {
		/* "stat": endpoint status */
		uint32_t dsr = ep_read(dev, EP_REG_DSR);
		pp_printf("ready:   %u\n", (dsr & EP_DSR_GTREADY));
		pp_printf("rx sync: %u\n", (dsr & EP_DSR_RXSYNC));
		pp_printf("link ok: %u\n", (dsr & EP_DSR_LSTATUS));
		return 0;
	}
	case 4: {
		/* "tx 0/1": enable SFP tx (laser) */
		unsigned en;
		en =  (args[1] == NULL || atoi(args[1]) != 0);
		ep_sfp_enable(dev, en);
		return 0;
	}
	case 5:
		/* First reset the endpoint to unsynchronize */
		ep_pcs_write(dev, EP_MDIO_MCR, EP_MDIO_MCR_RESET);
		usleep(10);
		/* Then the GT.  This will also stop the clocks */
		ep_pcs_write(dev, EP_MDIO_MCR, EP_MDIO_MCR_PDOWN);
		return 0;
	case 6:
		/* "pd": drives reset and powerdown */
		ep_write(dev, EP_REG_ECR, 0);
		ep_pcs_write(dev, EP_MDIO_MCR,
			     EP_MDIO_MCR_PDOWN | EP_MDIO_MCR_RESET);
		return 0;
	case 7:	{
		/* Enable of disable the endpoint */
		unsigned en;
		en = (args[1] == NULL || atoi(args[1]) != 0);
		ep_enable(dev, en, 1);
		return 0;
	}
	default:
		return -1;
	}
}
