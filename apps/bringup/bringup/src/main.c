#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/arch/arm/cortex_a_r/lib_helpers.h>
#include <soc.h>

static void ticker(void *a, void *b, void *c)
{
	for (;;) {
		k_msleep(500);
		printk("  [ticker] uptime %lld ms\n", k_uptime_get());
	}
}

K_THREAD_DEFINE(ticker_tid, 2048, ticker, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	uint32_t midr, cpsr;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));

	printk("Zephyr %s on %s\n", KERNEL_VERSION_STRING, CONFIG_BOARD_TARGET);
	printk("loaded at 0x%08x, linked at 0x%08x\n", omap3_load_addr,
	       (uint32_t)CONFIG_SRAM_BASE_ADDRESS);
	printk("boot SCTLR 0x%08x, now SCTLR 0x%08x\n", omap3_boot_sctlr, read_sctlr());
	printk("MIDR 0x%08x, CPSR 0x%08x\n", midr, cpsr);
	printk("timer %u Hz, %u ticks/s\n", sys_clock_hw_cycles_per_sec(),
	       CONFIG_SYS_CLOCK_TICKS_PER_SEC);

	for (int i = 0; ; i++) {
		uint32_t c0 = k_cycle_get_32();

		k_msleep(1000);
		printk("main: %d, 1000 ms sleep = %u cycles\n", i, k_cycle_get_32() - c0);
	}

	return 0;
}
