/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2011,2012 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <inttypes.h>
#include <stdarg.h>

#include "wrc.h"
#include "dev/w1.h"
#include "dev/syscon.h"
#include "dev/console.h"
#include "dev/endpoint.h"
#include "dev/minic.h"
#include "dev/pps_gen.h"
#include "dev/gpio.h"
#include "dev/simple_uart.h"
#include "dev/netif.h"
#include "net.h"
#include "dev/i2c.h"
#include "storage.h"
#include "softpll_ng.h"
#include "dev/pps_gen.h"
#include "shell.h"
#include "lib/ipv4.h"
#include "lib/events-ptp.h"
#include "dev/rxts_calibrator.h"
#include "dev/gpio.h"
#include "netconsole.h"
#include "dev/wdiags.h"
#include "dev/timecode.h"
#include "dev/auxclk.h"
#include "dev/nmea_out.h"

#include "wrpc.h"
#include "system_checks.h"
#include "ppsi/ppsi.h"
#include "wrc_global.h"
#include "dev/w1.h"
#include "dev/temp-fake.h"
#include "dev/temp-w1.h"
#include "dev/temperature.h"
#include "sensors.h"
#include "tasks.h"

#include "board.h"
#include <hw/timecode_regs.h>

#ifdef CONFIG_DAC_LOG
#include "dev/dac_log.h"
#endif

#ifdef CONFIG_LATENCY_PROBE
#include "lib/latency.h"
#endif

#ifdef CONFIG_LLDP
#include "lib/lldp.h"
#endif

#ifndef CONFIG_LPDC_NONE
#include "lpdc.h"
#endif

struct wr_endpoint_device wrc_endpoint_dev;

int wrc_wr_diags(void); // fixme: move the header

extern char _binary__config_bin_start[];

struct wrc_global_link wrc_global_link = {
	.version = WRC_G_LINK_VERSION,
	.vlan = CONFIG_VLAN_NR,
	.ip_state = IP_TRAINING,
};

const struct wrc_global wrc_global = {
	.magic = WRC_G_MAGIC,
	.version = WRC_G_VERSION,
	.global_link = &wrc_global_link,
	.task_list_max = 0,
	.task_list = NULL,
	.temp_group_list_max = WRC_MAX_TEMPERATURES,
#ifdef CONFIG_TEMP_SENSORS
	.temp_group_list = temp_sensors,
#endif
	.softpll = &softpll,
#ifdef CONFIG_CMD_CONFIG
	.config = _binary__config_bin_start,
#endif
	.sfp_info = &sfp_info,
};

static void wrc_initialize(void)
{
	console_init();
	timer_init(1);
	spll_very_init();
	usleep_init();

	/* Initialize W1 before board, in case mac address is read from it. */
	if (HAS_W1)
		wrc_w1_init();

	wrc_board_early_init();

	wdiags_init();

	pp_printf("WR Core: starting up...\n");
	get_hw_name(wrc_global_link.wrc_hw_name);

#ifndef BOARD_HAS_CUSTOM_NETWORK_INIT
	net_rst();
	ep_init( &wrc_endpoint_dev, (void *) BASE_EP );
	netif_register_device(&wrc_endpoint_dev, &minic);
	/* Sleep for 1s to make sure WRS v4.2 always realizes that
	 * the link is down */
	timer_delay_ms(200);
	ep_enable( &wrc_endpoint_dev, 1, 1 );
#endif

	minic_init(&minic, (void *)BASE_MINIC);
	shw_pps_gen_init();

	/* initialize w1 temp sensor.  Note that w1 must have been initialized
	   by board. */
	if (HAS_TEMP_SENSORS && HAS_W1_TEMP)
		temp_w1_init();

	if (HAS_TEMP_SENSORS && HAS_TEMP_FAKE)
		temp_faketemp_init();


#if defined CONFIG_AUX_TIMING_EN

	#if defined(CONFIG_AUXCLK_EN)
    auxclk_init(CONFIG_AUXCLK_FREQ, CONFIG_AUXCLK_DUTY);
  #endif

  #if defined(CONFIG_NMEA_OUT_EN)
    struct timecode *wrc_timecode =  ((struct timecode *)(BASE_TIMECODE));
    #if defined(CONFIG_NMEA_OUT_INVERT)
    	nmea_out_init(&wrc_timecode->nmea, CONFIG_NMEA_OUT_BAUD, 1);
    #else
    	nmea_out_init(&wrc_timecode->nmea, CONFIG_NMEA_OUT_BAUD, 0);
    #endif
  #endif

  //mux setup
  #if defined(CONFIG_AUXTMG_SEL_CLK)
    timecode_sel(TIMECODE_SEL_CLK);
  #endif

  #if defined(CONFIG_AUXTMG_SEL_IRIG)
    timecode_sel(TIMECODE_SEL_IRIG);
  #endif

  #if defined(CONFIG_AUXTMG_SEL_NMEA)
    timecode_sel(TIMECODE_SEL_NMEA);
  #endif

#endif

	wrc_board_init();

	/* BSP didn't load the calibration parameters? go ahead */
	if (!storage_is_calibration_loaded())
		storage_load_calibration();

	wrc_ptp_init();
	/* try reading t24 phase transition from EEPROM */
	calib_t24p_load();
	shell_init();

	_endram = ENDRAM_MAGIC;

	wrc_ptp_set_mode(WRC_MODE_SLAVE);
	wrc_ptp_start();

	wrc_tasks_accounting_init();
}

int is_link_up(void)
{
	return link_status == NETIF_LINK_UP;
}

int wrc_check_link(void)
{
	static int prev_state = 0;
	int state = ep_link_up( &wrc_endpoint_dev, NULL);
	int rv = 0;

	if (!prev_state && state) {
		wrc_verbose("Link up.\n");
		event_post( WRC_EVENT_LINK_UP );
		sfp_match(0);
		wrc_ptp_start();
		link_status = NETIF_LINK_WENT_UP;
		rv = 1;
	} else if (prev_state && !state) {
		wrc_verbose("Link down.\n");
		wrc_events_ptp_link_down();
		event_post( WRC_EVENT_LINK_DOWN );
		link_status = NETIF_LINK_WENT_DOWN;
		wrc_ptp_stop();
		wrc_ptp_link_down();
		rv = 1;
	} else
		link_status = (state ? NETIF_LINK_UP : NETIF_LINK_DOWN);

	prev_state = state;

	return rv;
}

int ui_update(void)
{
	return shell_interactive();
}

/* initialize functions to be called after reset in check_reset function */
void init_hw_after_reset(void)
{
	/* Ok, now init the devices so we can printf and delay */
	console_init();
	timer_init(1);
}


/* count uptime, in seconds, for remote polling */
static uint32_t uptime_lastj;
uint32_t uptime_sec;

void init_uptime(void)
{
	uptime_lastj = timer_get_tics();
}

int update_uptime(void)
{
	uint32_t j;
	static uint32_t fraction = 0;

	j = timer_get_tics();
	fraction += j - uptime_lastj;
	uptime_lastj = j;
	if (fraction > TICS_PER_SECOND) {
		fraction -= TICS_PER_SECOND;
		uptime_sec++;
		return 1;
	}
	return 0;
}

int main(void) __attribute__ ((weak));
int main(void)
{
	check_reset();

	wrc_initialize();

	/* initialization of individual tasks */
	wrc_tasks_run_inits();

	for (;;) {
		// run all pending tasks
		wrc_poll_all_tasks();
		// call all event handlers
		if (BOARD_USE_EVENTS)
			events_dispatch();
		/* better safe than sorry */
		check_stack();
	}
}
