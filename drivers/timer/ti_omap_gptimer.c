/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * System timer on a TI OMAP3 general-purpose timer (GPTIMER), built on the
 * generic timer core. The counter runs freely over its 32 bits and the
 * match register, which compares for equality, raises the interrupt.
 */

#define DT_DRV_COMPAT ti_omap3430_timer

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#define TIMER_BASE DT_INST_REG_ADDR(0)
#define TIMER_IRQN DT_INST_IRQN(0)

#define GPT_TIOCP_CFG (TIMER_BASE + 0x10U)
#define GPT_TISTAT    (TIMER_BASE + 0x14U)
#define GPT_TISR      (TIMER_BASE + 0x18U)
#define GPT_TIER      (TIMER_BASE + 0x1CU)
#define GPT_TCLR      (TIMER_BASE + 0x24U)
#define GPT_TCRR      (TIMER_BASE + 0x28U)
#define GPT_TLDR      (TIMER_BASE + 0x2CU)
#define GPT_TWPS      (TIMER_BASE + 0x34U)
#define GPT_TMAR      (TIMER_BASE + 0x38U)
#define GPT_TSICR     (TIMER_BASE + 0x40U)

#define TIOCP_CFG_SOFTRESET BIT(1)
#define TISTAT_RESETDONE    BIT(0)
#define TI_MAT              BIT(0)
#define TCLR_ST             BIT(0)
#define TCLR_AR             BIT(1)
#define TCLR_CE             BIT(6)
#define TSICR_POSTED        BIT(2)

/* TWPS: write pending to the register */
#define TWPS_TCLR BIT(0)
#define TWPS_TCRR BIT(1)
#define TWPS_TLDR BIT(2)
#define TWPS_TMAR BIT(4)

BUILD_ASSERT(DT_INST_PROP(0, clock_frequency) == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "GPTIMER clock-frequency must match SYS_CLOCK_HW_CYCLES_PER_SEC");

#if defined(CONFIG_TEST)
const int32_t z_sys_timer_irq_for_test = TIMER_IRQN;
#endif

/*
 * The timer runs in posted mode: a write to a functional clock register
 * returns at once and completes a few cycles of that clock later. Only a
 * second write to the same register has to wait for the first to land.
 */
static void gpt_write(uint32_t val, mm_reg_t reg, uint32_t pending)
{
	while ((sys_read32(GPT_TWPS) & pending) != 0U) {
	}
	sys_write32(val, reg);
}

#define TIMER_CORE_BACKEND_COMPARE_EXACT
#define TIMER_CORE_COUNTER_WIDTH     32
/* Covers the posted match write landing in the functional clock domain */
#define TIMER_CORE_ALARM_LEAD_CYCLES 64

static inline uint32_t timer_driver_cycle_get(void)
{
	return sys_read32(GPT_TCRR);
}

static inline void timer_driver_set_compare(uint32_t cycles)
{
	gpt_write(cycles, GPT_TMAR, TWPS_TMAR);
}

#include "system_timer_generic.h"

static void gptimer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	sys_write32(TI_MAT, GPT_TISR);
	timer_core_announce();
}

static int sys_clock_driver_init(void)
{
	sys_write32(TIOCP_CFG_SOFTRESET, GPT_TIOCP_CFG);
	while ((sys_read32(GPT_TISTAT) & TISTAT_RESETDONE) == 0U) {
	}

	sys_write32(TSICR_POSTED, GPT_TSICR);

	/* Free-running: reload 0 on overflow */
	gpt_write(0U, GPT_TLDR, TWPS_TLDR);
	gpt_write(0U, GPT_TCRR, TWPS_TCRR);
	sys_write32(TI_MAT, GPT_TISR);
	sys_write32(TI_MAT, GPT_TIER);
	gpt_write(TCLR_ST | TCLR_AR | TCLR_CE, GPT_TCLR, TWPS_TCLR);

	IRQ_CONNECT(TIMER_IRQN, DT_INST_IRQ(0, priority), gptimer_isr, NULL, 0);
	timer_core_init();
	irq_enable(TIMER_IRQN);

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
