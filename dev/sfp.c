/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012-2021 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Adam Wujek
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
/* SFP Detection / managenent functions */

#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "wrc-task.h"
#include "pp-printf.h"
#include "dev/syscon.h"
#include "dev/bb_i2c.h"
#include "dev/gpio.h"
#include "sfp.h"
#include "storage.h"

static struct shw_sfp_header sfp_header;
/* sfp_dom is static, so it is not included if CONFIG_SFP_DOM is not selected */
static struct shw_sfp_dom sfp_dom;

struct sfp_info sfp_info = {
	.sfp_header = &sfp_header,
#ifdef CONFIG_SFP_DOM
	.sfp_dom = &sfp_dom,
#endif
	.version = WRC_G_SFP_VERSION,
	.sfp_params = {
		.alpha = 0, /* default value for alpha */
	},
};

static int sfp_present(void)
{
	return !gen_gpio_in(&pin_sysc_sfp1_det);
}

static void sfp_read_i2c(int addr, uint8_t *mem, int start, int size)
{
	const struct i2c_bus *dev = &dev_i2c_sfp1;
	int i = start;
	uint8_t data;

	bb_i2c_init(dev);

	bb_i2c_start(dev);
	bb_i2c_put_byte(dev, addr << 1);
	bb_i2c_put_byte(dev, start);
	bb_i2c_repeat_start(dev);
	bb_i2c_put_byte(dev, addr << 1 | BB_I2C_WRITE);
	bb_i2c_get_byte(dev, &data, 1);
	bb_i2c_stop(dev);
	*(mem + i) = data;

	bb_i2c_start(dev);
	bb_i2c_put_byte(dev, addr << 1 | BB_I2C_WRITE);
	for (i++; i < start + size - 1; ++i) {
		bb_i2c_get_byte(dev, &data, 0);
		*(mem + i) = data;
	}
	bb_i2c_get_byte(dev, &data, 1);	//final word, checksum
	*(mem + i) = data;
	bb_i2c_stop(dev);
}

static int verify_checksum(uint8_t *mem, int from, int to)
{
	int i;
	uint16_t sum = 0;

	for (i = from; i < to; i++) {
		sum += *(mem + i);
	}
	sum = sum & 0xff;

	if (sum == *(mem + to))
		return 0;
	return 1;
}

int sfp_dom_update(void)
{
	static uint32_t sfp_dom_last_update_tick;

	if (wrc_task_not_yet(&sfp_dom_last_update_tick,
			     SFP_DOM_UPDATE_TICK_INTERVAL)) {
		return 0;
	}

	if (!(sfp_header.diagnostic_monitoring_type & SFP_DIAG_IMPLEMENTED)) {
		return 0;
	}

	/* Read Real Time Diagnostics (DOM) data, bytes 96-111 */
	sfp_read_i2c(I2C_SFP_DOM_ADDRESS, (uint8_t *)&sfp_dom, 96, 10);

	return 1;
}

int sfp_match(int force)
{
	int match;

	if (!force && !sfp_present()) {
		return -ENODEV;
	}

	/* Read sfp header info from SFP */
	sfp_read_i2c(I2C_SFP_ADDRESS, (uint8_t *)&sfp_header, 0,
		     sizeof(struct shw_sfp_header));

	if (verify_checksum((uint8_t *)&sfp_header, 0, 63)
	    || verify_checksum((uint8_t *)&sfp_header, 64, 95)) {
		/* Print error */
		pp_printf("Wrong SFP checksum %s\n", "");
		return -EIO;
	}

	if (HAS_SFP_DOM
	    && sfp_header.diagnostic_monitoring_type & SFP_DIAG_IMPLEMENTED) {
		/* Read sfp DOM info from SFP only if DOM supported */
		sfp_read_i2c(I2C_SFP_DOM_ADDRESS, (uint8_t *)&sfp_dom, 0,
			     sizeof(struct shw_sfp_dom));
		if (verify_checksum((uint8_t *)&sfp_dom, 0, 95)) {
			pp_printf("Wrong SFP checksum %s\n", "DOM");
		}
	}

	memcpy(sfp_info.sfp_params.pn, sfp_info.sfp_header->vendor_pn,
	       SFP_PN_LEN);
	match = storage_match_sfp(&sfp_info.sfp_params);
	if (match <= 0) {
		sfp_info.sfp_in_db = SFP_NOT_MATCHED;
		return match < 0 ? match : -ENXIO;
	}

	sfp_info.sfp_in_db = SFP_MATCHED;
	return 0;
}

// #define DEBUG_I2C

/* lm32's compiler does not remove strings used in a function if
 * the function is optimized out. So if an option is not used don't include
 * (the not optimized out strings in) functions. Ugly but can be
 * removed when LM32's support is dropped. */
