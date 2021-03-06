#ifndef __ASMARM_ARCH_TIMER_H
#define __ASMARM_ARCH_TIMER_H

#include <linux/ioport.h>

#include <asm/errno.h>
#include <linux/clocksource.h>

struct arch_timer {
	struct resource	res[2];
};

#ifdef CONFIG_ARM_ARCH_TIMER
#define ARCH_HAS_READ_CURRENT_TIMER
int arch_timer_register(struct arch_timer *);
int arch_timer_sched_clock_init(void);
struct timecounter *arch_timer_get_timecounter(void);
#else
static inline int arch_timer_register(struct arch_timer *at)
{
	return -ENXIO;
}

static inline int arch_timer_of_register(void)
{
	return -ENXIO;
}

static inline int arch_timer_sched_clock_init(void)
{
	return -ENXIO;
}

static inline struct timecounter *arch_timer_get_timecounter(void)
{
	return NULL;
}
#endif

#endif
