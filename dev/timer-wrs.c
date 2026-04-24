/*
 * This work is part of the White Rabbit project
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include "board.h"

#include "dev/syscon.h"

unsigned long timer_get_tics(void)
{
  return *(volatile unsigned long *) (BASE_TIMER);
}

void timer_delay(unsigned long tics)
{
	unsigned long t_end = timer_get_tics() + tics;

	while (time_before(timer_get_tics(), t_end))
	       ;
}
