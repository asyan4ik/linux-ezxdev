/*
 * linux/include/asm-arm/arch-pxa/time.h
 *
 * Author:	Nicolas Pitre
 * Created:	Jun 15, 2001
 * Copyright:	MontaVista Software Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/*
 * Copyright (C) 2005 Motorola Inc.
 *
 * modified by e12051, for EZX platform
 */

#define NSEC_PER_MPUTICKS 308
static int timerInited = 0;

unsigned long __noinstrument machinecycles_to_usecs(unsigned long mputicks)
{
  /* round up to nearest usec */
  /* ROUNDUP((mputicks * NSEC_PER_MPUTICKS) / 1000) */
  return (mputicks * NSEC_PER_MPUTICKS * 2 + 1) / (1000*2);
}

unsigned long __noinstrument do_getmachinecycles(void)
{
  return (timerInited ? OSCR : 0);
}

static inline unsigned long pxa_get_rtc_time(void)
{
	return RCNR;
}

static int pxa_set_rtc(void)
{
	unsigned long current_time = xtime.tv_sec;

	if (RTSR & RTSR_ALE) {
		/* make sure not to forward the clock over an alarm */
		unsigned long alarm = RTAR;
		if (current_time >= alarm && alarm >= RCNR)
			return -ERESTARTSYS;
	}
	RCNR = current_time;
	return 0;
}

/* IRQs are disabled before entering here from do_gettimeofday() */
static unsigned long pxa_gettimeoffset (void)
{
	long ticks_to_match, elapsed, usec;

	/* Get ticks before next timer match */
	ticks_to_match = OSMR0 - OSCR;

	/* We need elapsed ticks since last match */
	elapsed = LATCH - ticks_to_match;

	/* don't get fooled by the workaround in pxa_timer_interrupt() */
	if (elapsed <= 0)
		return 0;

	/* Now convert them to usec */
	usec = (unsigned long)(elapsed*tick)/LATCH;

	return usec;
}

static void pxa_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	long flags;
	int next_match;

	do_profile(regs);

	/* Loop until we get ahead of the free running timer.
	 * This ensures an exact clock tick count and time acuracy.
	 * IRQs are disabled inside the loop to ensure coherence between
	 * lost_ticks (updated in do_timer()) and the match reg value, so we
	 * can use do_gettimeofday() from interrupt handlers.
	 *
	 * HACK ALERT: it seems that the PXA timer regs aren't updated right
	 * away in all cases when a write occurs.  We therefore compare with
	 * 8 instead of 0 in the while() condition below to avoid missing a
	 * match if OSCR has already reached the next OSMR value.
	 * Experience has shown that up to 6 ticks are needed to work around
	 * this problem, but let's use 8 to be conservative.  Note that this
	 * affect things only when the timer IRQ has been delayed by nearly
	 * exactly one tick period which should be a pretty rare event. 
	 */
	do {
		do_leds();
		do_set_rtc();
		save_flags_cli( flags );
		do_timer(regs);
		OSSR = OSSR_M0;  /* Clear match on timer 0 */
		next_match = (OSMR0 += LATCH);
		restore_flags( flags );
	} while( (signed long)(next_match - OSCR) <= 8 );
}

extern inline void setup_timer (void)
{
	gettimeoffset = pxa_gettimeoffset;
	set_rtc = pxa_set_rtc;
	timer_irq.handler = pxa_timer_interrupt;
	OSMR0 = 0;		/* set initial match at 0 */
	OSSR = 0xf;		/* clear status on all timers */
	setup_arm_irq(IRQ_OST0, &timer_irq);
	OIER |= OIER_E0;	/* enable match on timer 0 to cause interrupts */
	OSCR = 0;		/* initialize free-running timer, force first match */
	timerInited = 1;
}

