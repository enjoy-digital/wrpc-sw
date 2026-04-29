/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2010 - 2015 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Grzegorz Daniluk <grzegorz.daniluk@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>

#include <wrc.h>
#include "board.h"
#include "softpll_ng.h"
#include "spll_hw.h"

#include "irq.h"

unsigned char spll_n_chan_ref, spll_n_chan_out;
int lj_periph_type_global = 0;
int scb_ljd_present_global = 0;	/* Low-jitter Daughterboard presence indicator */
int periph_id_global = 0;

static const char * const seq_states[] =
{
	[SEQ_START_EXT] = "start-ext",
	[SEQ_WAIT_EXT] = "wait-ext",
	[SEQ_START_HELPER] = "start-helper",
	[SEQ_WAIT_HELPER] = "wait-helper",
	[SEQ_START_MAIN] = "start-main",
	[SEQ_WAIT_MAIN] = "wait-main",
	[SEQ_DISABLED] = "disabled",
	[SEQ_READY] = "ready",
	[SEQ_CLEAR_DACS] = "clear-dacs",
	[SEQ_WAIT_CLEAR_DACS] = "wait-clear-dacs",
};
#define SEQ_STATES_NR  ARRAY_SIZE(seq_states)

static const char * const modes_name[] =
{
	[SPLL_MODE_DISABLED] = "disabled",
	[SPLL_MODE_GRAND_MASTER] = "grandmaster",
	[SPLL_MODE_FREE_RUNNING_MASTER] = "freemaster",
	[SPLL_MODE_SLAVE] = "slave"
};

volatile struct softpll_state softpll;

static volatile int ptracker_mask = 0;
/* fixme: should be done by spll_init() but spll_init is called to
 * switch modes (and we won't like messing around with ptrackers
 * there) */

static inline void start_ptrackers(struct softpll_state *s)
{
	int i;
	for (i = 0; i < spll_n_chan_ref; i++)
		if (ptracker_mask & (1 << i))
			ptracker_start(&s->ptrackers[i]);
}

static inline void update_ptrackers(struct softpll_state *s, int tag_value, int tag_source)
{
	int i;

	/* Ptracker for reference channels */
	if(tag_source <= spll_n_chan_ref)
		ptrackers_update(s->ptrackers, tag_value, tag_source);

	/* Ptracker for auxilliary channels (monitor only) */
	for( i = 0; i < spll_n_chan_out - 1; i++ )
	{
		struct spll_aux_state *aux = &s->aux[i];
		if( aux->mode != SPLL_AUX_MODE_PHASE_MONITOR )
			continue;

		if( tag_source == spll_n_chan_ref + i + 1)
		{
			aux->pll.tracker.dbg_channel = i;
			ptrackers_update( &aux->pll.tracker, tag_value, 0 );
		}
	}
}

