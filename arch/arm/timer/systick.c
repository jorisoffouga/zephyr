/* systick.c - ARM systick device driver */

/*
 * Copyright (c) 2013-2015 Wind River Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Wind River Systems nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
DESCRIPTION
This module implements the VxMicro's CORTEX-M3 ARM's systick device driver.
It provides the standard VxMicro "system clock driver" interfaces.

The driver utilizes systick to provide kernel ticks.

\INTERNAL IMPLEMENTATION DETAILS
The systick device provides a 24-bit clear-on-write, decrementing,
wrap-on-zero counter. Only edge sensitive triggered interrupt is supported.

\INTERNAL PACKAGING DETAILS
The systick device driver is part of the microkernel in both a monolithic kernel
system and a split kernel system; it is not included in the nanokernel portion
of a split kernel.

The device driver is also part of a nanokernel-only system, but omits more
complex capabilities (such as tickless idle support) that are only used in
conjunction with a microkernel.
*/

#include <nanokernel.h>
#include <nanokernel/cpu.h>
#include <toolchain.h>
#include <sections.h>
#include <misc/__assert.h>
#include <clock_vars.h>
#include <drivers/system_timer.h>

#ifdef CONFIG_MICROKERNEL

#include <microkernel.h>
#include <cputype.h>

extern struct nano_stack K_Args;

#endif /* CONFIG_MICROKERNEL */

/* running total of timer count */
static uint32_t accumulatedCount = 0;

/*
 * A board support package's board.h header must provide definitions for the
 * following constants:
 *
 *    CONFIG_SYSTICK_CLOCK_FREQ
 *
 * This is the sysTick input clock frequency.
 */

#include <board.h>

/* defines */

/*
 * When GDB_INFO is enabled, the handler installed in the vector table
 * (__systick), can be found in systick_gdb.s. In this case, the handler
 * in this file becomes _Systick() and will be called by __systick.
 */
#ifdef CONFIG_GDB_INFO
#define _TIMER_INT_HANDLER _real_timer_int_handler
#else
#define _TIMER_INT_HANDLER _timer_int_handler
#endif

#ifdef CONFIG_TICKLESS_IDLE
#define TIMER_MODE_PERIODIC 0 /* normal running mode */
#define TIMER_MODE_ONE_SHOT 1 /* emulated, since sysTick has 1 mode */

#define IDLE_NOT_TICKLESS 0 /* non-tickless idle mode */
#define IDLE_TICKLESS 1     /* tickless idle  mode */
#endif			    /* CONFIG_TICKLESS_IDLE */

/* globals */

#ifdef CONFIG_INT_LATENCY_BENCHMARK
extern uint32_t _hw_irq_to_c_handler_latency;
#endif

#ifdef CONFIG_ADVANCED_POWER_MANAGEMENT
extern int32_t _NanoIdleValGet(void);
extern void _NanoIdleValClear(void);
extern void _SysPowerSaveIdleExit(int32_t ticks);
#endif /* CONFIG_ADVANCED_POWER_MANAGEMENT */

#ifdef CONFIG_TICKLESS_IDLE
extern int32_t _SysIdleElapsedTicks;
#endif /* CONFIG_TICKLESS_IDLE */

/* locals */

#ifdef CONFIG_TICKLESS_IDLE
static uint32_t __noinit defaultLoadVal; /* default count */
static uint32_t idleOrigCount = 0;
static uint32_t __noinit maxSysTicks;
static uint32_t idleOrigTicks = 0;
static uint32_t __noinit maxLoadValue;
static uint32_t __noinit timerIdleSkew;
static unsigned char timerMode = TIMER_MODE_PERIODIC;
static unsigned char idleMode = IDLE_NOT_TICKLESS;
#endif /* CONFIG_TICKLESS_IDLE */

#if defined(CONFIG_TICKLESS_IDLE) || \
	defined(CONFIG_SYSTEM_TIMER_DISABLE)

/*******************************************************************************
*
* sysTickStop - stop the timer
*
* This routine disables the systick counter.
*
* RETURNS: N/A
*
* \NOMANUAL
*/

static ALWAYS_INLINE void sysTickStop(void)
{
	union __stcsr reg;

	/*
	 * Disable the counter and its interrupt while preserving the
	 * remaining bits.
	 */
	reg.val = __scs.systick.stcsr.val;
	reg.bit.enable = 0;
	reg.bit.tickint = 0;
	__scs.systick.stcsr.val = reg.val;
}

