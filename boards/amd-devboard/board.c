/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2023-2025 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Frederik Pfautsch <frederik.pfautsch@missinglinkelectronics.com>
 *         Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *
 * This file is intended to cover a range of AMD development boards.
 * Currently, the following boards are supported:
 *   - ZC706
 *   - ZCU102
 *   - ZCU106
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "board-config.h"
#include "dev/bb_i2c.h"
#include "dev/console.h"
#include "dev/i2c_eeprom.h"
#include "dev/syscon.h"
#include "dev/endpoint.h"
#include "dev/si57x.h"
#include "dev/si5324.h"
#include "storage.h"
#include "util.h"
#include "wrc-debug.h"
#include "wrc_global.h"

#include "board_zcu10x_bus_wb.h"

/* base addreses must be configured on runtime as they depend on the type of AMD devboard*/
uint32_t BASE_FMC_ENABLE;
uint32_t BASE_GNSS_UART;
uint32_t BASE_SI570;

typedef enum {
	ZCU102,
	ZCU106,
	ZC706,
	/* additional boards can be added here */
	NUM_SUPPORTED_BOARDS,
	UNSUPPORTED_BOARD
} supported_boards_t;

typedef enum {
	XM105,    /* AMD FMC XM105 Debug Card */
	/* additional cards can be added here */
	NUM_SUPPORTED_CARDS,
	UNSUPPORTED_CARD,
	NO_CARD
} supported_expansion_card_t;

typedef enum {
	UNSUPPORTED = 0,
	OPTIONAL,
	REQUIRED,
} expansion_card_support_t;

typedef struct {
	uint8_t addr;
	uint8_t ch_bitmask;
} i2c_mux_cfg_t;
#define NUM_SUPPORTED_I2C_MUX 2

typedef struct {
	i2c_mux_cfg_t mux_cfg[NUM_SUPPORTED_I2C_MUX];
	uint8_t vita_ga;
	const char *name;
} fmc_cfg_t;

typedef struct {
	uint8_t format_version;
	uint8_t internal_use_offset;
	uint8_t chassis_info_offset;
	uint8_t board_offset;
	uint8_t product_info_offset;
	uint8_t multirecord_offset;
	uint8_t padding;
	uint8_t checksum;
} fmc_eeprom_header_t;

static struct i2c_bus i2c_wrc_general;
static struct i2c_bus i2c_wrc_eeprom;
static struct i2c_eeprom_device wrc_eeprom_mac_dev;
static struct i2c_eeprom_device wrc_eeprom_sdbfs_dev;
static struct wr_si57x_interface_device wrc_si570_dev;
static struct wr_si5324_interface_device si5324_if_dev;

static struct gpio_device wrs_gpio_fmc_enable;

/* We support at most two FMC locations, ordering as in fmc_cfg_t above */
#define NUM_SUPPORTED_FMC_SITES 2
static supported_expansion_card_t this_card[NUM_SUPPORTED_FMC_SITES] = { NO_CARD, NO_CARD };
static supported_boards_t this_board = UNSUPPORTED_BOARD;

/* match board type with hardware name string in syscon */
static const char * const supported_boards_hw_name_strs[NUM_SUPPORTED_BOARDS] = {
	[ZCU102] = "X102",
	[ZCU106] = "X106",
	[ZC706] = "X706",
};

/* match expansion card with fmc eeprom string */
static const char * const supported_expansion_card_strs[NUM_SUPPORTED_CARDS] = {
	[XM105] = "FMC XM105 Debug",
};

/* board names for printing */
static const char * const supported_boards_strs[NUM_SUPPORTED_BOARDS] = {
	[ZCU102] = "ZCU102",
	[ZCU106] = "ZCU106",
	[ZC706] = "ZC706",
};

