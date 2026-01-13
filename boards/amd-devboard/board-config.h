/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2023-2024 CERN (www.cern.ch)
 *                         Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Frederik Pfautsch <frederik.pfautsch@missinglinkelectronics.com>
 *         Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#ifndef __BOARD_CONFIG_AMD_DEVBOARD_H
#define __BOARD_CONFIG_AMD_DEVBOARD_H
/*
 * This is meant to be automatically included by the Makefile,
 * when wrpc-sw is build for wrc (node) -- as opposed to wrs (switch)
 */

/* Added peripherals */
#define BASE_FMC_ENABLE   (DEV_BASE + 0x8000) // on all boards
#define BASE_SI570        (DEV_BASE + 0x8100) // on X102, X706

/* Board-specific parameters */
#define TICS_PER_SECOND 1000

/* WR Core system/CPU clock frequency in Hz */
#define CPU_CLOCK 62500000ULL

/* WR Reference clock period (picoseconds) and frequency (Hz) */
#define NS_PER_CLOCK 16
#define REF_CLOCK_PERIOD_PS 16000
#define REF_CLOCK_FREQ_HZ 62500000

/* Maximum number of simultaneously created sockets */
#define NET_MAX_SOCKETS 12

/* Socket buffer size, determines the max. RX packet size */
#define NET_MAX_SKBUF_SIZE 512

/* spll parameter that are board-specific */
/* For timing closure, DMTD clock is divided by 2 when the RX clock
  is 125Mhz. This is controlled by the g_divide_input_by_2 generic
  in wr_core.vhd */
#define BOARD_DIVIDE_DMTD_CLOCKS 0

/* Number of reference channels (RX clocks) */
#define BOARD_MAX_CHAN_REF 1
/* Number of external pll that can be disciplined */
#define BOARD_MAX_CHAN_AUX 2
/* Should be the same as reference channels */
#define BOARD_MAX_PTRACKERS 1

/* Events are not used on this platform */
#define BOARD_USE_EVENTS 0

/* Use one uart at 115200 baud. Some boards may add extra uart. */
#define CONSOLE_UART_BAUDRATE 115200

/* i2c mux parameters */
#define ZC706_I2C_MUX0_ADR 0x74
#define ZC706_I2C_MUX0_CH_BIT_SI57X_SFPPLUS (1 << 0)
#define ZC706_I2C_MUX0_CH_BIT_EEPROM (1 << 2)
#define ZC706_I2C_MUX0_CH_BIT_SI5324_RTC8564JE (1 << 4)
#define ZC706_I2C_MUX0_CH_BIT_FMC_HPC (1 << 5)
#define ZC706_I2C_MUX0_CH_BIT_FMC_LPC (1 << 6)

#define ZCU1XX_I2C_MUX0_ADR 0x74
#define ZCU1XX_I2C_MUX0_CH_BIT_EEPROM (1 << 0)
#define ZCU1XX_I2C_MUX0_CH_BIT_SI5341 (1 << 1)
#define ZCU1XX_I2C_MUX0_CH_BIT_SI570 (1 << 2)
#define ZCU1XX_I2C_MUX0_CH_BIT_MGT_SI570 (1 << 3)

#define ZCU1XX_I2C_MUX1_ADR 0x75
#define ZCU1XX_I2C_MUX1_CH_BIT_SFP0 (1 << 7)
#define ZCU1XX_I2C_MUX1_CH_BIT_SFP1 (1 << 6)
#define ZCU1XX_I2C_MUX1_CH_BIT_SFP2 (1 << 5)
#define ZCU1XX_I2C_MUX1_CH_BIT_SFP3 (1 << 4)
#define ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC1 (1 << 1)
#define ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC0 (1 << 0)

/* i2c eeprom properties */
#define EEPROM_M24C08_ADR 0x54
#define EEPROM_M24C08_BYTE_OFFSET -2
#define EEPROM_HDMI_EDID_ADR 0x50
#define EEPROM_HDMI_EDID_BYTE_OFFSET 2

/* fmc i2c eeprom address */
#define EEPROM_FMC_SMALL 0x50
#define EEPROM_FMC_LARGE 0x54

/* i2c addresses */
#define SI570_ADR 0x5D
#define SI5324_ADR 0x68

/* Maximum number of files in the sdb filesystem.
   Need at least 4: ., sfp database, init script and calibration
   MAC address could also be written on sdbfs. */
#define SDBFS_REC 5

#endif /* __BOARD_CONFIG_AMD_DEVBOARD_H */