#endif /* CONFIG_TICKLESS_IDLE || CONFIG_SYSTEM_TIMER_DISABLE */

#ifdef CONFIG_TICKLESS_IDLE

/*******************************************************************************
*
* sysTickStart - start the timer
*
* This routine enables the systick counter.
*
* RETURNS: N/A
*
* \NOMANUAL
*/

static ALWAYS_INLINE void sysTickStart(void)
{
	union __stcsr reg;

	/*
	 * Enable the counter, its interrupt and set the clock source to be
	 * the system clock while preserving the remaining bits.
	 */
	reg.val =
		__scs.systick.stcsr.val; /* countflag is cleared by this read */
	reg.bit.enable = 1;
	reg.bit.tickint = 1;
	reg.bit.clksource = 1;
	__scs.systick.stcsr.val = reg.val;
}

/*******************************************************************************
*
* sysTickCurrentGet - get the current counter value
*
* This routine gets the value from the timer's current value register.  This
* value is the 'time' remaining to decrement before the timer triggers an
* interrupt.
*
* RETURNS: the current counter value
*
* \NOMANUAL
*/
static ALWAYS_INLINE uint32_t sysTickCurrentGet(void)
{
	return __scs.systick.stcvr;
}

/*******************************************************************************
*
* sysTickReloadGet - get the reload/countdown value
*
* This routine returns the value from the reload value register.
*
* RETURNS: the counter's initial count/wraparound value
*
* \NOMANUAL
*/
static ALWAYS_INLINE uint32_t sysTickReloadGet(void)
{
	return __scs.systick.strvr;
}

#endif /* CONFIG_TICKLESS_IDLE */

/*******************************************************************************
*
* sysTickReloadSet - set the reload/countdown value
*
* This routine sets value from which the timer will count down and also
* sets the timer's current value register to zero.
* Note that the value given is assumed to be valid (i.e., count < (1<<24)).
*
* RETURNS: N/A
*
* \NOMANUAL
*/

static ALWAYS_INLINE void sysTickReloadSet(
	uint32_t count /* count from which timer is to count down */
	)
{
	/*
	 * Write the reload value and clear the current value in preparation
	 * for enabling the timer.
	 * The countflag in the control/status register is also cleared by
	 * this operation.
	 */
	__scs.systick.strvr = count;
	__scs.systick.stcvr = 0; /* also clears the countflag */
}

/*******************************************************************************
*
* _TIMER_INT_HANDLER - system clock tick handler
*
* This routine handles the system clock tick interrupt. A TICK_EVENT event
* is pushed onto the microkernel stack.
*
* The symbol for this routine is either _timer_int_handler (for normal
* system operation) or _real_timer_int_handler (when GDB_INFO is enabled).
*
* RETURNS: N/A
*
* \NOMANUAL
*/