/* I2C MUX configurations per board */
#define zcu102_i2c_mux_cfg {                               \
    /* Mux 0 */                                            \
    {.addr = ZCU1XX_I2C_MUX0_ADR,                          \
     .ch_bitmask = ZCU1XX_I2C_MUX0_CH_BIT_EEPROM           \
                 | ZCU1XX_I2C_MUX0_CH_BIT_SI5341           \
                 | ZCU1XX_I2C_MUX0_CH_BIT_MGT_SI570},      \
    /* Mux 1 */                                            \
    {.addr = ZCU1XX_I2C_MUX1_ADR,                          \
     .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_SFP2}            \
}

#define zc706_i2c_mux_cfg {                                \
    /* Mux 0 */                                            \
    {.addr = ZC706_I2C_MUX0_ADR,                           \
     .ch_bitmask = ZC706_I2C_MUX0_CH_BIT_SI57X_SFPPLUS     \
                 | ZC706_I2C_MUX0_CH_BIT_EEPROM            \
                 | ZC706_I2C_MUX0_CH_BIT_SI5324_RTC8564JE} \
}

#define zcu106_i2c_mux_cfg {                               \
    /* Mux 0 */                                            \
    {.addr = ZCU1XX_I2C_MUX0_ADR,                          \
     .ch_bitmask = ZCU1XX_I2C_MUX0_CH_BIT_EEPROM           \
                 | ZCU1XX_I2C_MUX0_CH_BIT_SI5341           \
                 | ZCU1XX_I2C_MUX0_CH_BIT_MGT_SI570},      \
    /* Mux 1 */                                            \
    {.addr = ZCU1XX_I2C_MUX1_ADR,                          \
     .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_SFP0}            \
}

/*
 * due to being static, any member that is not explicitly defined will default
 * to 0, so check for addr = 0 before trying to configure MUX
 */
static const i2c_mux_cfg_t i2c_mux_cfgs[NUM_SUPPORTED_BOARDS][NUM_SUPPORTED_I2C_MUX] = {
	[ZCU102] = zcu102_i2c_mux_cfg,
	[ZCU106] = zcu106_i2c_mux_cfg,
	[ZC706] = zc706_i2c_mux_cfg,
};

#define zcu102_fmc_cfg {                                                           \
    /* FMC site 0, order is arbitrary but must match compatibility table below */  \
    [0] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZCU1XX_I2C_MUX0_ADR,                                          \
             .ch_bitmask = 0},                                                     \
            /* Mux 1 */                                                            \
            {.addr = ZCU1XX_I2C_MUX1_ADR,                                          \
             .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC0}                        \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC HPC0"                                                         \
    },                                                                             \
    /* FMC site 1, order is arbitrary but must match compatibility table below */  \
    [1] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZCU1XX_I2C_MUX0_ADR,                                          \
             .ch_bitmask = 0},                                                     \
            /* Mux 1 */                                                            \
            {.addr = ZCU1XX_I2C_MUX1_ADR,                                          \
             .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC1}                        \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC HPC1"                                                         \
    }                                                                              \
}

#define zcu106_fmc_cfg {                                                           \
    /* FMC site 0, order is arbitrary but must match compatibility table below */  \
    [0] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZCU1XX_I2C_MUX0_ADR,                                          \
             .ch_bitmask = 0},                                                     \
            /* Mux 1 */                                                            \
            {.addr = ZCU1XX_I2C_MUX1_ADR,                                          \
             .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC0}                        \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC HPC0"                                                         \
    },                                                                             \
    /* FMC site 1, order is arbitrary but must match compatibility table below */  \
    [1] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZCU1XX_I2C_MUX0_ADR,                                          \
             .ch_bitmask = 0},                                                     \
            /* Mux 1 */                                                            \
            {.addr = ZCU1XX_I2C_MUX1_ADR,                                          \
             .ch_bitmask = ZCU1XX_I2C_MUX1_CH_BIT_FMC_HPC1}                        \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC HPC1"                                                         \
    }                                                                              \
}

