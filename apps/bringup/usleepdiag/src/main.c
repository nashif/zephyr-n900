#include <zephyr/kernel.h>

static volatile uint32_t batches;
static volatile uint32_t iters;

static void watcher(void *a, void *b, void *c)
{
	uint32_t last_iters = 0;

	for (;;) {
		k_busy_wait(2000000);
		if (iters == last_iters) {
			printk("STALL: no progress for 2 s at iteration %u, uptime %lld\n",
			       iters, k_uptime_get());
		}
		last_iters = iters;
	}
}

/* Lower priority than main so it only runs when main sleeps; busy-waits for detection */
K_THREAD_DEFINE(watch_tid, 1024, watcher, NULL, NULL, NULL, 14, 0, 0);

int main(void)
{
	printk("usleep diag: %u ticks/s, %u cycles/s\n", CONFIG_SYS_CLOCK_TICKS_PER_SEC,
	       sys_clock_hw_cycles_per_sec());

	for (;;) {
		uint32_t c0 = k_cycle_get_32();
		int64_t t0 = k_uptime_ticks();

		for (int i = 0; i < 1000; i++) {
			k_usleep(1);
			iters++;
		}

		uint32_t dc = k_cycle_get_32() - c0;
		int64_t dt = k_uptime_ticks() - t0;

		batches++;
		printk("batch %u: 1000 x k_usleep(1) = %u cycles (%u us/iter), %lld ticks\n",
		       batches, dc, (uint32_t)(((uint64_t)dc * 1000U) / sys_clock_hw_cycles_per_sec()), dt);
	}

	return 0;
}