void _TIMER_INT_HANDLER(void *unused)
{
	ARG_UNUSED(unused);

#ifdef CONFIG_INT_LATENCY_BENCHMARK
	uint32_t value = __scs.systick.val;
	uint32_t delta = __scs.systick.reload - value;

	if (_hw_irq_to_c_handler_latency > delta) {
		/* keep the lowest value observed */
		_hw_irq_to_c_handler_latency = delta;
	}
#endif

#ifdef CONFIG_ADVANCED_POWER_MANAGEMENT
	int32_t numIdleTicks;

	/*
	 * All interrupts are disabled when handling idle wakeup.
	 * For tickless idle, this ensures that the calculation and programming
	 * of
	 * the device for the next timer deadline is not interrupted.
	 * For non-tickless idle, this ensures that the clearing of the kernel
	 * idle
	 * state is not interrupted.
	 * In each case, _SysPowerSaveIdleExit is called with interrupts
	 * disabled.
	 */
	__asm__(" cpsid i"); /* PRIMASK = 1 */

#ifdef CONFIG_TICKLESS_IDLE
	/*
	 * If this a wakeup from a completed tickless idle or after
	 *  _timer_idle_exit has processed a partial idle, return
	 *  to the normal tick cycle.
	 */
	if (timerMode == TIMER_MODE_ONE_SHOT) {
		sysTickStop();
		sysTickReloadSet(defaultLoadVal);
		sysTickStart();
		timerMode = TIMER_MODE_PERIODIC;
	}

	/* set the number of elapsed ticks and announce them to the kernel */

	if (idleMode == IDLE_TICKLESS) {
		/* tickless idle completed without interruption */
		idleMode = IDLE_NOT_TICKLESS;
		_SysIdleElapsedTicks =
			idleOrigTicks + 1; /* actual # of idle ticks */
		nano_isr_stack_push(&K_Args, TICK_EVENT);
	} else {
		/*
		 * Increment the tick because _timer_idle_exit does not
		 * account for the tick due to the timer interrupt itself.
		 * Also, if not in tickless mode, _SysIdleElpasedTicks will be
		 * 0.
		 */
		_SysIdleElapsedTicks++;

		/*
		 * If we transition from 0 elapsed ticks to 1 we need to
		 * announce the
		 * tick event to the microkernel. Other cases will be covered by
		 * _timer_idle_exit.
		 */

		if (_SysIdleElapsedTicks == 1) {
			nano_isr_stack_push(&K_Args, TICK_EVENT);
		}
	}

	/* accumulate total counter value */
	accumulatedCount += defaultLoadVal * _SysIdleElapsedTicks;
#else  /* !CONFIG_TICKLESS_IDLE */
	/*
	 * No tickless idle:
	 * Update the total tick count and announce this tick to the kernel.
	 */
	accumulatedCount += sys_clock_hw_cycles_per_tick;

	nano_isr_stack_push(&K_Args, TICK_EVENT);
#endif /* CONFIG_TICKLESS_IDLE */

	numIdleTicks = _NanoIdleValGet(); /* get # of idle ticks requested */

	if (numIdleTicks) {
		_NanoIdleValClear(); /* clear kernel idle setting */

		/*
		 * Complete idle processing.
		 * Note that for tickless idle, nothing will be done in
		 * _timer_idle_exit.
		 */
		_SysPowerSaveIdleExit(numIdleTicks);
	}

	__asm__(" cpsie i"); /* re-enable interrupts (PRIMASK = 0) */

#else /* !CONFIG_ADVANCED_POWER_MANAGEMENT */

	/* accumulate total counter value */
	accumulatedCount += sys_clock_hw_cycles_per_tick;

#ifdef CONFIG_MICROKERNEL
	/*
	 * one more tick has occurred -- don't need to do anything special since
	 * timer is already configured to interrupt on the following tick
	 */
	nano_isr_stack_push(&K_Args, TICK_EVENT);
#else
	_nano_ticks++;

	if (_nano_timer_list) {
		_nano_timer_list->ticks--;

		while (_nano_timer_list && (!_nano_timer_list->ticks)) {
			struct nano_timer *expired = _nano_timer_list;
			struct nano_lifo *chan = &expired->lifo;
			_nano_timer_list = expired->link;
			nano_isr_lifo_put(chan, expired->userData);
		}
	}
#endif /* CONFIG_MICROKERNEL */
#endif /* CONFIG_ADVANCED_POWER_MANAGEMENT */

	extern void _ExcExit(void);
	_ExcExit();
}

#ifdef CONFIG_TICKLESS_IDLE

/*******************************************************************************
*
* sysTickTicklessIdleInit - initialize the tickless idle feature
*
* This routine initializes the tickless idle feature by calculating the
* necessary hardware-specific parameters.
*
* Note that the maximum number of ticks that can elapse during a "tickless idle"
* is limited by <defaultLoadVal>.  The larger the value (the lower the
* tick frequency), the fewer elapsed ticks during a "tickless idle".
* Conversely, the smaller the value (the higher the tick frequency), the
* more elapsed ticks during a "tickless idle".
*
* RETURNS: N/A
*
* \NOMANUAL
*/

