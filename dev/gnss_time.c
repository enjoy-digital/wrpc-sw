/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 - 2025 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *         Arthur Finkelmann <arthur.finkelmann@missinglinkelectronics.com>
 *         Frederik Pfautsch <frederik.pfautsch@missinglinkelectronics.com>
 *
 * Released according to the GNU GPL, version 2 or any later version,
 * OR according to the GNU LGPL, version 3.0 or any later version
 */

/**
 * @file gnss_time.c
 * @brief GNSS communication and data handling functions.
 *
 * This module provides functionality for interfacing with a GNSS receiver, handling
 * UART communication, and extracting GNSS fix and UTC data.
 *
 * @note The actual parsing of NMEA sentences is handled separately. This module
 *       focuses on receiving and managing raw GNSS data.
 */

#include "dev/gnss_time.h"

#include "board.h"
#include "dev/console.h"
#include "dev/pps_gen.h"
#include "lib/nmea.h"
#include "pp-printf.h"
#include "util.h"
#include "wrpc.h"
#include "wrc-debug.h"

#include <sys/types.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/*                           Types, defines, globals                          */
/* -------------------------------------------------------------------------- */
#define GNSS_FIX_TIMEOUT_MS     2000

#define UART_RX_BUFFER_SIZE     1024 // must match gateware!

static struct gnss_device gnss_dev;
static struct simple_uart_device gnss_uart_dev;

typedef enum parser_state {
	NMEA_WAIT_DELIM_START,
	NMEA_WAIT_DELIM_STOP_1,
	NMEA_WAIT_DELIM_STOP_2
} parser_state_t;

static int gnss_latest_fix = -1;
static int64_t gnss_latest_utc = -1;
static timeout_t tmo_fix;
static bool print_raw = false;
static bool sync_utc = false;

/* -------------------------------------------------------------------------- */
/*                                 Assertions                                 */
/* -------------------------------------------------------------------------- */
/* GNSS configuration byte string must contain even number of characters */
extern int assert_cfg_data_string[((sizeof(CONFIG_GNSS_CFG_DATA)-1) % 2) ? -1 : 0]; 

/**
 * @brief UART interface wrapper to get a character from the GNSS module,
 * wraps suart_read_byte()
 *
 * @return int see suart_read_byte()
 */
static int gnss_uart_get_char(void)
{
	return suart_read_byte(&gnss_uart_dev);
}

/**
 * @brief UART interface wrapper to write a string to the GNSS module, 
 * wraps suart_write_string()
 *
 * @param str the string
 * @return int see suart_write_string()
 */
static int gnss_uart_write_string(const char *str)
{
	return suart_write_string(&gnss_uart_dev, str);
}

/**
 * @brief UART interface wrapper to write a byte to the GNSS module,
 * wraps suart_write_byte()
 *
 * @param c the character
 */
static void gnss_uart_write_byte(const char c)
{
	suart_write_byte(&gnss_uart_dev, (int) c);
}

/**
 * @brief UART interface function to write a number of bytes to the GNSS module,
 * calls gnss_uart_write_byte()
 *
 * @param str the bytestring
 * @param len number of bytes to write
 */
static void gnss_uart_write_bytes(const char *str, int len)
{
	for (int i = 0; i < len; i++) {
		gnss_uart_write_byte(str[i]);
	}
}

/**
 * @brief UART interface wrapper to clear the RX data FIFO, wraps
 * suart_purge_rx_fifo()
 *
 * @return int see suart_purge_rx_fifo()
 */
static int gnss_uart_clear_rx_buffer(void)
{
	return suart_purge_rx_fifo(&gnss_uart_dev);
}

/**
 * @brief UART interface wrapper to get RX data FIFO count, wraps
 * suart_poll()
 *
 * @return int see suart_poll()
 */
static int gnss_uart_poll_rx_buffer(void)
{
	return suart_poll(&gnss_uart_dev);
}

/**
 * @brief Parse the Kconfig byte string for device configuration and transmit it
 *
 */
