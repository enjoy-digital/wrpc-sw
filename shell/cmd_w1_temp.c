/*
 * Onewire generic interface
 * Alessandro Rubini, 2013 GNU GPL2 or later
 */
#include "wrc.h"
#include "shell.h"
#include "dev/w1.h"
#include "cmds.h"

#define BLEN 32
/* A shell command, for checking */
/**
 * @shellcommand w1
 * @shellusage w1
 * Lists devices connected to the 1-Wire bus.
 */
int cmd_w1(const char *args[])
{
	int i;
	struct w1_dev *d;
	int32_t temp;

	for (i = 0; i < W1_MAX_DEVICES; i++) {
		d = wrpc_w1_bus.devs + i;
		if (d->rom) {
			pp_printf("device %i: %08x%08x\n", i,
				  (int)(d->rom >> 32), (int)d->rom);
		temp = w1_read_temp(d, 0);
		pp_printf("temp: %d.%04d\n", (int) (temp >> 16),
			  (int)((temp & 0xffff) * 10 * 1000 >> 16));
		}
	}
	return 0;
}

