/*
 * Onewire generic interface
 * Alessandro Rubini, 2013 GNU GPL2 or later
 */
#include "wrc.h"
#include "shell.h"
#include "dev/w1.h"
#include "cmds.h"

#define BLEN 32

/**
 * @shellcommand w1w
 * @shellusage w1w <offset> <byte> [<byte> ...]
 * Writes decimal byte values to a 1-Wire EEPROM, up to 32 bytes.
 */
int cmd_w1w(const char *args[])
{
	struct w1_dev *w1_dev = w1_find_eeprom_device(&wrpc_w1_bus);
	int offset, i, blen;
	unsigned char buf[BLEN];

	if (!args[0] || !args[1] || w1_dev == NULL)
		return -1;
	offset = atoi(args[0]);
	for (i = 1, blen = 0; args[i] && blen < BLEN; i++, blen++) {
		buf[blen] = atoi(args[i]);
		pp_printf("offset %4i (0x%03x): %3i (0x%02x)\n",
			  offset + blen, offset + blen, buf[blen], buf[blen]);
	}
	i = w1_write_eeprom(w1_dev, offset, buf, blen);
	pp_printf("write(0x%x, %i): result = %i\n", offset, blen, i);
	return i == blen ? 0 : -1;
}

/**
 * @shellcommand w1r
 * @shellusage w1r <offset> <length>
 * Reads up to 32 bytes from a 1-Wire EEPROM. Both commands require
 * CONFIG_W1_EEPROM and a discoverable EEPROM device.
 */
int cmd_w1r(const char *args[])
{
	struct w1_dev *w1_dev = w1_find_eeprom_device(&wrpc_w1_bus);
	int offset, i, blen;
	unsigned char buf[BLEN];

	if (!args[0] || !args[1] || w1_dev == NULL)
		return -1;
	offset = atoi(args[0]);
	blen = atoi(args[1]);
	if (blen > BLEN)
		blen = BLEN;
	i = w1_read_eeprom(w1_dev, offset, buf, blen);
	pp_printf("read(0x%x, %i): result = %i\n", offset, blen, i);
	if (i <= 0 || i > blen) return -1;
	for (blen = 0; blen < i; blen++) {
		pp_printf("offset %4i (0x%03x): %3i (0x%02x)\n",
			  offset + blen, offset + blen, buf[blen], buf[blen]);
	}
	return i == blen ? 0 : -1;
}
