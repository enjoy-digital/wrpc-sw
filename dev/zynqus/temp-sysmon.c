///////////////////////////////////////////////////////////////////////////////
// SPDX-FileCopyrightText: 2026 CERN (home.cern)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
///////////////////////////////////////////////////////////////////////////////
/*
 * This work is part of the White Rabbit project
 *
 * Temperature sensors for ZynqUS+
 */
#include "wrc.h"
#include "dev/temperature.h"
#include "dev/zynqus/plsysmon.h"
#include "dev/zynqus/pssysmon.h"
#include "dev/zynqus/temp-sysmon.h"

static volatile struct plsysmon * const plsysmon = ZYNQUS_PLSYSMON_REGS;
static volatile struct pssysmon * const pssysmon = ZYNQUS_PSSYSMON_REGS;


static struct wrc_temp_sensor temp_sysmon_data[] = {
	{"pl", TEMP_INVALID},
	{"lpd", TEMP_INVALID},
	{"fpd", TEMP_INVALID},
	{NULL,}
};

static unsigned long nextt;

static int32_t conv_temp(uint32_t v)
{
	int32_t sv;

	v &= 0xffff;

	/* Cf ug580 p41 */
	v = (v * 50931) >> 16;

	sv = v - 28023;

	/* Convert from /100 C to /(1<<16) C */
	return (sv << 16) / 100;
}

/* Returns 1 if it did something */
static int temp_zynqus_sysmon_refresh(struct wrc_temp_sensor *t)
{
	if (time_before(timer_get_tics(), nextt))
		return 0;

	nextt += 1000 * CONFIG_TEMP_POLL_INTERVAL;

	temp_sysmon_data[0].t = conv_temp(plsysmon->temperature);
	temp_sysmon_data[1].t = conv_temp(pssysmon->temp_lpd);
	temp_sysmon_data[2].t = conv_temp(pssysmon->temp_fpd);

	return 1;
}

void temp_zynqus_sysmon_init(void)
{
	struct wrc_temp_group gr;

	nextt = timer_get_tics() + 1000;

	/* Loop sequence, with temperatures */
	pssysmon->seq_channel2 = 0x003f;
	pssysmon->seq_channel0 = 0x47e0;
	pssysmon->config_reg1 = 0x2f0f;

	gr.read = temp_zynqus_sysmon_refresh;
	gr.t = temp_sysmon_data;
	wrc_temp_register(&gr);
}