#define zc706_fmc_cfg {                                                            \
    /* FMC site 0, order is arbitrary but must match compatibility table below */  \
    [0] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZC706_I2C_MUX0_ADR,                                           \
             .ch_bitmask = ZC706_I2C_MUX0_CH_BIT_FMC_HPC}                          \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC HPC"                                                          \
    },                                                                             \
    /* FMC site 1, order is arbitrary but must match compatibility table below */  \
    [1] = {                                                                        \
        .mux_cfg = {                                                               \
            /* Mux 0 */                                                            \
            {.addr = ZC706_I2C_MUX0_ADR,                                           \
             .ch_bitmask = ZC706_I2C_MUX0_CH_BIT_FMC_LPC}                          \
        },                                                                         \
        .vita_ga = 0x0,                                                            \
        .name = "FMC LPC"                                                          \
    }                                                                              \
}

/*
 * due to being static, any member that is not explicitly defined will default
 * to 0, so check for mux_cfg.addr = 0 before trying to configure MUX
 */
static const fmc_cfg_t fmc_cfgs[NUM_SUPPORTED_BOARDS][NUM_SUPPORTED_FMC_SITES] = {
	[ZCU102] = zcu102_fmc_cfg,
	[ZCU106] = zcu106_fmc_cfg,
	[ZC706] = zc706_fmc_cfg,
};

/*
 * due to being static, any member that is not explicitly defined will default
 * to 0, which is UNSUPPORTED according to enum expansion_card_support_t
 * The FMC site index must match the fmc_cfg_t above.
 */
static const expansion_card_support_t board_card_compatibility[NUM_SUPPORTED_BOARDS][NUM_SUPPORTED_CARDS][NUM_SUPPORTED_FMC_SITES] = {
	[ZCU102][XM105][0] = OPTIONAL,
	[ZCU102][XM105][1] = OPTIONAL,
	[ZCU106][XM105][0] = OPTIONAL,
	[ZC706][XM105][0] = OPTIONAL,
	[ZC706][XM105][1] = OPTIONAL,
};

/*
 * generic I2C MUX configuration, assuming the process of enabling channels
 * consists of simply writing a bitmask to some I2C address.
 */
static void amd_devboard_generic_i2c_mux_apply_cfg(struct i2c_bus *i2c_bus, const i2c_mux_cfg_t *i2c_mux_cfg)
{
	bb_i2c_start(i2c_bus);
	bb_i2c_put_byte(i2c_bus, i2c_mux_cfg->addr << 1);
	bb_i2c_put_byte(i2c_bus, i2c_mux_cfg->ch_bitmask);
	bb_i2c_stop(i2c_bus);
}

/*
 * reads a specific field from FMC eeprom at a specified offset into a result buffer.
 * The field must be prefixed with a single byte indicating the length of
 * the following value field according to FMC/FRU spec:
 * https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/ipmi-platform-mgt-fru-info-storage-def-v1-0-rev-1-3-spec-update.pdf
 * Also, the supplied result buffer needs to be at least 16 chars long (sizeof(char buf) + 1).
 * This function can only handle ASCII input and does not evaluate the language/type field
 */
static int amd_devboard_eeprom_read_field(struct i2c_eeprom_device *dev, char *result, int *offset)
{
	char buf[15];
	uint8_t len;
	int safe_size;

	i2c_eeprom_read(dev, *offset, &len, 1);
	if (len == 0xC1) {
		// end of fields
		return -1;
	}
	else if (len == 0) {
		// empty
		*result = '\0';
		*offset += 1;
		return 0;
	}
	else if (len >> 6 != 0x3) {
		// type is not 8-bit ASCII + Latin 1
		return -1;
	}

	len &= ~0xC0; // Remove type from len, which is in bits 7:6
	safe_size = min(len, sizeof(buf));

	if (result) {
		i2c_eeprom_read(dev, *offset + 1, buf, safe_size);
		strncpy(result, buf, safe_size);
		result[safe_size + 1] = '\0';
	}

	*offset += len + 1;
	return 0;
}