static inline void sequencing_fsm(struct softpll_state *s, int tag_value, int tag_source)
{
	if( tag_source == spll_n_chan_ref ) // main osc
		s->tag_count++;
	else if ( tag_source == 0 ) // ref 0
		s->ref_count++;

	switch (s->seq_state) {
		/* State "Disabled". Entered when the whole PLL is off */
		case SEQ_DISABLED:
			break;

		/* State "Clear DACs": initial SPLL sequnencer state. Brings both DACs (not the AUXs) to the default values
		   prior to starting the SPLL. */
		case SEQ_CLEAR_DACS:
		{
			/* Helper always starts at the maximum value (to make sure it locks on positive offset */
			spll_write_helper_dac (s->helper.pi.y_max);

			/* Main starts at midscale */
			spll_write_dac (0, (s->mpll.pi.y_max + s->mpll.pi.y_min) / 2);

			/* we need tags from at least one channel, so that the IRQ that calls this function
			   gets called again */
			spll_enable_tagger(MAIN_CHANNEL, 1);

			s->dac_timeout = timer_get_tics() + TICS_PER_SECOND / 20;
			s->seq_state = SEQ_WAIT_CLEAR_DACS;

			break;
		}

		/* State "Wait until DACs have been cleared". Makes sure the VCO control inputs have stabilized before starting the PLL. */
		case SEQ_WAIT_CLEAR_DACS:
		{
			if (time_after(timer_get_tics(), s->dac_timeout))
			{
				if(s->mode == SPLL_MODE_GRAND_MASTER)
					s->seq_state = SEQ_START_EXT;
				else
					s->seq_state = SEQ_START_HELPER;
			}
			break;
		}

		/* State "Start external reference PLL": starts up BB PLL for locking local reference to 10 MHz input */
		case SEQ_START_EXT:
		{
			spll_enable_tagger(MAIN_CHANNEL, 0);
			external_start(&s->ext);

			s->seq_state = SEQ_WAIT_EXT;
			break;
		}

		/* State "Wait until we are locked to external 10MHz clock" */
		case SEQ_WAIT_EXT:
		{
			if (external_locked(&s->ext)) {
				start_ptrackers(s);
				s->seq_state = SEQ_READY;
				spll_set_channel_status(s->mpll.id_ref, 1);
			}
			break;
		}

		/* Once the DAC are on and stable, start helper PLL */
		case SEQ_START_HELPER:
		{
			helper_start(&s->helper);

			s->seq_state = SEQ_WAIT_HELPER;
			break;
		}

		case SEQ_WAIT_HELPER:
		{
			if (s->helper.ld.locked && s->helper.ld.lock_changed)
			{
				if (s->mode == SPLL_MODE_SLAVE)
				{
					s->seq_state = SEQ_START_MAIN;
				} else {
					/* Free running master, no need to
					   lock the main clock */
					start_ptrackers(s);
					s->seq_state = SEQ_READY;
					spll_set_channel_status(s->mpll.id_ref, 1);
				}
			}
			break;
		}

		case SEQ_START_MAIN:
		{
			mpll_start(&s->mpll);
			s->seq_state = SEQ_WAIT_MAIN;
			break;
		}

		case SEQ_WAIT_MAIN:
		{
			if (s->mpll.locked)
			{
				start_ptrackers(s);
				s->seq_state = SEQ_READY;
				spll_set_channel_status(s->mpll.id_ref, 1);
			}
			break;
		}

		case SEQ_READY:
		{
			if ((s->mode == SPLL_MODE_GRAND_MASTER && !external_locked(&s->ext))
			    || !s->helper.ld.locked
			    || (s->mode == SPLL_MODE_SLAVE && !s->mpll.locked)) {
				s->delock_count++;
				s->seq_state = SEQ_CLEAR_DACS;
				spll_set_channel_status(s->mpll.id_ref, 0);
			}
			break;
		}
	}
}

static inline void update_loops(struct softpll_state *s, int tag_value, int tag_source)
{

	helper_update(&s->helper, tag_value, tag_source);

	if(s->helper.ld.locked)
	{
		mpll_update(&s->mpll, tag_value, tag_source);

		if(s->seq_state == SEQ_READY) {
			if(s->mode == SPLL_MODE_SLAVE) {
				int i;
				for (i = 0; i < spll_n_chan_out - 1; i++)
					mpll_update(&s->aux[i].pll.dmtd, tag_value, tag_source);
			}

			update_ptrackers(s, tag_value, tag_source);
		}
	}
}

void spll_handle_tags(int tag_source, int tag_value)
{
	struct softpll_state *s = (struct softpll_state *) &softpll;

	if (0) {
		spll_debug(SPLL_DBG_SRC_RAW, SPLL_DBG_SIGNAL_SRC, tag_source, 0);
		spll_debug(SPLL_DBG_SRC_RAW, SPLL_DBG_SIGNAL_TAG, tag_value, 1);
	}

	sequencing_fsm(s, tag_value, tag_source);
	update_loops(s, tag_value, tag_source);
}

void spll_very_init(void)
{
	spll_ppsg_init();

	memset( (void *) &softpll, 0, sizeof(struct softpll_state ));
	softpll.mode = SPLL_MODE_DISABLED;

	spll_init_n_chan();

	softpll.mpll.gain_sched = NULL;

	helper_very_init((struct spll_helper_state *) &softpll.helper); // set up default PI gains/lock thresholds

	init_irq();
}

