/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2010 - 2013 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* softpll_ng.h: public SoftPLL API header */

#ifndef __SOFTPLL_NG_H
#define __SOFTPLL_NG_H

#include <stdint.h>
#include "util.h"
#include "softpll_export.h"
#include "spll_defs.h"
#include "spll_common.h"
#include "spll_debug.h"
#include "spll_helper.h"
#include "spll_main.h"
#include "spll_ptracker.h"
#include "spll_external.h"

/* Note on channel naming:

 - ref_channel means a PHY recovered clock input. There can be one (as in
   WR core) or more (WR switch).

 - out_channel means an output channel, which represents PLL feedback signal
   from a local, tunable oscillator. Every SPLL implementation has at least
   one output channel, connected to the 125 / 62.5 MHz transceiver (WR)
   reference. This channel (MAIN_CHANNEL) has always out index 0 and is
   compared against all reference channels by the phase tracking mechanism.
*/

/* Number of reference/output channels. We don't plan to have more than one
   SoftPLL instantiation per project, so these can remain global. */
extern unsigned char spll_n_chan_ref, spll_n_chan_out;

/* Channels id:
   -1                                    : helper clock (for dmtd)
   0 - (n_chan_ref-1)                    : reference clocks (from RX)
   n_chan_ref .. n_chan_ref+n_chan_out-1 : main clock + auxilliary clocks
   n_chan_ref + n_chan_out ..            : external clocks (10Mhz)

   So for WR-Core,
   0 : rxclk
   1 : ref_clk
   2.. : aux clocks
*/
/* So the main clock channel id is spll_n_chan_ref. */
#define MAIN_CHANNEL (spll_n_chan_ref)

/* Shortcut for 'channels' parameter in various API functions to perform
   operation on all channels */
#define SPLL_ALL_CHANNELS 0xffffffff

/* Auxilliary input clocks can be configured in two modes: */
/* Aux clock is disciplined from the local WR time base */
#define SPLL_AUX_MODE_SLAVE 0
/* Aux clock phase is monitored by the softPLL using another reference clock */
#define SPLL_AUX_MODE_PHASE_MONITOR 1

/* Aux clock flags */
#define SPLL_AUX_SLAVE_ENABLED (1<<0) /* Locking the particular aux channel to the WR reference is enabled */
#define SPLL_AUX_SLAVE_LOCKED (1<<1)  /* The particular aux clock is already locked to WR reference */
#define SPLL_AUX_MONITOR_ENABLED (1<<2) /* The particilar aux clock phase is monitored against the local WR reference */
#define SPLL_AUX_MONITOR_READY (1<<3)

/* flags passed to spll_init() */
#define SPLL_FLAG_ALIGN_PPS (1<<0) /* enables rephasing of the local oscillator to the external PPS signal */

/* For the result of spll_get_aux_status. */
struct spll_aux_clock_status
{
	uint16_t flags;
	uint16_t mode;
	int phase;
};

/* 
Initializes the SoftPLL to work in mode (mode). Extra parameters depend on choice of the mode:
- for SPLL_MODE_GRAND_MASTER: flags == SPLL_FLAG_ALIGN_PPS value enables realignment of the WR reference rising edge to the 
  rising edge of 10 MHz external clock that comes immediately after a PPS pulse
- for SPLL_MODE_SLAVE: (ref_channel) indicates the reference channel to which we are locking our PLL. 
*/
void spll_init(int mode, int ref_channel, int flags);
void spll_very_init(void);

/* Disables the SoftPLL and cleans up stuff */
void spll_shutdown(void);

/* Returns number of reference and output channels implemented in HW. */
void spll_get_num_channels(int *n_ref, int *n_out);

/* Starts locking output channel (out_channel) */
int spll_start_channel(int out_channel);

/* Stops locking output channel (out_channel) */
void spll_stop_channel(int out_channel);

/* Returns non-zero if output channel (out_channel) is locked to a WR reference */
int spll_check_lock(int out_channel);

/* Sets phase setpoint for given output channel. */
void spll_set_phase_shift(int out_channel, int32_t value_picoseconds);

/* Retreives the current phase shift and desired setpoint for given output channel */
void spll_get_phase_shift(int out_channel, int32_t *current, int32_t *target);

/* Returns non-zero if the given output channel is busy phase shifting to a new preset */
int spll_shifter_busy(int out_channel);

/* Enables/disables phase tracking on channel (ref_channel). Phase is always measured between
   the WR local reference (out_channel 0) and ref_channel */
void spll_enable_ptracker(int ref_channel, int enable);

/* Reads tracked phase shift value for given reference channel */
int spll_read_ptracker(int ref_channel, int32_t *phase_ps, int *enabled);

/* Calls non-realtime update state machine. Must be called regularly (although
 * it is not time-critical) in the main loop of the program if aux clocks or
 * external reference are used in the design. */
int spll_update(void);

/* Returns the status of given aux clock output (SPLL_AUX_) */
struct spll_aux_clock_status spll_get_aux_status(int channel );

/* Debug/testing functions */

/* Returns how many time the PLL has de-locked since last call of spll_init() */
int spll_get_delock_count(void);

void spll_show_stats(void);
void ptracker_show_stats(void);

/* Sets VCXO tuning DAC corresponding to output (out_channel) to a given value */
void spll_set_dac(int out_channel, int value);

/* Returns current DAC sample value for output (out_channel) */
int spll_get_dac(int out_channel);

void spll_set_gain_schedule( spll_gain_schedule_t* sch );
void spll_set_pi_gain( int loop, int sched_stage, int kp, int ki, int shift );
void spll_set_pi_gain_kp_ki(int loop, int kp, int ki);

void spll_set_ptracker_average_samples(int channel, int nsamples);

void spll_update_ext_pps_latency_ps(int offset_ps);

int spll_get_debug_queue_samples( uint32_t *buf, unsigned count );
void spll_debug_queue_configure( int undersample, int coalsesce_threshold );

void spll_set_aux_mode( int channel, int mode );
void spll_set_aux_frequency_ratio( int channel, int div_ref, int div_fb );

int spll_is_ext_supported(void);

/*
 * Aux and main state:
 * used to be in .c file, but we need it here for memory dumping
 */

/* NOTE: Please increment WRPC_SHMEM_VERSION if you change this structure */
struct spll_aux_state {
	int mode; /* SPLL_AUX_MODE* */
	int seq_state;
#ifdef CONFIG_FRAC_SPLL
	/* Fractional pll ratio (0 if 1:1).  */
	int div_ref;
	int div_fb;
#endif
	int32_t phase_value;
	union {
		struct spll_main_state dmtd;
		struct spll_ptracker_state tracker;
		/* spll_external_state ch_bb */
	} pll;
};

/* NOTE: Please increment WRPC_SHMEM_VERSION if you change this structure */
struct softpll_state {
	int mode;
	int seq_state;
	uint32_t dac_timeout;
	int delock_count;
	unsigned irq_count;
	unsigned ref_count, tag_count;
	int32_t mpll_shift_ps;

	struct spll_helper_state helper;
	struct spll_external_state ext;
	struct spll_main_state mpll;
	struct spll_aux_state aux[MAX_CHAN_AUX];

	/* ptracker for each ref channel (recovered clock). */
	struct spll_ptracker_state ptrackers[MAX_PTRACKERS];
};

extern volatile struct softpll_state softpll;
extern int lj_periph_type_global;
extern int scb_ljd_present_global;
extern int periph_id_global;

#endif // __SOFTPLL_NG_H

