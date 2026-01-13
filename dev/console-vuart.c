/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012-2020 CERN (www.cern.ch)
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <stdint.h>
#include <errno.h>
#include <stddef.h>

#include "util.h"
#include "board.h"
#include "hw/vuart_board_map.h"
#include "dev/console.h"
#include "dev/console-vuart.h"

struct console_device console_vuart_dev;

static int vuart_put_string(struct console_device* dev, const char *s)
{
    volatile struct vuart_board_map *regs =
	(volatile struct vuart_board_map *)dev->priv;
    char c;
    int count = 0;

    while ( (c = *s++) != 0 )
    {
	if( (dev->flags & CONSOLE_FLAGS_INSERT_CRLF) &&  c == '\n')
	    regs->TDR = '\r';

	regs->TDR = c;
        count++;
    }

    return count;
}

static int vuart_getc(struct console_device* dev)
{
    volatile struct vuart_board_map *regs =
	(volatile struct vuart_board_map *)dev->priv;
    uint32_t rdr = regs->RDR;

    if (rdr & VUART_BOARD_MAP_RDR_RDY)
	return (rdr & VUART_BOARD_MAP_RDR_DATA_MASK);
    else
	return -1;
}

void console_vuart_init(struct console_device *dev, unsigned addr)
{
    dev->flags = CONSOLE_FLAGS_MODE_TTY | CONSOLE_FLAGS_INSERT_CRLF;
    dev->priv = (void *)addr;
    dev->get_char = vuart_getc;
    dev->put_string = vuart_put_string;

    console_register_device(dev);
}

