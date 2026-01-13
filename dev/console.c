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
#include "pp-printf.h"
#include "board.h"
#include "dev/simple_uart.h"
#include "dev/console.h"
#include "dev/console-uart.h"
#include "dev/console-vuart.h"

#include "netconsole.h"
#include "lib/syslog.h"

/* Default: no extra consoles */
#ifndef BOARD_EXTRA_CONSOLES
#define BOARD_EXTRA_CONSOLES 0
#endif

/* 2 consoles (uart + vuart) are always registered */
static struct console_device* console_devs[2 + BOARD_EXTRA_CONSOLES + HAS_NETCONSOLE + HAS_PUTS_SYSLOG];

void console_register_device( struct console_device *dev )
{
    int i;
    for(i = 0; i < ARRAY_SIZE(console_devs); i++)
    {
        if ( console_devs[i] == NULL )
        {
            console_devs[i] = dev;
            return;
        }
    }
}

int puts(const char *s)
{
    int i, rv = 0;

    for(i = 0; i < ARRAY_SIZE(console_devs); i++)
    {
	    struct console_device *con = console_devs[i];
        if(!con)
            continue;
        rv = con->put_string( con, s);
    }

    return rv;
}


int console_getc(void)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(console_devs); i++)
    {
        struct console_device *con = console_devs[i];
        if(!con)
            continue;
        /* continue if no get_char function implemented */
        if (!con->get_char)
            continue;
        int b = con->get_char( con );

        if( b > 0 )
            return b;
    }

    return -1;
}

void console_init(void)
{
    console_vuart_init(&console_vuart_dev, BASE_VUART);
    console_uart_init(&console_uart_dev, &console_uart_priv,
		      BASE_UART, CONSOLE_UART_BAUDRATE);

#ifdef CONFIG_IPMI_CONSOLE
    console_ipmi_init();
#endif

#ifdef CONFIG_NETCONSOLE
    console_netconsole_init();
#endif

#ifdef CONFIG_PUTS_SYSLOG
    console_syslog_init();
#endif
}
