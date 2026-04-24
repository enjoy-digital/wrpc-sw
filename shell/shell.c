/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Copyright (C) 2012 GSI (www.gsi.de)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Wesley W. Terpstra <w.terpstra@gsi.de>
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <wrc.h>
#include "sensors.h"
#include "dev/console.h"
#include "dev/dac_log.h"
#include "dev/syscon.h"
#include "dev/temp-fake.h"
#include "dev/temp-w1.h"
#include "dev/w1.h"
#include "dev/temperature.h"

#include "shell.h"
#include "storage.h"
#include "lib/syslog.h"
#include "board.h"
#include "cmds.h"

/* interactive shell state definitions */

#define SH_PROMPT 0
#define SH_INPUT 1
#define SH_EXEC 2
#define SH_EXEC_UI 3

#define ESCAPE_FLAG 0x100

#define KEY_LEFT (ESCAPE_FLAG | 68)
#define KEY_RIGHT (ESCAPE_FLAG | 67)
#define KEY_ENTER (13)
#define KEY_ENTER10 (10)
#define KEY_ESCAPE (27)
#define KEY_BACKSPACE (127)
#define KEY_DELETE (126)

static char cmd_buf[SH_MAX_LINE_LEN + 1];
static unsigned cmd_len = 0;
static unsigned char state = SH_PROMPT;

struct wrc_shell_cmd {
	const char *name;
	int (*exec) (const char *args[]);
};

static const struct wrc_shell_cmd cmds[] = {
#define WRC_COMMAND2(name, func) { #name, cmd_##func },
#include "cmds.h"
#undef WRC_COMMAND2
};

unsigned char shell_is_interacting;
int (*shell_ui_callback)(void);

#ifdef CONFIG_EXTENDED_CLI

static unsigned cmd_pos = 0;
static uint16_t current_key = 0;

static int insert(char c)
{
	if (cmd_len >= SH_MAX_LINE_LEN)
		return 0;

	if (cmd_pos != cmd_len)
		memmove(&cmd_buf[cmd_pos + 1], &cmd_buf[cmd_pos],
			cmd_len - cmd_pos);
	cmd_buf[cmd_pos] = c;
	cmd_pos++;
	cmd_len++;

	return 1;
}

static void delete(int where)
{
	memmove(&cmd_buf[where], &cmd_buf[where + 1], cmd_len - where);
	cmd_len--;
}

static void esc(char code)
{
	pp_printf("\033[1%c", code);
}

static void line_edit(int c)
{
	if (c == 27 || ((current_key & ESCAPE_FLAG) && c == '[')) {
		/* Escape sequence */
		current_key = ESCAPE_FLAG;
		return;
	}

	current_key |= c;

	switch (current_key) {
	case KEY_LEFT:
		if (cmd_pos > 0) {
			cmd_pos--;
			esc('D'); /* Move cursor backward */
		}
		break;
	case KEY_RIGHT:
		if (cmd_pos < cmd_len) {
			cmd_pos++;
			esc('C'); /* Move cursor forward */
		}
		break;
	case KEY_DELETE:
		if (cmd_pos != cmd_len) {
			delete(cmd_pos);
			esc('P'); /* Delete character */
		}
		break;
	case KEY_ENTER:
	case KEY_ENTER10:
		pp_printf("\n");
		state = SH_EXEC;
		break;

	case KEY_BACKSPACE:
		if (cmd_pos > 0) {
			esc('D'); /* Move cursor backward */
			esc('P'); /* Delete character */
			delete(cmd_pos - 1);
			cmd_pos--;
		}
		break;

	case '\t':
		break;

	default:
		if (insert(current_key)) {
			esc('@'); /* Insert space */
			pp_printf("%c", current_key);
		}
		break;

	}
	current_key = 0;
}

#else

static void line_edit(int c)
{
	switch (c) {
	case KEY_ENTER:
	case KEY_ENTER10:
		pp_printf("\n");
		cmd_buf[cmd_len] = 0;
		state = SH_EXEC;
		break;

	case KEY_BACKSPACE:
		if (cmd_len > 0) {
			pp_printf("\b \b");
			cmd_len--;
		}
		break;

	case '\t':
		break;

	default:
		if (cmd_len < SH_MAX_LINE_LEN) {
			cmd_buf[cmd_len++] = c;
			pp_printf("%c", c);
		}
		break;

	}
}
#endif

int sub_cmd(const char * const *cmds, unsigned len, const char *args[])
{
	unsigned i;

	if (args[0]) {
		for (i = 0; i < len; i++) {
			if (!strcmp (cmds[i], args[0]))
				return i;
		}
	}

	pp_printf ("usage:\n");
	for (i = 0; i < len; i++) {
		/* Hack: we know we are called from shell_exec, so
		   args[-1] is valid. */
		pp_printf(" %s %s\n", args[-1], cmds[i]);
	}
	return -1;
}

