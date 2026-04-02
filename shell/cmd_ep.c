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
#include "hw/ep_mdio_regs.h"
#include "hw/lpdc_mdio_regs.h"
#include "cmds.h"

struct reg_desc {
	const char *name;
	unsigned addr;
};

static const struct reg_desc regs[] = {
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
};

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
		for (r = regs; r->name; r++)
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
	default:
		return -1;
	}
}
