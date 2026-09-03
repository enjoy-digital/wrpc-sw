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
/** @brief Lists the shell commands available in this WRPC instance. */
WRC_COMMAND(help)

#ifdef CONFIG_CMD_AUXCLK
/** @brief Configures the auxiliary clock output. */
WRC_COMMAND(auxclk)
#endif
#ifdef CONFIG_CMD_AUXTMG
/** @brief Configures auxiliary timing outputs. */
WRC_COMMAND(auxtmg)
#endif
/** @brief Runs or reads the WRPC link calibration procedure. */
WRC_COMMAND(calibration)
#ifdef CONFIG_CMD_CONFIG
/** @brief Prints the configuration used to build WRPC. */
  WRC_COMMAND(config)
#endif
#ifdef CONFIG_DAC_LOG
/** @brief Sends DAC and VCXO data to a host over UDP. */
  WRC_COMMAND(daclog)
#endif
#ifdef CONFIG_CMD_LL
/** @brief Reads or sets the WRPC frame transmission delays. */
  WRC_COMMAND(delays)
/** @brief Reads or writes a 32-bit memory or FPGA register value. */
  WRC_COMMAND(devmem)
#endif
#ifdef CONFIG_AUX_DIAG
/** @brief Reads and writes auxiliary diagnostic registers. */
  WRC_COMMAND(diag)
#endif
#ifdef CONFIG_CMD_EP
/** @brief Displays and controls the Ethernet endpoint. */
  WRC_COMMAND(ep)
#endif
#ifdef CONFIG_TEMP_FAKE
/** @brief Reads or sets the simulated temperature values. */
  WRC_COMMAND(faketemp)
#endif
#ifdef CONFIG_FREQUENCY_MONITOR
/** @brief Displays frequency monitor measurements. */
  WRC_COMMAND(freqmon)
#endif
#ifdef CONFIG_GNSS
/** @brief Displays and configures GNSS time input. */
  WRC_COMMAND(gnss)
#endif
/** @brief Starts the WRPC graphical monitor. */
WRC_COMMAND(gui)
/** @brief Manages the WRPC startup initialization script. */
WRC_COMMAND(init)
#ifdef CONFIG_IP
/** @brief Reads and sets the WRPC IPv4 network configuration. */
  WRC_COMMAND(ip)
#endif
#ifdef CONFIG_CMD_LEAPSEC
/** @brief Reads and sets the PTP leap-second value. */
  WRC_COMMAND(leapsec)
#endif
#ifdef CONFIG_LATENCY_PROBE
/** @brief Configures and tests the WRPC latency probe. */
  WRC_COMMAND(ltest)
#endif
/** @brief Reads and sets the WRPC MAC address. */
WRC_COMMAND(mac)
/** @brief Legacy alias for the ptp command. */
WRC_COMMAND2(mode, ptp)
#ifdef CONFIG_CMD_NETCONSOLE
/** @brief Displays and configures the network console peer. */
  WRC_COMMAND(netconsole)
#endif
#ifdef CONFIG_CMD_NMEA
/** @brief Displays and configures NMEA timing input. */
  WRC_COMMAND(nmea)
#endif
/** @brief Controls and queries the software PLL. */
WRC_COMMAND(pll)
#ifdef CONFIG_CMD_PPS
/** @brief Displays and controls PPS generation. */
  WRC_COMMAND(pps)
#endif
/** @brief Displays CPU task activity and profiling information. */
WRC_COMMAND(ps)
/** @brief Configures and controls the White Rabbit PTP daemon. */
WRC_COMMAND(ptp)
/** @brief Enables or disables phase tracking. */
WRC_COMMAND(ptrack)
#ifdef CONFIG_CMD_REFRESH
/** @brief Changes the update period of GUI and statistics reporting. */
  WRC_COMMAND(refresh)
#endif
/** @brief Displays and manages devices on the Wishbone bus. */
WRC_COMMAND(sdb)
#ifdef CONFIG_GENERIC_SENSORS
/** @brief Displays readings from generic sensors. */
  WRC_COMMAND(sensors)
#endif
/** @brief Displays and manages the SFP calibration database. */
WRC_COMMAND(sfp)
/** @brief Suspends shell execution for a specified time. */
WRC_COMMAND(sleep)
/** @brief Displays and controls WRPC statistics reporting. */
WRC_COMMAND(stat)
#ifdef CONFIG_SYSLOG
/** @brief Displays and configures the remote syslog destination. */
  WRC_COMMAND(syslog)
#endif
#ifdef CONFIG_TEMP_SENSORS
/** @brief Displays current temperature sensor readings. */
  WRC_COMMAND(temp)
#endif
/** @brief Displays and sets the WRPC time. */
WRC_COMMAND(time)
/** @brief Displays the time elapsed since WRPC startup. */
WRC_COMMAND(uptime)
/** @brief Prints the running WRPC software version. */
WRC_COMMAND(ver)
/** @brief Sets the PPSi verbosity level. */
WRC_COMMAND(verbose)
#ifdef CONFIG_VLAN
/** @brief Displays and configures VLAN operation. */
  WRC_COMMAND(vlan)
#endif
#ifdef CONFIG_W1_TEMP
/** @brief Lists devices connected to the 1-Wire bus. */
  WRC_COMMAND(w1)
#endif
#ifdef CONFIG_W1_EEPROM
/** @brief Reads data from a 1-Wire EEPROM. */
  WRC_COMMAND(w1r)
/** @brief Writes data to a 1-Wire EEPROM. */
  WRC_COMMAND(w1w)
#endif

#ifdef BOARD_COMMANDS
BOARD_COMMANDS
#endif

#undef WRC_COMMAND
#undef WRC_COMMAND2