static void gnss_send_cfg_data(void)
{
	char hex[3] = {0};
	for (int i = 0; i < sizeof(CONFIG_GNSS_CFG_DATA)/2; i++) {
		hex[0] = CONFIG_GNSS_CFG_DATA[2*i];
		hex[1] = CONFIG_GNSS_CFG_DATA[2*i+1];
		const char* end;
		int byte;
		// If parsing stopped early, an invalid character was involved
		if ((end = fromhex(hex, &byte)) != &hex[2]) {
			pp_error("%s(): provided configuration byte string contains non-hex character %c (0x%02x), abort parsing\n", __func__, *end, *end);
			return;
		}
		gnss_dev.write_byte((char)byte);
	}
}

/* -------------------------------------------------------------------------- */
/*                             Core functionality                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Sync the current utc time to hardware
 * @return int -1 on error, 0 on success
 */
static int gnss_sync_utc(void)
{
	if (gnss_latest_fix <= 0) {
		return -1;
	}

	// some modules appear to report an invalid time even though fix
	// is achieved, so check if the time parsed is plausible by
	// comparing against a suitable lower bound
	if (gnss_latest_utc < CONFIG_GNSS_RECENT_UTC_TIMESTAMP) {
		return -1;
	}

	dev_dbg("Acquired valid GNSS UTC: %s\n", format_time(gnss_latest_utc, TIME_FORMAT_SORTED));

	int tmp, system_offset;
	wrc_ptp_get_leapsec(&tmp /* dummy */, &system_offset);
	int64_t tai_timestamp = gnss_latest_utc + system_offset;
	dev_dbg("Derived TAI timestamp: %lld (UTC timestamp: %lld, leap seconds: %d)\n", tai_timestamp, gnss_latest_utc, system_offset);

	shw_pps_gen_set_time(tai_timestamp, 0, PPSG_SET_SEC);
	dev_dbg("Local time set to TAI timestamp, sync done!\n");

	return 0;
}

/**
 * @brief Parse the given NMEA sentence and update fix and UTC data if applicable.
 *
 * @param sentence The NMEA sentence to process.
 * @return nmea_sentence_type_t processed sentence type
 */
static nmea_sentence_type_t gnss_process_sentence(const char *sentence)
{
	nmea_sentence_type_t type = NMEA_ANY;

	if (!nmea_sentence_is_valid(sentence)) {
		dev_dbg("%s(): Invalid NMEA sentence checksum, ignoring \"%s\"\n", __func__, sentence);
		return type;
	}

	if (nmea_sentence_type_is(sentence, NMEA_GGA)) {
		gnss_latest_fix = nmea_gga_parse_hasfix(sentence);

		// No fix for more than GNSS_FIX_TIMEOUT_MS means data is not valid anymore
		tmo_init(&tmo_fix, GNSS_FIX_TIMEOUT_MS);
		type = NMEA_GGA;
	} else if (nmea_sentence_type_is(sentence, NMEA_RMC)) {
		gnss_latest_utc = nmea_rmc_parse_utc(sentence);
		type = NMEA_RMC;
	}

	return type;
}

void gnss_init(void)
{
	// configure GNSS interface
#ifdef CONFIG_GNSS_INTF_UART
	suart_init(&gnss_uart_dev, BASE_GNSS_UART, CONFIG_GNSS_UART_BAUDRATE);
	gnss_dev.get_char = gnss_uart_get_char;
	gnss_dev.write_string = gnss_uart_write_string;
	gnss_dev.write_byte = gnss_uart_write_byte;
	gnss_dev.write_bytes = gnss_uart_write_bytes;
	gnss_dev.clear_rx_buffer = gnss_uart_clear_rx_buffer;
	gnss_dev.poll_rx_buffer = gnss_uart_poll_rx_buffer;
#else
	//additional interface types (e.g. I2C) should go here
#endif

	// apply GNSS module configuration
#ifdef CONFIG_GNSS_GENERIC
	// NOP
#elif CONFIG_GNSS_UBLOX_LEA_M8F
	/* u-blox LEA-M8F specific configuration messages generated with pygpsclient,
	 * Stored as file and dumped using $ hexdump -ve '"0x" 1/1 "%02X, "'
	 *
	 *  - string 1: configure timepulse output as PPS signal
	 *  - string 2: set GPS as main talker ID
	 *  - string 3+: disable all factory-default sentences, enable GGA and RMC
	 */
	static const char msg_ubx_lea_m8f_init[] = {
		0xB5, 0x62, 0x06, 0x31, 0x20, 0x00, 0x01, 0x01, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x40, 0x42, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x00, 0x00, 0x00, 0x36, 0x6E,
		0xB5, 0x62, 0x06, 0x17, 0x12, 0x00, 0x00, 0x40, 0x00, 0x06, 0x76, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEC, 0xE6,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x38,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x46,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x31,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x5B,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x04, 0x37,
		0xB5, 0x62, 0x06, 0x01, 0x08, 0x00, 0xF0, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x08, 0x53};

	gnss_dev.write_bytes(msg_ubx_lea_m8f_init, sizeof(msg_ubx_lea_m8f_init));
#endif

	// send custom configuration byte string to gnss device
	gnss_send_cfg_data();
}

