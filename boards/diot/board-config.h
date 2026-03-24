/*
 * This work is part of the White Rabbit project
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#ifndef __BOARD_CONFIG_H
#define __BOARD_CONFIG_H

/*
 * This is meant to be automatically included by the Makefile,
 * when wrpc-sw is build for wrc (node) -- as opposed to wrs (switch)
 */

/* Board-specific parameters: the number of tics per seconds, or by how many
   steps SYSCON->TVR is incremented each second.
   This is controlled by the g_cntr_period generic in wrc_periph.vhd, the
   value is 62500 and is not expected to change.
   If clk_sys is 62.5Mhz, there are 1000 tics per second. */
#define TICS_PER_SECOND             1000

/* WR Core system/CPU clock frequency in Hz (clk_sys) */
#define CPU_CLOCK                   62500000ULL

/* WR Reference clock period (picoseconds) and frequency (Hz)
   The eth phy interface datarate is 125MB/s.  */
#define NS_PER_CLOCK                16
#define REF_CLOCK_PERIOD_PS         16000
#define REF_CLOCK_FREQ_HZ           62500000

/* Maximum number of simultaneously created sockets */
#define NET_MAX_SOCKETS             12

/* Socket buffer size, determines the max. RX packet size */
#define NET_MAX_SKBUF_SIZE          512

/* spll parameter that are board-specific */
#define BOARD_DIVIDE_DMTD_CLOCKS    0

/* Number of reference channels (RX clocks) */
#define BOARD_MAX_CHAN_REF          1
/* Number of external pll that can be disciplined */
#define BOARD_MAX_CHAN_AUX          2
/* Should be the same as reference channels */
#define BOARD_MAX_PTRACKERS         1

/* Events are not used on this platform */
#define BOARD_USE_EVENTS            0

/* Use one uart at 115200 baud. Some boards may add extra uart. */
#define BOARD_EXTRA_CONSOLES        0
#define CONSOLE_UART_BAUDRATE       115200

/* Maximum number of files in the sdb filesystem.
   Need at least 4: ., sfp database, init script and calibration
   MAC address could also be written on sdbfs. */
#define SDBFS_REC                   5

/* Specific to this board (see board.c) */
/* I2C address of the storage eeprom */
#define I2C_STORAGE_EEPROM_ADR      0x54

/* I2C address of the storage eeprom */
#define I2C_MAC_EEPROM_ADR          0x56
#define I2C_MAC_EEPROM_OFFSET       0xFA

/* However it is not used. */
#define EEPROM_STORAGE              0

#endif /* __BOARD_CONFIG_H */
