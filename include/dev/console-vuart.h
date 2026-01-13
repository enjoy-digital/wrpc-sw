/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012-2019 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#ifndef __CONSOLE_VUART_H
#define __CONSOLE_VUART_H

#include <stdint.h>

#include "dev/console.h"

void console_vuart_init(struct console_device *dev, unsigned addr);

extern struct console_device console_vuart_dev;


#endif

