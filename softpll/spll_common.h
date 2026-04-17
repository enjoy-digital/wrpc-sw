/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2010 - 2013 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* spll_common.h - common data structures and functions used by the SoftPLL */

#ifndef __SPLL_COMMON_H
#define __SPLL_COMMON_H

#include <dev/syscon.h>

/* PI regulator state */
typedef struct {
	int ki, kp;		/* integral and proportional gains (1<<PI_FRACBITS == 1.0f) */
	int shift;		/* fractional bits shift factor (defaults to PI_FRACBITS) */
	int bias;		/* DC offset always added to the output */
	int64_t integrator;		/* current integrator value */
	int anti_windup;	/* when non-zero, anti-windup is enabled */
	int y_min;		/* min/max output range, used by clapming and antiwindup algorithms */
	int y_max;
	int x, y;		/* Current input (x) and output value (y) */
} spll_pi_t;

/* lock detector state */
typedef struct {
	int lock_cnt;		/* Lock sample counter */
	int lock_samples;	/* Number of samples below the (threshold) to assume that we are locked */
	int delock_samples;	/* Accumulated number of samples that causes the PLL go get out of lock.
				   delock_samples < lock_samples.  */
	int threshold;		/* Error threshold */
	int locked;		/* Non-zero: we are locked */
	int lock_changed;
} spll_lock_det_t;

typedef struct {
	int kp, ki, shift, lock_samples;
} spll_gain_schedule_item_t;

typedef struct {
	int n_stages;
	int current_stage;
	spll_gain_schedule_item_t stages[SPLL_GAIN_SCHED_MAX];
	int locked_d;
} spll_gain_schedule_t;

/* initializes the PI controller state. Currently almost a stub. */
void pi_init(spll_pi_t *pi);

/* Processes a single sample (x) with PI control algorithm
 (pi). Returns the value (y) to drive the actuator. */
int pi_update(spll_pi_t *pi, int x);

void ld_init(spll_lock_det_t *ld);
void ld_update(spll_lock_det_t *ld, int y);

#endif // __SPLL_COMMON_H