/*
 * Tries to access either a small FMC eeprom at 0x50 (1 address bytes) or a large eeprom
 * (2 address bytes) at 0x54. The tries to read board data such as product name from eeprom.
 * The board data can be either stored in the board area or the product info area.
 *
 * FMC/FRU spec: https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/ipmi-platform-mgt-fru-info-storage-def-v1-0-rev-1-3-spec-update.pdf
 *
 * vita_ga refers to the GA[1] and GA[0] pin of VITA 57.1-2019 Rule 5.81:
 * Manufacturers shall either use a 2Kb (or smaller) or a 32Kb (or larger) eeprom. The 2Kb eeprom shall be addressible
 * using 0b101_00xx using a single byte addressing scheme, a 32Kb shall use 0b101_01xx using a two byte addresing scheme. 
 * x refer to GA[0..1], which can be used to differentiate between multiple eeproms / FMC connectors on the same bus.
 * 4Kb to 16Kb eeproms are not supported, as they often use a single byte addressing scheme, which redefine some of the
 * address bits that VITA connects to the GA pins.
 *
 * Caution: This function expects FMC cards to comply with this standard (similarly to the
 * BMC, that supplies V_adj. If cards do not do that, then one might have to implement
 * nasty workarounds)!
 */
static int amd_devboard_read_fmc_id(uint8_t vita_ga, char *manufacturer, char *product, char *serial, char *part)
{
	static struct i2c_eeprom_device wrc_fmc_eeprom_dev;
	static fmc_eeprom_header_t header;
	uint8_t format_version;
	int current_offset = 0;
	int ret;
	uint8_t lang;
	int date_time;

	vita_ga &= 0x3;
	if (bb_i2c_devprobe(&i2c_wrc_general, EEPROM_FMC_SMALL | vita_ga)) {
		i2c_eeprom_create(&wrc_fmc_eeprom_dev, &i2c_wrc_general, EEPROM_FMC_SMALL | vita_ga, 1);
	}
	else if (bb_i2c_devprobe(&i2c_wrc_general, EEPROM_FMC_LARGE | vita_ga)) {
		i2c_eeprom_create(&wrc_fmc_eeprom_dev, &i2c_wrc_general, EEPROM_FMC_LARGE | vita_ga, 2);
	}
	else {
		board_dbg("No FMC eeprom detected\n");
		return 0;
	}

	/* Check if an eeprom responds with correct format */
	i2c_eeprom_read(&wrc_fmc_eeprom_dev, current_offset, &header, sizeof(header));
	if (header.format_version != 1) {
		board_dbg("WARNING: No valid FMC header detected, either unsupported version or invalid header (%u)\n",
			header.format_version);
		return -1;
	}

	if (header.board_offset > 0 && header.board_offset < 0xFF) {
		board_dbg("Using FMC board area\n");
		/* offset is stored as multiple of 8B */
		current_offset = header.board_offset * 8;
		date_time = 3;
	}
	else if (header.product_info_offset > 0 && header.product_info_offset < 0xFF) {
		board_dbg("Using FMC product info area\n");
		/* offset is stored as multiple of 8B */
		current_offset = header.product_info_offset * 8;
		date_time = 0;
	}
	else {
		board_dbg("WARNING: No valid FMC board area or product info detected (%u, %u)\n",
			header.board_offset, header.product_info_offset);
		return -1;
	}

	/* Check if an eeprom responds with correct format */
	i2c_eeprom_read(&wrc_fmc_eeprom_dev, current_offset, &format_version, 1);
	if (format_version != 1)  {
		board_dbg("WARNING: No valid FMC header detected, either unsupported version or invalid header (%u)\n",
			format_version);
		return -1;
	}

	/* Read language code */
	current_offset += 2;
	i2c_eeprom_read(&wrc_fmc_eeprom_dev, current_offset, &lang, 1);
	if (lang != 0 && lang != 25)  {
		board_dbg("WARNING: EEPROM is in unicode (language other than english) (%u)\n", lang);
		return -1;
	}

	/* Read manufacturer, Starting at offset 6 or 4, depending if date_time is part of the area */
	current_offset += 1 + date_time;
	ret = amd_devboard_eeprom_read_field(&wrc_fmc_eeprom_dev, manufacturer, &current_offset);
	if (ret < 0) {
		goto err_manufacturer;
	}

	/* Read product name */
	ret = amd_devboard_eeprom_read_field(&wrc_fmc_eeprom_dev, product, &current_offset);
	if (ret < 0) {
		goto err_product;
	}

	/* Read serial number */
	ret = amd_devboard_eeprom_read_field(&wrc_fmc_eeprom_dev, serial, &current_offset);
	if (ret < 0) {
		goto err_serial;
	}

	/* Read part number */
	ret = amd_devboard_eeprom_read_field(&wrc_fmc_eeprom_dev, part, &current_offset);
	if (ret < 0) {
		goto err_part;
	}

	return 1;

err_manufacturer:
	strcpy(manufacturer, "--");

err_product:
	strcpy(product, "--");

err_serial:
	strcpy(serial, "--");

err_part:
	strcpy(part, "--");

	return 1;
}