static void sysTickTicklessIdleInit(void)
{
	/* enable counter, disable interrupt and set clock src to system clock
	 */
	union __stcsr stcsr = {.bit = {1, 0, 1, 0, 0, 0}};
	volatile uint32_t dummy; /* used to help determine the 'skew time' */

	/* store the default reload value (which has already been set) */
	defaultLoadVal = sysTickReloadGet();

	/* calculate the max number of ticks with this 24-bit H/W counter */
	maxSysTicks = 0x00ffffff / defaultLoadVal;

	/* determine the associated load value */
	maxLoadValue = maxSysTicks * defaultLoadVal;

	/*
	 * Calculate the skew from switching the timer in and out of idle mode.
	 * The following sequence is emulated:
	 *    1. Stop the timer.
	 *    2. Read the current counter value.
	 *    3. Calculate the new/remaining counter reload value.
	 *    4. Load the new counter value.
	 *    5. Set the timer mode to periodic/one-shot.
	 *    6. Start the timer.
	 *
	 * The timer must be running for this to work, so enable the
	 * systick counter without generating interrupts, using the processor
	 *clock.
	 * Note that the reload value has already been set by the caller.
	 */

	__scs.systick.stcsr.val |= stcsr.val;
	__asm__(" isb"); /* ensure the timer is started before reading */

	timerIdleSkew = sysTickCurrentGet(); /* start of skew time */

	__scs.systick.stcsr.val |= stcsr.val; /* normally sysTickStop() */

	dummy = sysTickCurrentGet(); /* emulate sysTickReloadSet() */

	/* emulate calculation of the new counter reload value */
	if ((dummy == 1) || (dummy == defaultLoadVal)) {
		dummy = maxSysTicks - 1;
		dummy += maxLoadValue - defaultLoadVal;
	} else {
		dummy = dummy - 1;
		dummy += dummy * defaultLoadVal;
	}

	/* _sysTickStart() without interrupts */
	__scs.systick.stcsr.val |= stcsr.val;

	timerMode = TIMER_MODE_PERIODIC;

	/* skew time calculation for down counter (assumes no rollover) */
	timerIdleSkew -= sysTickCurrentGet();

	/* restore the previous sysTick state */
	sysTickStop();
	sysTickReloadSet(defaultLoadVal);
}

/*******************************************************************************
*
* _timer_idle_enter - Place the system timer into idle state
*
* Re-program the timer to enter into the idle state for the given number of
* ticks. It is set to a "one shot" mode where it will fire in the number of
* ticks supplied or the maximum number of ticks that can be programmed into
* hardware. A value of -1 will result in the maximum number of ticks.
*
* RETURNS: N/A
*/

void _timer_idle_enter(int32_t ticks /* system ticks */
				)
{
	sysTickStop();

	/*
	 * We're being asked to have the timer fire in "ticks" from now. To
	 * maintain accuracy we must account for the remaining time left in the
	 * timer. So we read the count out of it and add it to the requested
	 * time out
	 */
	idleOrigCount = sysTickCurrentGet() - timerIdleSkew;

	if ((ticks == -1) || (ticks > maxSysTicks)) {
		/*
		 * We've been asked to fire the timer so far in the future that
		 * the
		 * required count value would not fit in the 24-bit reload
		 * register.
		 * Instead, we program for the maximum programmable interval
		 * minus one
		 * system tick to prevent overflow when the left over count read
		 * earlier
		 * is added.
		 */
		idleOrigCount += maxLoadValue - defaultLoadVal;
		idleOrigTicks = maxSysTicks - 1;
	} else {
		/* leave one tick of buffer to have to time react when coming
		 * back */
		idleOrigTicks = ticks - 1;
		idleOrigCount += idleOrigTicks * defaultLoadVal;
	}

	/*
	 * Set timer to virtual "one shot" mode - sysTick does not have multiple
	 * modes, so the reload value is simply changed.
	 */
	timerMode = TIMER_MODE_ONE_SHOT;
	idleMode = IDLE_TICKLESS;
	sysTickReloadSet(idleOrigCount);
	sysTickStart();
}

/*******************************************************************************
*
* _timer_idle_exit - handling of tickless idle when interrupted
*
* The routine, called by _SysPowerSaveIdleExit, is responsible for taking
* the timer out of idle mode and generating an interrupt at the next
* tick interval.  It is expected that interrupts have been disabled.
*
* Note that in this routine, _SysIdleElapsedTicks must be zero because the
* ticker has done its work and consumed all the ticks. This has to be true
* otherwise idle mode wouldn't have been entered in the first place.
*
* RETURNS: N/A
*/