void spll_init(int mode, int slave_ref_channel, int flags)
{
	int i;

	struct softpll_state *s = (struct softpll_state *) &softpll;

	disable_irq();

	s->mode = mode;
	s->delock_count = 0;
	s->ref_count = s->tag_count = 0;

	/* Reset HW registers */
	spll_regs_init();

	if(mode == SPLL_MODE_DISABLED)
		s->seq_state = SEQ_DISABLED;
	else
		s->seq_state = SEQ_CLEAR_DACS;

	int helper_ref;

	if( mode == SPLL_MODE_SLAVE)
		helper_ref = slave_ref_channel; // Slave mode: lock the helper to an uplink port
	else
		helper_ref = spll_n_chan_ref; // Master/GM mode: lock the helper to the local ref clock

	helper_init(&s->helper, helper_ref);
	mpll_init(&s->mpll, slave_ref_channel, spll_n_chan_ref);

	for (i = 0; i < spll_n_chan_out - 1; i++) {
		mpll_init(&s->aux[i].pll.dmtd, slave_ref_channel, spll_n_chan_ref + i + 1);
		s->aux[i].seq_state = AUX_DISABLED;
	}

	for (i = 0; i < spll_n_chan_ref; i++)
		ptracker_init(&s->ptrackers[i], i, PTRACKER_AVERAGE_SAMPLES);

	if(mode == SPLL_MODE_GRAND_MASTER) {
		if (spll_is_ext_supported()) {
			s->ext.helper = &s->helper;
			s->ext.main = &s->mpll;
			external_init(&s->ext, spll_n_chan_ref + spll_n_chan_out);
		} else {
			pll_verbose("softpll: attempting to enable GM mode on non-GM hardware.\n");
			return;
		}
	}

	pll_verbose
	    ("softpll: mode %s, %d ref channels, %d out channels, ref: %d\n",
	     modes_name[mode], spll_n_chan_ref, spll_n_chan_out,
	     slave_ref_channel);

	/* Purge tag buffer */
	spll_purge_tags();

	/* No need to purge the debug queue, it's done by the wrpc tool.  */

	if(mode == SPLL_MODE_DISABLED)
		return;

	spll_regs_enable();

	enable_irq();
}

int spll_start_channel(int channel)
{
	struct softpll_state *s = (struct softpll_state *) &softpll;

	if (s->seq_state != SEQ_READY || !channel) {
		pll_verbose("Can't start channel %d, the PLL is not ready\n",
			  channel);
		return -1;
	}

	struct spll_aux_state *a = &s->aux[channel - 1];
	struct spll_main_state *m = &a->pll.dmtd;

	mpll_start(m);

	return 0;
}

void spll_stop_channel(int channel)
{
	struct softpll_state *s = (struct softpll_state *) &softpll;

	if (!channel)
		return;

	mpll_stop(&s->aux[channel - 1].pll.dmtd);
}

int spll_check_lock(int channel)
{
	if (!channel)
		return (softpll.seq_state == SEQ_READY);
	else
		return (softpll.seq_state == SEQ_READY)
		    && softpll.aux[channel - 1].pll.dmtd.phase_ld.locked;
}

static int32_t to_picos(int32_t units)
{
	return (int32_t) (((int64_t) units *
			   (int64_t) CLOCK_PERIOD_PICOSECONDS) >> HPLL_N);
}

/* Channel 0 = local PLL reference, 1...N = aux oscillators */
static void set_phase_shift(int channel, int32_t value_picoseconds)
{
	struct spll_main_state *st = (struct spll_main_state *)
	    (!channel ? &softpll.mpll : &softpll.aux[channel - 1].pll.dmtd);
	mpll_set_phase_shift(st, value_picoseconds);
	softpll.mpll_shift_ps = value_picoseconds;
}