int gnss_poll(void)
{
	static char sentence[NMEA_SENTENCE_MAXLEN + 1] = { 0 };
	static size_t sentence_len = 0;
	static parser_state_t parser_state = NMEA_WAIT_DELIM_START;

	// invalidate data if there hasn't been a valid fix for some time
	if (tmo_expired(&tmo_fix)) {
		gnss_latest_fix = -1;
		gnss_latest_utc = -1;
	}

	// Check if RX hardware buffer is completely full - if so, recent data may
	// have been dropped. Purge both hardware and software buffer to avoid
	// parsing outdated information.
	if (gnss_dev.poll_rx_buffer() == UART_RX_BUFFER_SIZE) {
		gnss_dev.clear_rx_buffer();
		parser_state = NMEA_WAIT_DELIM_START;

		return 0;
	} else if (gnss_dev.poll_rx_buffer() == 0) {
		// Nothing new, return early

		return 0;
	}

	bool new_utc_received = false;
	int rx_char;
	while ((rx_char = gnss_dev.get_char()) >= 0) {
		const char c = (char)rx_char;
		if (print_raw) {
			pp_printf("%c", (char)rx_char);
		}

		switch (parser_state)
		{
			case NMEA_WAIT_DELIM_START:
				// start buffering sentence when encountering start delimiter
				if (c == NMEA_DELIM_START) {
					sentence[0] = c;
					sentence_len = 1;
					parser_state = NMEA_WAIT_DELIM_STOP_1;
				}
				break;

			case NMEA_WAIT_DELIM_STOP_1:
				sentence[sentence_len++] = c;

				// wait for first stop delimiter
				if (c == NMEA_DELIM_STOP_1) {
					parser_state = NMEA_WAIT_DELIM_STOP_2;
				} else if (c == NMEA_DELIM_START) {
					// Received a START symbol again, reset to beginning of sentence
					sentence[0] = c;
					sentence_len = 1;
				} else if (sentence_len >= NMEA_SENTENCE_MAXLEN - 2) {
					dev_dbg("%s(): encountered NMEA sentence that is too long, waiting for next sentence\n", __func__);
					parser_state = NMEA_WAIT_DELIM_START;
				}
				break;

			case NMEA_WAIT_DELIM_STOP_2:
				// wait for second stop delimiter, then validate sentence
				if (c == NMEA_DELIM_STOP_2) {
					sentence[sentence_len++] = c;
					sentence[sentence_len] = '\0';

					nmea_sentence_type_t type = gnss_process_sentence(sentence);
					if (type == NMEA_RMC) {
						new_utc_received = true;
					}
				} else {
					dev_dbg("%s(): encountered NMEA sentence that has invalid stop delimiter, waiting for next sentence\n", __func__);
				}
				parser_state = NMEA_WAIT_DELIM_START;
				break;

			default:
				break;
		}
	}

	if (sync_utc && new_utc_received && gnss_sync_utc() >= 0) {
		sync_utc = false;
	}

	return 1;
}

int64_t gnss_get_utc(void)
{
	return gnss_latest_utc;
}

int gnss_get_fix(void)
{
	return gnss_latest_fix;
}

void gnss_raw(bool enable)
{
	print_raw = enable;
}

void gnss_sync(void)
{
	sync_utc = true;
}
