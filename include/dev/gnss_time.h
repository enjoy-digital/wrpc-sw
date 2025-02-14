/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 - 2025 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *         Arthur Finkelmann <arthur.finkelmann@missinglinkelectronics.com>
 *
 * Released according to the GNU GPL, version 2 or any later version,
 * OR according to the GNU LGPL, version 3.0 or any later version
 */

/* Retrieve date/time information from GNSS module */

#ifndef __GNSS_TIME_H
#define __GNSS_TIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "util.h"
#include "dev/simple_uart.h"

/**
 * @brief gnss device interface struct type with a generic interface; function
 * pointers are set depending on selected device interface
 *
 */
struct gnss_device {
	int (*get_char)(void);
	int (*write_string)(const char *str);
	void (*write_byte)(char c);
	void (*write_bytes)(const char *str, int len);
	int (*clear_rx_buffer)(void);
	int (*poll_rx_buffer)(void);
};

/**
 * @brief Set up interface for GNSS module and apply initial configuration 
 * 
 */
void gnss_init(void);

/**
 * @brief Set up polling for GNSS module
 *
 */
int gnss_poll(void);

/**
 * @brief Retrieve UTC timestamp from GNSS
 * 
 * @return int64_t timestamp on success, -1 on error
 */
int64_t gnss_get_utc(void);

/**
 * @brief Retrieve fix status from GNSS
 * @return int 1 on success, -1 on error
 */
int gnss_get_fix(void);

/**
 * @brief Enable raw printing of GNSS characters
 * @param enable enable raw printing of GNSS characters
 */
void gnss_raw(bool enable);

/**
 * @brief Sync HW UTC and TAI to GNSS time
 */
void gnss_sync(void);

#endif