void spll_set_phase_shift(int channel, int32_t value_picoseconds)
{
	int i;

	pll_verbose("set_phase_shift %d %ld\n", channel, value_picoseconds);

	if (channel == SPLL_ALL_CHANNELS) {
		set_phase_shift(0, value_picoseconds);
		for (i = 0; i < spll_n_chan_out - 1; i++)
			if (softpll.aux[i].seq_state == AUX_SLAVE_READY)
				set_phase_shift(i + 1, value_picoseconds);
	} else
		set_phase_shift(channel, value_picoseconds);
}

void spll_get_phase_shift(int channel, int32_t *current, int32_t *target)
{
	volatile struct spll_main_state *st = (struct spll_main_state *)
	    (!channel ? &softpll.mpll : &softpll.aux[channel - 1].pll.dmtd);
	int div = (DIVIDE_DMTD_CLOCKS_BY_2 ? 2 : 1);
	if (current)
		*current = to_picos(st->phase_shift_current * div);
	if (target)
		*target = to_picos(st->phase_shift_target * div);
}

int spll_read_ptracker(int channel, int32_t *phase_ps, int *enabled)
{
	volatile struct spll_ptracker_state *st = &softpll.ptrackers[channel];
	int phase = st->phase_val;
	if (phase < 0)
		phase += (1 << HPLL_N);
	else if (phase >= (1 << HPLL_N))
		phase -= (1 << HPLL_N);

	if (DIVIDE_DMTD_CLOCKS_BY_2) {
		phase <<= 1;
		phase &= (1 << HPLL_N) - 1;
	}

	*phase_ps = to_picos(phase);
	if (enabled)
		*enabled = ptracker_mask & (1 << st->id) ? 1 : 0;
	return st->ready;
}

void spll_set_ptracker_average_samples(int channel, int nsamples)
{
	struct softpll_state *s = (struct softpll_state *) &softpll;
	struct spll_ptracker_state *pt = &s->ptrackers[channel];

	disable_irq();
	pt->preserve_sign = 0;
	pt->ready = 0;
	pt->acc = 0;
	pt->avg_count = 0;
	pt->n_avg = nsamples;
	enable_irq();
}


void spll_get_num_channels(int *n_ref, int *n_out)
{
	if (n_ref)
		*n_ref = spll_n_chan_ref;
	if (n_out)
		*n_out = spll_n_chan_out;
}

void ptracker_show_stats(void)
{
	int ch;

	for (ch = 0; ch < MAX_PTRACKERS; ch++) {
		struct spll_ptracker_state *s =
			(struct spll_ptracker_state *)&softpll.ptrackers[ch];
		int32_t phase;
		spll_read_ptracker(ch, &phase, NULL);
		pp_printf("ptrack %d: en %d id %d ready %d phase %d (%d ps) avg %d\n",
			  ch, s->enabled, s->id, s->ready,
			  s->phase_val, (int)phase, s->n_avg);
	}
}

