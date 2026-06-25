/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */


#include "board.h"
#include "hw/wrc_syscon_regs.h"

#define SYSCON  ((volatile struct SYSC_WB *)BASE_SYSCON)

void timer_init(int enable)
{
    SYSCON->TCR |= SYSC_TCR_ENABLE;
}

uint32_t timer_get_tics()
{
    return SYSCON->TVR;
}