static void amd_devboard_si570_init(void)
{
	uint32_t f_xtal;
	uint32_t f_target;

	// Initialize Si570 on ZCx10x
	wr_si57x_interface_init(&wrc_si570_dev, BASE_SI570, SI570_ADR);
	si57x_reset(&wrc_si570_dev);
	timer_delay_ms(10);

	switch (this_board) {
		case ZCU102:
		case ZCU106:
			si57x_get_xtal_frequency(&wrc_si570_dev, 156250000, &f_xtal); // ZCU10x uses 156250000 as f0

			// tune to exactly 124975605 (125M - 200ppm)
			f_target = 124975605;
			break;

		case ZC706:
			si57x_get_xtal_frequency(&wrc_si570_dev, 156250000, &f_xtal); // ZC706 uses 156250000 as f0

			f_target = 125000000;
			break;

		default:
			pp_printf("[board] WARNING: Unsupported board for Si570 config!\n");
			return;
	}

	si57x_set_frequency(&wrc_si570_dev, f_xtal, f_target, 3); // hw interface VCO gain = 3 (~20 ppm)
	board_dbg("Si570: frequency set\n");

	si57x_get_xtal_frequency(&wrc_si570_dev, f_target, &f_xtal);

	board_dbg("Wait 1 second for clock to settle ...\n");
	timer_delay_ms(1000);
}

static void amd_devboard_si5324_init(void)
{
	wr_si5324_interface_init(&si5324_if_dev, &i2c_wrc_general, SI5324_ADR);
	si5324_reset(&si5324_if_dev);

	switch (this_board) {
		case ZC706:
			// Si4324 as bypass for main -> GT
			si5324_set_125m_clean(&si5324_if_dev);
			break;

		default:
			pp_printf("[board] WARNING: Unsupported board for Si5324 config!\n");
			return;
	}
}