void spll_show_stats(void)
{
	struct softpll_state *s = (struct softpll_state *)&softpll;

	pp_printf("softpll: mode:%s seq:%s n_ref %d n_out %d\n",
		  modes_name[s->mode], seq_states[s->seq_state],
		  spll_n_chan_ref, spll_n_chan_out);

	if (s->mode != SPLL_MODE_DISABLED)
	{
		/* Needs several pp_printf to avoid buffer overflow on printf
		   buffer. */
		pp_printf("irqs:%u "
			  "ext-align:%d DelCnt:%u setp:%d refcnt:%u tagcnt:%u",
			  s->irq_count, s->ext.align_state,
			  s->delock_count, s->mpll.phase_shift_current,
			  s->ref_count, s->tag_count);
		pp_printf(" MFL%d MPL%d MY:%d m_kp:%d m_ki:%d m_sh:%d",
			  s->mpll.freq_ld.locked, s->mpll.phase_ld.locked,
			  s->mpll.pi.y,
			  s->mpll.pi.kp, s->mpll.pi.ki, s->mpll.pi.shift);
#ifndef CONFIG_IGNORE_HPLL
		pp_printf(" HL%d HY=%d h_kp:%d h_ki:%d h_sh:%d",
			  s->helper.ld.locked, s->helper.pi.y,
			  s->helper.pi.kp, s->helper.pi.ki, s->helper.pi.shift);
#endif
		pp_printf(" h_lock_ms:%d m_freq_lock_ms:%d m_phase_lock_ms:%d",
			  s->helper.last_lock_duration_ms,
			  s->mpll.last_freq_lock_duration_ms,
			  s->mpll.last_phase_lock_duration_ms);
		pp_printf(" main: en:%d ref:%d out:%d lck:%d freeze:%d",
			  s->mpll.enabled,
			  s->mpll.id_ref,
			  s->mpll.id_out,
			  s->mpll.locked,
			  s->mpll.vco_freeze);

		if (softpll.mpll.gain_sched)
			pp_printf(" gain_sched:%d/%d",
				  softpll.mpll.gain_sched->current_stage + 1,
				  softpll.mpll.gain_sched->n_stages );

		pp_printf("\n");
	}

	int ch;

	for (ch = 1; ch < spll_n_chan_out; ch++)
	{
		struct spll_aux_state *s = (struct spll_aux_state *) &softpll.aux[ch - 1];

		pp_printf("softpll: AUX%d:", ch-1);
		pp_printf(" ph %d seq %d en %d lock %d samples %d ref %d out %d ERR=%d Y=%d\n",
			  (int)s->phase_value,
			  s->seq_state,
			  s->pll.dmtd.enabled,
			  s->pll.dmtd.locked,
			  s->pll.dmtd.sample_n,
			  s->pll.dmtd.id_ref,
			  s->pll.dmtd.id_out,
			  s->pll.dmtd.pi.x,
			  s->pll.dmtd.pi.y );
	}

#ifndef CONFIG_TARGET_WR_SWITCH
	ptracker_show_stats();
#endif

}

int spll_shifter_busy(int channel)
{
	if (!channel)
		return mpll_shifter_busy((struct spll_main_state *)&softpll.mpll);
	else
		return mpll_shifter_busy((struct spll_main_state *)&softpll.aux[channel - 1].pll.dmtd);
}

void spll_enable_ptracker(int ref_channel, int enable)
{
	if (enable) {
		pll_verbose("Enabling ptracker channel: %d\n", ref_channel);
		ptracker_start((struct spll_ptracker_state *)&softpll.
			       ptrackers[ref_channel]);
		ptracker_mask |= (1 << ref_channel);

	} else {
		pll_verbose("Disabling ptracker tagger: %d\n", ref_channel);
		ptracker_mask &= ~(1 << ref_channel);
		if (ref_channel != softpll.mpll.id_ref)
			spll_enable_tagger(ref_channel, 0);
	}
}

int spll_get_delock_count(void)
{
	return softpll.delock_count;
}