static int _shell_exec(void)
{
	const char *tokptr[SH_MAX_ARGS + 1];
	const struct wrc_shell_cmd *p;
	int n = 0, i = 0, rv;

	memset(tokptr, 0, sizeof(tokptr));

	while (1) {
		if (n >= SH_MAX_ARGS)
			break;

		/* Skip spaces at the start and before an argument.
		   Replace them with a null byte to mark end of string. */
		while (cmd_buf[i] == ' ')
			cmd_buf[i++] = 0;

		/* End of line. */
		if (!cmd_buf[i])
			break;

		/* New argument. */
		tokptr[n++] = &cmd_buf[i];

		/* Skip it. */
		while (cmd_buf[i] != ' ' && cmd_buf[i])
			i++;

		if (!cmd_buf[i])
			break;
	}

	if (!n)
		return 0;

	if (*tokptr[0] == '#')
		return 0;

	for (i = 0; i < ARRAY_SIZE(cmds); i++)
	{
		p = &cmds[i];
		if (!strcasecmp(p->name, tokptr[0])) {
			rv = p->exec(tokptr + 1);
			if (rv < 0)
				pp_printf("Command \"%s\": error %d\n",
					p->name, rv);
			return rv;
		}
	}

	pp_printf("Unrecognized command \"%s\".\n", tokptr[0]);
	return -EINVAL;
}

int shell_exec(const char *cmd)
{
	int i;

	if (cmd != cmd_buf)
		strncpy(cmd_buf, cmd, SH_MAX_LINE_LEN);
	cmd_len = strlen(cmd_buf);
	shell_is_interacting = 1;
	i = _shell_exec();
	shell_is_interacting = 0;
	/* clean cmd_buf */
	cmd_buf[0] = '\0';
	return i;
}

void shell_init()
{
	state = SH_PROMPT;
	shell_ui_callback = NULL;
}

int shell_interactive()
{
	int c;

	switch (state) {
	case SH_PROMPT:
		pp_printf("wrc# ");
#ifdef CONFIG_EXTENDED_CLI
		cmd_pos = 0;
#endif
		cmd_len = 0;
		state = SH_INPUT;
		return 1;

	case SH_INPUT:
		c = console_getc();
		if (c < 0)
			return 0;
		line_edit(c);
		return 1;

	case SH_EXEC:
		cmd_buf[cmd_len] = 0;
		_shell_exec();

// fixme: ugly hack, we should manage the shell FSM state in a cleaner way.
		if( state == SH_EXEC_UI )
			return 1;

		state = SH_PROMPT;
		return 1;


	case SH_EXEC_UI:
		c = console_getc();
		if (c == 'r')
			redraw_gui();

		if (!shell_ui_callback || shell_ui_callback() < 0 || c == 27 || c == 'q')
		{
			cmd_buf[cmd_len] = 0;
			state = SH_PROMPT;
		}
		return 1;
	}
	return 0;
}

#ifdef CONFIG_INIT_COMMAND
static const char shell_init_cmd[] = CONFIG_INIT_COMMAND;

static int build_init_readcmd(uint8_t *cmd, int maxlen)
{
	static const char *p = shell_init_cmd;
	int i;

	/* use semicolon as separator */
	for (i = 0; i < maxlen && p[i] && p[i] != ';'; i++)
		cmd[i] = p[i];
	cmd[i] = '\0';
	p += i;
	if (*p == ';')
		p++;
	if (i == 0) {
		/* it's the last call, roll-back *p to be ready for the next
		 * call */
		p = shell_init_cmd;
	}
	return i;
}
#endif

void shell_boot_script(void)
{
	int next = 0;

#ifdef CONFIG_INIT_COMMAND
	while (1) {
		cmd_len = build_init_readcmd((uint8_t *)cmd_buf,
					SH_MAX_LINE_LEN);
		if (!cmd_len)
			break;
		pp_printf("executing: %s\n", cmd_buf);
		shell_exec(cmd_buf);
	}
#endif

	while (CONFIG_HAS_FLASH_INIT) {
		int len = storage_init_readcmd((uint8_t *)cmd_buf,
					      SH_MAX_LINE_LEN, next);
		if (len <= 0) {
			if (next == 0)
				pp_printf("Empty init script...\n");
			break;
		}
		cmd_buf[len - 1] = 0;

		pp_printf("executing: %s\n", cmd_buf);
		shell_exec(cmd_buf);
		next = 1;
	}

	return;
}

void shell_show_build_init(void)
{
	int i = 0;

	pp_printf("-- built-in script --\n");
#ifdef CONFIG_INIT_COMMAND
	while (1) {
		cmd_len = build_init_readcmd((uint8_t *)cmd_buf,
					SH_MAX_LINE_LEN);
		if (!cmd_len)
			break;
		pp_printf("%s\n", cmd_buf);
		++i;
	}
#endif
	if (!i)
		pp_printf("(empty)\n");
}


void shell_activate_ui_command( int (*callback)(void) )
{
	shell_ui_callback = callback;
	state = SH_EXEC_UI;
	term_clear();
	cmd_len = 0;
}

int cmd_help(const char *args[])
{
	int i;
	pp_printf("Available commands:\n");

	for(i = 0; i < ARRAY_SIZE(cmds); i++) {
		pp_printf(" %s\n", cmds[i].name);
	}

	return 0;
}