int wrc_board_early_init(void)
{
	/*
	 * fetch specific board type from SYSCON
	 * TODO: could initialization of wrc_global_link.wrc_hw_name be moved before
	 * wrc_board_early_init() in wrc_main.c?
	 */
	char hw_name[5] = {0};
	get_hw_name(hw_name);
	for (int i = 0; i < NUM_SUPPORTED_BOARDS; i++) {
		if (strncmp(supported_boards_hw_name_strs[i], hw_name, 4) == 0) {
			this_board = i;
			board_dbg("AMD development board: %s\n", supported_boards_strs[this_board]);
		}
	}
	if (this_board == UNSUPPORTED_BOARD) {
		board_dbg("WARNING: Not a supported AMD development board (hardware name: %s).\n"
		          "Attempting generic initialization, but functionality may be missing or broken.\n", hw_name);
	}

	/*
	 * set up base addresses for board-level peripherals
	 */
	switch (this_board) {
		case ZCU102:
		case ZCU106:
			BASE_FMC_ENABLE = (BASE_AUXWB + BOARD_ZCU10X_BUS_WB_FMC_ENABLE);
			BASE_GNSS_UART  = (BASE_AUXWB + BOARD_ZCU10X_BUS_WB_GNSS_UART);
			BASE_SI570      = (BASE_AUXWB + BOARD_ZCU10X_BUS_WB_SI5XX);
			break;
		case ZC706:
			//TODO: replace with cheby definitions
		case UNSUPPORTED_BOARD:
		case NUM_SUPPORTED_BOARDS:
			/* use some default address values */
			BASE_FMC_ENABLE = (BASE_AUXWB + 0x0000);
			BASE_GNSS_UART  = (BASE_AUXWB + 0x0100);
			BASE_SI570      = (BASE_AUXWB + 0x0200);
			break;
	}
	/*
	 * create and init I2C busses.
	 * Notice:
	 *   - ZC706: General and EEPROM buses are merged at top level of the board
	 *     implementation, but it doesn't hurt to create both I2C buses in any case
	 */
	bb_i2c_create(&i2c_wrc_general, &pin_sysc_sfp1_scl, &pin_sysc_sfp1_sda);
	bb_i2c_init(&i2c_wrc_general);

	bb_i2c_create(&i2c_wrc_eeprom, &pin_sysc_fmc_scl, &pin_sysc_fmc_sda);
	bb_i2c_init(&i2c_wrc_eeprom);

	if (this_board == UNSUPPORTED_BOARD) {
		/* bail out, as rest is irrelevant */
		return 0;
	}

	/*
	 * Before general I2C Mux initialization, we first iterate the FMC connectors
	 * and check for possible expansion cards and compatibility
	 */
	wb_gpio_create(&wrs_gpio_fmc_enable, BASE_FMC_ENABLE);
	struct gpio_pin ena;
	for (int i = 0; i < NUM_SUPPORTED_FMC_SITES && fmc_cfgs[this_board][i].name > 0; i++) {
		char manufacturer[16];
		char product[16];
		char serial[16];
		char part[16];

		board_dbg("Checking %s for an expansion card...\n", fmc_cfgs[this_board][i].name);
		for (int j = 0; j < NUM_SUPPORTED_I2C_MUX && fmc_cfgs[this_board][i].mux_cfg[j].addr > 0; j++) {
			amd_devboard_generic_i2c_mux_apply_cfg(&i2c_wrc_general, &fmc_cfgs[this_board][i].mux_cfg[j]);
		}
		int ret = amd_devboard_read_fmc_id(fmc_cfgs[this_board][i].vita_ga, manufacturer, product, serial, part);
		if (ret > 0) {
			board_dbg("Detected %s (%s, serial: %s, part: %s) on %s.\n",
				product, manufacturer, serial, part, fmc_cfgs[this_board][i].name);
			this_card[i] = UNSUPPORTED_CARD;
			for (int j = 0; j < NUM_SUPPORTED_CARDS; j++) {
				if (strncmp(supported_expansion_card_strs[j], product, sizeof(product)) == 0) {
					this_card[i] = j;

					if (board_card_compatibility[this_board][this_card[i]][i] != UNSUPPORTED) {
						/* Enable FMC card */
						ena = (struct gpio_pin) {&wrs_gpio_fmc_enable, i};
						gen_gpio_set_dir(&ena, 1);
						gen_gpio_out(&ena, 1);
						break;
					} else {
						pp_printf("[board] WARNING: Not a supported expansion card (%s %s) on this AMD development board "
						          "(hardware name: %s). Leaving this FMC site disabled.\n", manufacturer, product, hw_name);
					}
				}
			}
		}
		else if (ret < 0) {
			board_dbg("Unable to identify FMC card on %s.\n", fmc_cfgs[this_board][i].name);
		}

		for (int j = 0; j < NUM_SUPPORTED_CARDS; j++) {
			if (this_card[i] == NO_CARD && board_card_compatibility[this_board][j][i] == REQUIRED) {
				pp_printf("[board] WARNING: This AMD development board (hardware name: %s) requires an expansion card "
					      "(name: %s) on %s.\nWithout it, this design will probably not work.\n",
					hw_name, supported_expansion_card_strs[j], fmc_cfgs[this_board][i].name);
			}
		}
	}

	/*
	 * For all currently supported boards that utilize one or more I2C Muxes,
	 * it's sufficient to enable all relevant channels on the MUX, as there is
	 * no address overlap that would require using only one channel at a time.
	 */
	for (int i = 0; i < NUM_SUPPORTED_I2C_MUX && i2c_mux_cfgs[this_board][i].addr > 0; i++) {
		amd_devboard_generic_i2c_mux_apply_cfg(&i2c_wrc_general, &i2c_mux_cfgs[this_board][i]);
	}

	/*
	 * Set up EEPROM(s) and SDBFS
	 * Notice:
	 *   - ZCU10x family devices may have board-level metadata stored in the
	 *     'regular' EEPROM (including a MAC address, which will be used if the
	 *     SDBFS doesn't contain one -- NOTE: This might collide with the GbE of
	 *     the Zynq PS, if both interfaces are used on the same network!).
	 *     In order not to corrupt the data present in the "regular" EEPROM,
	 *     SDBFS is stored in HDMI EDID EEPROM instead.
	 *
	 * Afterwards, run necessary clock init
	 */
	int ret = 0;
	switch (this_board) {
		case ZCU102:
		case ZCU106:
			/* for ZCU10X, use HDMI EDID EEPROM */
			i2c_eeprom_create(&wrc_eeprom_sdbfs_dev, &i2c_wrc_eeprom, EEPROM_HDMI_EDID_ADR, EEPROM_HDMI_EDID_BYTE_OFFSET);
			storage_i2ceeprom_create(&wrc_storage_dev, &wrc_eeprom_sdbfs_dev);
			storage_mount(&wrc_storage_dev);

			/* create (READONLY!) MAC EEPROM for ZCU10X */
			i2c_eeprom_create(&wrc_eeprom_mac_dev, &i2c_wrc_general, EEPROM_M24C08_ADR, EEPROM_M24C08_BYTE_OFFSET);

			amd_devboard_si570_init();
			break;

		case ZC706:
			/* for ZC706, use free M24C08 EEPROM */
			i2c_eeprom_create(&wrc_eeprom_sdbfs_dev, &i2c_wrc_eeprom, EEPROM_M24C08_ADR, EEPROM_M24C08_BYTE_OFFSET);
			storage_i2ceeprom_create(&wrc_storage_dev, &wrc_eeprom_sdbfs_dev);

			/* any board using M24C08 EEPROM: set size to 1kB */
			wrc_storage_dev.size = 1024;
			wrc_storage_dev.block_size = 16;
			wrc_storage_dev.entry_points = (int32_t[]) { 0, -1 };
			storage_mount(&wrc_storage_dev);

			amd_devboard_si570_init();
			amd_devboard_si5324_init();
			break;

		case UNSUPPORTED_BOARD:
		case NUM_SUPPORTED_BOARDS:
			/* Do not do anything special here */
			break;
	}

	return ret;
}

int wrc_board_init(void)
{
	uint8_t mac_addr[6];

	if (storage_get_persistent_mac(0, mac_addr) == 0) {
		board_dbg("Got MAC address from SDBFS\n");
	} else if ((this_board == ZCU102 ||
	            this_board == ZCU106) &&
	           i2c_eeprom_read(&wrc_eeprom_mac_dev, 0x20, mac_addr, 6) == 6) {
		board_dbg("Got MAC address from board-level metadata EEPROM\n");
	} else {
		board_dbg("Failed to get MAC address from EEPROM. Using fallback address.\n");
		mac_addr[0] = 0x22;
		mac_addr[1] = 0x33;
		mac_addr[2] = 0x44;
		mac_addr[3] = 0x55;
		mac_addr[4] = 0x66;
		mac_addr[5] = 0x77;
	}
	ep_set_mac_addr(&wrc_endpoint_dev, mac_addr);
	ep_pfilter_init_default(&wrc_endpoint_dev);

	return 0;
}