static int spll_update_aux_clock(int ch)
{
	int done_sth = 0;
	struct spll_aux_state *s = (struct spll_aux_state *)&softpll.aux[ch - 1];

	if(s->seq_state != AUX_DISABLED && !spll_aux_locking_enabled(ch))
	{
		pll_verbose("softpll: disabled aux channel %d\n", ch);
		spll_stop_channel(ch);
		spll_set_channel_status(ch, 0);
		s->seq_state = AUX_DISABLED;
		return 1;
	}

	switch (s->seq_state) {
	case AUX_DISABLED:
		if (softpll.mpll.locked && spll_aux_locking_enabled(ch)) {
			if( s->mode == SPLL_AUX_MODE_SLAVE )
			{
				pll_verbose("softpll: enabled slave aux channel %d\n", ch);
				if( !spll_start_channel(ch) )
				{
					s->seq_state = AUX_LOCK_PLL;
				}
			}
			else if ( s->mode == SPLL_AUX_MODE_PHASE_MONITOR )
			{
				pll_verbose("softpll: enabled phase monitor on aux channel %d\n", ch);
				s->seq_state = AUX_WAIT_MONITOR_LOCK;
				ptracker_init( &s->pll.tracker, ch + spll_n_chan_ref, PTRACKER_AVERAGE_SAMPLES );
				ptracker_start( &s->pll.tracker );
			}
			done_sth = 1;
		}
		break;

	case AUX_WAIT_MONITOR_LOCK:
		if( s->pll.tracker.ready )
		{
			s->seq_state = AUX_MONITOR_READY;
			s->phase_value = s->pll.tracker.phase_val;
			spll_set_channel_status(ch, 1);
			done_sth = 1;
			break;
		}

	case AUX_MONITOR_READY:
		if (!softpll.mpll.locked)
		{
			pll_verbose("softpll: aux phase monitor channel %d disabled due to PLL LOS\n", ch);
			spll_set_channel_status(ch, 0);
			s->seq_state = AUX_DISABLED;
			done_sth = 1;
		}
		else
		{
			s->phase_value = s->pll.tracker.phase_val;
		}
		break;

	case AUX_LOCK_PLL:
		if (s->pll.dmtd.phase_ld.locked) {
			pll_verbose ("softpll: channel %d locked [aligning @ %d ps]\n", ch, softpll.mpll_shift_ps);
			set_phase_shift(ch, softpll.mpll_shift_ps);
			s->seq_state = AUX_ALIGN_PHASE;
			done_sth = 1;
		}
		break;

	case AUX_ALIGN_PHASE:
		if (!mpll_shifter_busy(&s->pll.dmtd)) {
			pll_verbose("softpll: channel %d phase aligned\n", ch);
			spll_set_channel_status(ch, 1);
			s->seq_state = AUX_SLAVE_READY;
			done_sth = 1;
		}
		break;

	case AUX_SLAVE_READY:
		if (!softpll.mpll.locked || !s->pll.dmtd.phase_ld.locked) {
			pll_verbose("softpll: aux channel %d or mpll lost lock\n", ch);
			spll_set_channel_status(ch, 0);
			s->seq_state = AUX_DISABLED;
			done_sth = 1;
		}
		break;
	}
	return done_sth;
}

/* Bottom half of the interrupt handler  */
int spll_update(void)
{
	int ret = 0;
	int ch;

	switch(softpll.mode) {
		case SPLL_MODE_GRAND_MASTER:
			ret = external_align_fsm(&softpll.ext);
			break;
	}

	for (ch = 1; ch < spll_n_chan_out; ch++)
	  ret |= spll_update_aux_clock(ch);

#ifdef CONFIG_TARGET_WR_SWITCH
	/* store statistics */
	stats->sequence++;
	stats->mode  = softpll.mode;
	stats->irq_cnt = softpll.irq_count;
	stats->seq_state = softpll.seq_state;
	stats->align_state = softpll.ext.align_state;
	stats->H_lock = softpll.helper.ld.locked;
	stats->M_lock = softpll.mpll.locked;
	stats->H_y = softpll.helper.pi.y;
	stats->M_y = softpll.mpll.pi.y;
	stats->del_cnt = softpll.delock_count;
	stats->ext_pps_latency_ps = softpll.ext.pps_latency_ps;
	stats->main_pll_kp = softpll.mpll.pi.kp;
	stats->main_pll_ki = softpll.mpll.pi.ki;
	stats->helper_pll_kp = softpll.helper.pi.kp;
	stats->helper_pll_ki = softpll.helper.pi.ki;

	stats->sequence++;
#endif

	return ret != 0;
}

struct spll_aux_clock_status spll_get_aux_status(int channel)
{
	struct spll_aux_clock_status rval;

	rval.flags = 0;
	rval.mode = 0;
	rval.phase = 0;

	if( channel < 0 || channel >= MAX_CHAN_AUX )
		return rval;

	int state = softpll.aux[channel].seq_state;

	rval.mode = softpll.aux[channel].mode;