void _timer_idle_exit(void)
{
	uint32_t count; /* timer's current count register value */

	if (timerMode == TIMER_MODE_PERIODIC) {
		/*
		 * The timer interrupt handler is handling a completed tickless
		 * idle
		 * or this has been called by mistake; there's nothing to do
		 * here.
		 */
		return;
	}

	sysTickStop();

	/* timer is in idle mode, adjust the ticks expired */

	count = sysTickCurrentGet();

	if ((count == 0) || (__scs.systick.stcsr.bit.countflag)) {
		/*
		 * The timer expired and/or wrapped around. Re-set the timer to
		 * its default value and mode.
		 */
		sysTickReloadSet(defaultLoadVal);
		timerMode = TIMER_MODE_PERIODIC;

		/*
		 * Announce elapsed ticks to the microkernel. Note we are
		 * guaranteed
		 * that the timer ISR will execute before the tick event is
		 * serviced,
		 * so _SysIdleElapsedTicks is adjusted to account for it.
		 */
		_SysIdleElapsedTicks = idleOrigTicks - 1;
		nano_isr_stack_push(&K_Args, TICK_EVENT);
	} else {
		uint32_t elapsed;   /* elapsed "counter time" */
		uint32_t remaining; /* remaining "counter time" */

		elapsed = idleOrigCount - count;

		remaining = elapsed % defaultLoadVal;

		/* ensure that the timer will interrupt at the next tick */

		if (remaining == 0) {
			/*
			 * Idle was interrupted on a tick boundary. Re-set the
			 * timer to
			 * its default value and mode.
			 */
			sysTickReloadSet(defaultLoadVal);
			timerMode = TIMER_MODE_PERIODIC;
		} else if (count > remaining) {
			/*
			 * There is less time remaining to the next tick
			 * boundary than
			 * time left for idle. Leave in "one shot" mode.
			 */
			sysTickReloadSet(remaining);
		}

		_SysIdleElapsedTicks = elapsed / defaultLoadVal;

		if (_SysIdleElapsedTicks) {
			/* Announce elapsed ticks to the microkernel */
			nano_isr_stack_push(&K_Args, TICK_EVENT);
		}
	}

	idleMode = IDLE_NOT_TICKLESS;
	sysTickStart();
}

#endif /* CONFIG_TICKLESS_IDLE */

/*******************************************************************************
*
* timer_driver - initialize and enable the system clock
*
* This routine is used to program the systick to deliver interrupts at the
* rate specified via the 'sys_clock_us_per_tick' global variable.
*
* RETURNS: N/A
*/
void timer_driver(int priority /* priority parameter is ignored by this driver
				  */
		  )
{
	/* enable counter, interrupt and set clock src to system clock */
	union __stcsr stcsr = {.bit = {1, 1, 1, 0, 0, 0}};

	ARG_UNUSED(priority);

	/*
	 * Determine the reload value to achieve the configured tick rate.
	 */

	/* systick supports 24-bit H/W counter */
	__ASSERT(sys_clock_hw_cycles_per_tick <= (1 << 24),
		 "sys_clock_hw_cycles_per_tick too large");
	sysTickReloadSet(sys_clock_hw_cycles_per_tick - 1);

#ifdef CONFIG_TICKLESS_IDLE

	/* calculate hardware-specific parameters for tickless idle */

	sysTickTicklessIdleInit();

#endif /* CONFIG_TICKLESS_IDLE */

#ifdef CONFIG_MICROKERNEL

	/* specify the kernel routine that will handle the TICK_EVENT event */

	task_event_set_handler(TICK_EVENT, K_ticker);

#endif /* CONFIG_MICROKERNEL */

	_ScbExcPrioSet(_EXC_SYSTICK, _EXC_IRQ_DEFAULT_PRIO);

	__scs.systick.stcsr.val = stcsr.val;
}

/*******************************************************************************
*
* timer_read - read the BSP timer hardware
*
* This routine returns the current time in terms of timer hardware clock cycles.
* Some VxMicro facilities (e.g. benchmarking code) directly call timer_read()
* instead of utilizing the 'timer_read_fptr' function pointer.
*
* RETURNS: up counter of elapsed clock cycles
*
* \INTERNAL WARNING
* systick counter is a 24-bit down counter which is reset to "reload" value
* once it reaches 0.
*/

uint32_t timer_read(void)
{
	return accumulatedCount + (__scs.systick.strvr - __scs.systick.stcvr);
}

#ifdef CONFIG_SYSTEM_TIMER_DISABLE

/*******************************************************************************
*
* timer_disable - stop announcing ticks into the kernel
*
* This routine disables the systick so that timer interrupts are no
* longer delivered.
*
* RETURNS: N/A
*/

void timer_disable(void)
{
	unsigned int key; /* interrupt lock level */
	union __stcsr reg;

	key = irq_lock();

	/* disable the systick counter and systick interrupt */

	sysTickStop();

	irq_unlock(key);
}

#endif /* CONFIG_SYSTEM_TIMER_DISABLE */
