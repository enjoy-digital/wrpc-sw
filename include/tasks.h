#ifndef DEF_TASK
#define DEF_TASK(NAME, INIT, JOB, ENABLED) \
  extern void INIT(void); \
  extern int JOB(void); \
  extern int ENABLED(void);
#endif

DEF_TASK("idle",	NO_INIT, NO_JOB, NO_ENABLED)

DEF_TASK("check-link",	NO_INIT, wrc_check_link, NO_ENABLED)
DEF_TASK("uptime",	init_uptime, update_uptime, NO_ENABLED)
DEF_TASK("ptp",		NO_INIT, wrc_ptp_update, NO_ENABLED)
DEF_TASK("ptp_bmc",	NO_INIT, wrc_ptp_bmc_update, NO_ENABLED)
DEF_TASK("shell+gui",	shell_boot_script, ui_update, NO_ENABLED)
DEF_TASK("spll-bh",	NO_INIT, spll_update, NO_ENABLED)

#ifdef CONFIG_TEMP_SENSORS
  DEF_TASK("temperature",	NO_INIT, wrc_temp_refresh, NO_ENABLED)
#endif

DEF_TASK("net-bh",	NO_INIT, net_bh_poll, is_link_up)

#ifdef CONFIG_DAC_LOG
DEF_TASK("dac-logger",	daclog_init, daclog_poll, NO_ENABLED)
#endif

#ifdef CONFIG_IP
  DEF_TASK("arp",	arp_init, arp_poll, is_link_up)

/* Run ipv4 even if link is down. ipv4_poll has to be executed even
 * on link down to trigger the bootp request when the link is up.
 * Needed by syslog to track link up */
  DEF_TASK("ipv4",	ipv4_init, ipv4_poll, NO_ENABLED)
#endif

#ifdef CONFIG_LATENCY_PROBE
DEF_TASK("latency-probe", latency_init, latency_poll, NO_ENABLED)
#endif

#ifdef CONFIG_LLDP
DEF_TASK( "lldp", lldp_init, lldp_poll, is_link_up )
#endif

#ifdef CONFIG_SNMP
DEF_TASK( "snmp", snmp_init, snmp_poll, is_link_up)
#endif

DEF_TASK( "stats", NO_INIT, wrc_log_stats, NO_ENABLED)

#ifdef CONFIG_WR_DIAG
DEF_TASK( "diags", NO_INIT, wrc_wr_diags, NO_ENABLED)
#endif

#ifdef CONFIG_NETCONSOLE
DEF_TASK( "netconsole", netconsole_init, netconsole_poll, is_link_up)
#endif

#ifdef CONFIG_SFP_DOM
/* Read DOM data from SFP even if the link is down or/and SFP
 * unplugged */
DEF_TASK("sfp_dom", NO_INIT, sfp_dom_update, NO_ENABLED)
#endif

#ifndef CONFIG_LPDC_NONE
DEF_TASK("phy-cal", phy_calibration_init, phy_calibration_poll, NO_ENABLED)
#endif

#ifdef CONFIG_AUX_TIMING_EN
DEF_TASK("timecode", NO_INIT, timecode_update, NO_ENABLED)
#endif

#ifdef CONFIG_GNSS
DEF_TASK("gnss", gnss_init, gnss_poll, NO_ENABLED)
#endif

#ifdef BOARD_TASKS
BOARD_TASKS
#endif

