/* Console scrolling speed */
#include <zephyr/kernel.h>

int main(void)
{
	uint32_t start;
	uint32_t us;

	printk("console scroll test\n");
	k_msleep(500);
	start = k_cycle_get_32();
	for (int i = 0; i < 200; i++) {
		printk("line %3d: The quick brown fox jumps over the lazy dog 0123456789\n", i);
	}
	us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
	printk("200 lines in %u ms, %u us per line\n", us / 1000U, us / 200U);
	for (int i = 0;; i++) {
		k_msleep(50);
		printk("tick %d, uptime %lld ms\n", i, k_uptime_get());
	}
	return 0;
}