	switch ( state )
	{
		case AUX_DISABLED:
			rval.flags = 0;
			break;
		case AUX_LOCK_PLL:
			rval.flags = SPLL_AUX_SLAVE_ENABLED;
			break;
		case AUX_WAIT_MONITOR_LOCK:
			rval.flags = SPLL_AUX_MONITOR_ENABLED;
			break;
		case AUX_MONITOR_READY:
			rval.flags = SPLL_AUX_MONITOR_ENABLED | SPLL_AUX_MONITOR_READY;
			break;
		case AUX_SLAVE_READY:
			rval.flags = SPLL_AUX_MONITOR_ENABLED | SPLL_AUX_SLAVE_LOCKED;
			break;
	}

	rval.phase = softpll.aux[channel].phase_value;

	return rval;
}

int spll_get_dac(int index)
{
	if (index < 0)
		return softpll.helper.pi.y;
	else if (index == 0)
		return softpll.mpll.pi.y;
	else if (index > 0)
		return softpll.aux[index - 1].pll.dmtd.pi.y;
	return 0;
}

void spll_set_dac(int index, int value)
{
	if (index < 0) {
		softpll.helper.pi.y = value;
		spll_write_helper_dac(value);
	} else {
		spll_write_dac(index, value & 0xffff);

		if (index == 0)
			softpll.mpll.pi.y = value;
		else if (index > 0)
			softpll.aux[index - 1].pll.dmtd.pi.y = value;
	}
}

void spll_set_gain_schedule( spll_gain_schedule_t* sch )
{
	disable_irq();
	softpll.mpll.gain_sched = sch;
	enable_irq();
}

void spll_set_pi_gain( int loop, int sched_stage, int kp, int ki, int shift )
{
	pll_verbose("set_pi_gain loop=%d stage=%d kp=%d ki=%d, shift=%d\n", loop, sched_stage, kp, ki, shift);
	disable_irq();
	switch(loop)
	{
		case SPLL_LOOP_HELPER:
			softpll.helper.pi.kp = kp;
			softpll.helper.pi.ki = ki;
			softpll.helper.pi.shift = shift;
			break;
		case SPLL_LOOP_MAIN:
			if( softpll.mpll.gain_sched && sched_stage < softpll.mpll.gain_sched->n_stages )
			{
				softpll.mpll.gain_sched->stages[sched_stage].ki = ki;
				softpll.mpll.gain_sched->stages[sched_stage].kp = kp;
				softpll.mpll.gain_sched->stages[sched_stage].shift = shift;
				if( softpll.mpll.gain_sched->current_stage == sched_stage )
				{
					softpll.mpll.pi.kp = kp;
					softpll.mpll.pi.ki = ki;
					softpll.mpll.pi.shift = shift;
				}
			}
			else
			{
				softpll.mpll.pi.kp = kp;
				softpll.mpll.pi.ki = ki;
				softpll.mpll.pi.shift = shift;
			}
			break;
		default:
			break;
	}
	enable_irq();
}

/* Simpler version of spll_set_pi_gain */
void spll_set_pi_gain_kp_ki(int loop, int kp, int ki)
{
	pll_verbose("set_pi_gain loop=%d kp=%d ki=%d\n", loop, kp, ki);
	disable_irq();
	switch(loop)
	{
		case SPLL_LOOP_HELPER:
			softpll.helper.pi.kp = kp;
			softpll.helper.pi.ki = ki;
			break;
		case SPLL_LOOP_MAIN:
			softpll.mpll.pi.kp = kp;
			softpll.mpll.pi.ki = ki;
			break;
		default:
			break;
	}
	enable_irq();
}

void spll_set_aux_mode( int channel, int mode )
{
	softpll.aux[channel].mode = mode;
}

int spll_pshifter_freeze(int freeze)
{
	softpll.mpll.ps_freeze = freeze;
	return 0;
}

int spll_vco_freeze(int freeze)
{
	softpll.mpll.vco_freeze = freeze;
	return 0;
}

void spll_update_ext_pps_latency_ps(int offset_ps)
{
	struct softpll_state *s = (struct softpll_state *)&softpll;
	s->ext.pps_latency_ps = offset_ps;
}