#if (defined CONFIG_CMD_SFP_TUNABLE && defined CONFIG_ARCH_LM32) || defined CONFIG_ARCH_RISCV
static void sfp_i2c_read(uint8_t addr, uint8_t reg, uint8_t * data, uint16_t len)
{
#ifdef DEBUG_I2C
	pp_printf("Reading from 0x%02x, reg=0x%02x len=0x%02x: ", addr, reg, len);
#endif

	bb_i2c_init( &dev_i2c_sfp1 );
	bb_i2c_start(&dev_i2c_sfp1);
	bb_i2c_put_byte(&dev_i2c_sfp1, addr<<1);
	/* select register to read */
	bb_i2c_put_byte(&dev_i2c_sfp1, reg);

	bb_i2c_repeat_start(&dev_i2c_sfp1);

	bb_i2c_put_byte(&dev_i2c_sfp1, addr << 1 | BB_I2C_WRITE);

	while (len > 0) {
		bb_i2c_get_byte(&dev_i2c_sfp1, data, len == 1);
#ifdef DEBUG_I2C
		pp_printf(" %02x", *data);
#endif
		len--;
		data++;
	}

	bb_i2c_stop(&dev_i2c_sfp1);
#ifdef DEBUG_I2C
	puts(", done!\n");
#endif
}

static void sfp_i2c_write(uint8_t addr, uint8_t reg, uint8_t * data, uint16_t len)
{
	bb_i2c_init( &dev_i2c_sfp1 );
	bb_i2c_start(&dev_i2c_sfp1);
	bb_i2c_put_byte(&dev_i2c_sfp1, addr<<1);
	bb_i2c_put_byte(&dev_i2c_sfp1, reg);
#ifdef DEBUG_I2C
	pp_printf("Writing to %02x, reg=%02x:\n", addr, reg);
#endif
	while (len > 0) {
#ifdef DEBUG_I2C
		pp_printf(" %02x", *data);
#endif
		bb_i2c_put_byte(&dev_i2c_sfp1, *data);
		len--;
		data++;
	}
	bb_i2c_stop(&dev_i2c_sfp1);
#ifdef DEBUG_I2C
	puts("\ndone!\n");
#endif
}

/* selects the page for the upper 128 bytes of address space A2 */
void sfp_a2_select_page(uint8_t page)
{
	sfp_i2c_write(I2C_SFP_DOM_ADDRESS, SFP_A2_PAGE_SELECT_REG, &page, 1);
}

uint8_t sfp_a2_read_u8(uint8_t reg)
{
	uint8_t data;
	sfp_i2c_read(I2C_SFP_DOM_ADDRESS, reg, &data, 1);
	return data;
}

uint16_t sfp_a2_read_u16(uint8_t reg)
{
	uint8_t data[2];
	uint16_t ret;

	sfp_i2c_read(I2C_SFP_DOM_ADDRESS, reg, data, 2);

	ret = data[0] << 8;
	ret |= data[1];

	return ret;
}

void sfp_a2_write_u16(uint8_t reg, uint16_t value)
{
	uint8_t data[2];
	data[0] = 0xFF & (value >> 8);
	data[1] = 0xFF & value;

	sfp_i2c_write(I2C_SFP_DOM_ADDRESS, reg, data, 2);
}


void sfp_a2_write_u8(uint8_t reg, uint8_t value)
{
	sfp_i2c_write(I2C_SFP_DOM_ADDRESS, reg, &value, 1);
}

int sfp_tune_ch(const char *args)
{
	uint8_t tmp8;
	uint8_t tmp16;
	uint16_t val;

	val = atoi(args);
	if (val > 500) {
		/* Assume tuning by wavelength */
		val *= 20;

		/* Find the decimal part of wavelength */
		while (*args) {
			if (*args == '.') {
				char atoi_buff[2];

				/* decimal point found */
				args++;

				atoi_buff[1] = 0;
				atoi_buff[0] = *args;
				/* Number of 0.1nm. val is in units of 0.05nm */
				val += atoi(atoi_buff) * 10 / 5;
				/* Wavelength is in units of 0.05, so it is
				 * enough to check the second char. If is 5,
				 * add 1 to val. Otherwise ignore. */
				if (*(args + 1) == '5')
					val++;

				break;
			}
			args++;
		}
	}

	sfp_a2_select_page(SFP_A2_PAGE_CONTROL_FUNC);

	if (val > 500) {
		pp_printf("Tuning SFP to wavelength: %d.%02d (raw: %d)\n",
			  (val * 5)/100, (val * 5) % 100, val);
		sfp_a2_write_u16(SFP_A2_CTRL_WL_SET_REG, val);
	} else {
		pp_printf("Tuning SFP to channel: %d\n", val);
		sfp_a2_write_u16(SFP_A2_CTRL_CHNO_SET_REG, val);
	}

	tmp8 = sfp_a2_read_u8(SFP_A2_CTRL_CUR_STATUS_REG);
	pp_printf("Status reg 0x%02x, TX tune %d, Unlocked %d\n",
		  tmp8,
		  tmp8 & SFP_A2_CTRL_CUR_STATUS_TXTUNE,
		  tmp8 & SFP_A2_CTRL_CUR_STATUS_WL_UNLOCK);
	tmp8 = sfp_a2_read_u8(SFP_A2_CTRL_LATCH_STATUS_REG);
	pp_printf("latch reg 0x%02x\n", tmp8);
	tmp16 = sfp_a2_read_u16(SFP_A2_CTRL_FREQ_ERR_REG);
	pp_printf("freq error 0x%02x\n", tmp16);
	tmp16 = sfp_a2_read_u16(SFP_A2_CTRL_WL_ERR_REG);
	pp_printf("wavelength error 0x%02x\n", tmp16);

	return 0;
}
#endif
