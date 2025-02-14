/*
 * This work is part of the White Rabbit project
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* This header file is special as it is also used to declare the
   array of command (see shell.c).  Thus:
   * no #include,
   * possibility to include many times
   Please #include "board.h" in order to add board specific commands.
*/

#ifndef WRC_COMMAND2
#define WRC_COMMAND2(name, func) extern int cmd_##func(const char *args[]);
#endif
#ifndef WRC_COMMAND
#define WRC_COMMAND(name) WRC_COMMAND2(name, name)
#endif

/* Commands are sorted, except for 'help' so that it is more visible */
WRC_COMMAND(help)

#ifdef CONFIG_CMD_AUXCLK
WRC_COMMAND(auxclk)
#endif
#ifdef CONFIG_CMD_AUXTMG
WRC_COMMAND(auxtmg)
#endif
WRC_COMMAND(calibration)
#ifdef CONFIG_CMD_CONFIG
  WRC_COMMAND(config)
#endif
#ifdef CONFIG_DAC_LOG
  WRC_COMMAND(daclog)
#endif
#ifdef CONFIG_CMD_LL
  WRC_COMMAND(delays)
  WRC_COMMAND(devmem)
#endif
#ifdef CONFIG_AUX_DIAG
  WRC_COMMAND(diag)
#endif
#ifdef CONFIG_CMD_EP
  WRC_COMMAND(ep)
#endif
#ifdef CONFIG_TEMP_FAKE
  WRC_COMMAND(faketemp)
#endif
#ifdef CONFIG_FREQUENCY_MONITOR
  WRC_COMMAND(freqmon)
#endif
#ifdef CONFIG_GNSS
  WRC_COMMAND(gnss)
#endif
WRC_COMMAND(gui)
WRC_COMMAND(init)
#ifdef CONFIG_IP
  WRC_COMMAND(ip)
#endif
#ifdef CONFIG_CMD_LEAPSEC
  WRC_COMMAND(leapsec)
#endif
#ifdef CONFIG_LATENCY_PROBE
  WRC_COMMAND(ltest)
#endif
WRC_COMMAND(mac)
WRC_COMMAND2(mode, ptp)
#ifdef CONFIG_CMD_NETCONSOLE
  WRC_COMMAND(netconsole)
#endif
#ifdef CONFIG_CMD_NMEA
  WRC_COMMAND(nmea)
#endif
WRC_COMMAND(pll)
#ifdef CONFIG_CMD_PPS
  WRC_COMMAND(pps)
#endif
WRC_COMMAND(ps)
WRC_COMMAND(ptp)
WRC_COMMAND(ptrack)
#ifdef CONFIG_CMD_REFRESH
  WRC_COMMAND(refresh)
#endif
WRC_COMMAND(sdb)
#ifdef CONFIG_GENERIC_SENSORS
  WRC_COMMAND(sensors)
#endif
WRC_COMMAND(sfp)
WRC_COMMAND(sleep)
WRC_COMMAND(stat)
#ifdef CONFIG_SYSLOG
  WRC_COMMAND(syslog)
#endif
#ifdef CONFIG_TEMP_SENSORS
  WRC_COMMAND(temp)
#endif
WRC_COMMAND(time)
WRC_COMMAND(uptime)
WRC_COMMAND(ver)
WRC_COMMAND(verbose)
#ifdef CONFIG_VLAN
  WRC_COMMAND(vlan)
#endif
#ifdef CONFIG_W1_TEMP
  WRC_COMMAND(w1)
#endif
#ifdef CONFIG_W1_EEPROM
  WRC_COMMAND(w1r)
  WRC_COMMAND(w1w)
#endif

#ifdef BOARD_COMMANDS
BOARD_COMMANDS
#endif

#undef WRC_COMMAND
#undef WRC_COMMAND2
