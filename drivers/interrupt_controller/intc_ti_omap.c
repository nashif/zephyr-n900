/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI OMAP3 / AM335x interrupt controller (INTCPS). Serves as the custom
 * interrupt controller of the Cortex-A core: it implements the z_soc_irq_*
 * API used by the AArch32 interrupt entry code.
 */

#define DT_DRV_COMPAT ti_omap3_intc

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#define INTC_BASE DT_INST_REG_ADDR(0)

#define INTC_SYSCONFIG     (INTC_BASE + 0x10U)
#define INTC_SYSSTATUS     (INTC_BASE + 0x14U)
#define INTC_SIR_IRQ       (INTC_BASE + 0x40U)
#define INTC_CONTROL       (INTC_BASE + 0x48U)
#define INTC_THRESHOLD     (INTC_BASE + 0x68U)
#define INTC_ITR(n)        (INTC_BASE + 0x80U + ((n) * 0x20U))
#define INTC_MIR_CLEAR(n)  (INTC_BASE + 0x88U + ((n) * 0x20U))
#define INTC_MIR_SET(n)    (INTC_BASE + 0x8CU + ((n) * 0x20U))
#define INTC_ISR_SET(n)    (INTC_BASE + 0x90U + ((n) * 0x20U))
#define INTC_ISR_CLEAR(n)  (INTC_BASE + 0x94U + ((n) * 0x20U))
#define INTC_ILR(m)        (INTC_BASE + 0x100U + ((m) * 4U))

#define SYSCONFIG_SOFTRESET BIT(1)
#define SYSSTATUS_RESETDONE BIT(0)
#define SIR_ACTIVEIRQ_MASK  0x7FU
#define SIR_SPURIOUS_MASK   0xFFFFFF80U
#define CONTROL_NEWIRQAGR   BIT(0)
#define THRESHOLD_DISABLED  0xFFU
#define ILR_PRIORITY_MAX    0x3FU
#define ILR_PRIORITY_SHIFT  2U

#define NUM_BANKS DIV_ROUND_UP(CONFIG_NUM_IRQS, 32U)

#define BANK(irq) ((irq) / 32U)
#define MASK(irq) BIT((irq) % 32U)

BUILD_ASSERT(CONFIG_NUM_IRQS <= DT_INST_PROP(0, ti_intc_size),
	     "CONFIG_NUM_IRQS exceeds the number of INTC inputs");

/*
 * Software copy of the enable state. The hardware mask also hides the
 * active line while its handler runs, so it does not reflect what
 * z_soc_irq_enable() and z_soc_irq_disable() requested.
 */
static uint32_t irq_enabled[NUM_BANKS];

void z_soc_irq_init(void)
{
	sys_write32(SYSCONFIG_SOFTRESET, INTC_SYSCONFIG);
	while ((sys_read32(INTC_SYSSTATUS) & SYSSTATUS_RESETDONE) == 0U) {
	}

	for (uint32_t bank = 0U; bank < NUM_BANKS; bank++) {
		sys_write32(UINT32_MAX, INTC_MIR_SET(bank));
		irq_enabled[bank] = 0U;
	}

	for (uint32_t irq = 0U; irq < CONFIG_NUM_IRQS; irq++) {
		sys_write32(0U, INTC_ILR(irq));
	}

	sys_write32(THRESHOLD_DISABLED, INTC_THRESHOLD);
	sys_write32(CONTROL_NEWIRQAGR, INTC_CONTROL);
}

void z_soc_irq_enable(unsigned int irq)
{
	unsigned int key = irq_lock();

	irq_enabled[BANK(irq)] |= MASK(irq);
	sys_write32(MASK(irq), INTC_MIR_CLEAR(BANK(irq)));
	irq_unlock(key);
}

void z_soc_irq_disable(unsigned int irq)
{
	unsigned int key = irq_lock();

	irq_enabled[BANK(irq)] &= ~MASK(irq);
	sys_write32(MASK(irq), INTC_MIR_SET(BANK(irq)));
	irq_unlock(key);
}

int z_soc_irq_is_enabled(unsigned int irq)
{
	return (irq_enabled[BANK(irq)] & MASK(irq)) != 0U;
}

void z_soc_irq_priority_set(unsigned int irq, unsigned int prio, uint32_t flags)
{
	ARG_UNUSED(flags);

	sys_write32(MIN(prio, ILR_PRIORITY_MAX) << ILR_PRIORITY_SHIFT, INTC_ILR(irq));
}

/*
 * Mask the active line and let the INTC sort the next interrupt, so other
 * lines can preempt the handler while the level-triggered source that
 * raised this one is still asserted. z_soc_irq_eoi() unmasks it again.
 */
unsigned int z_soc_irq_get_active(void)
{
	uint32_t sir = sys_read32(INTC_SIR_IRQ);
	unsigned int irq;

	if ((sir & SIR_SPURIOUS_MASK) == SIR_SPURIOUS_MASK) {
		sys_write32(CONTROL_NEWIRQAGR, INTC_CONTROL);
		barrier_dsync_fence_full();
		return CONFIG_NUM_IRQS;
	}

	irq = sir & SIR_ACTIVEIRQ_MASK;
	if (irq < CONFIG_NUM_IRQS) {
		sys_write32(MASK(irq), INTC_MIR_SET(BANK(irq)));
	}
	sys_write32(CONTROL_NEWIRQAGR, INTC_CONTROL);
	barrier_dsync_fence_full();

	return irq;
}

void z_soc_irq_eoi(unsigned int irq)
{
	if ((irq < CONFIG_NUM_IRQS) && ((irq_enabled[BANK(irq)] & MASK(irq)) != 0U)) {
		sys_write32(MASK(irq), INTC_MIR_CLEAR(BANK(irq)));
	}
}

void z_soc_irq_set_pending(unsigned int irq)
{
	sys_write32(MASK(irq), INTC_ISR_SET(BANK(irq)));
}

void z_soc_irq_clear_pending(unsigned int irq)
{
	sys_write32(MASK(irq), INTC_ISR_CLEAR(BANK(irq)));
}

bool z_soc_irq_is_pending(unsigned int irq)
{
	return (sys_read32(INTC_ITR(BANK(irq))) & MASK(irq)) != 0U;
}
