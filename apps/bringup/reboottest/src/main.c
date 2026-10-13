#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

int main(void)
{
	printk("reboot test: rebooting in 15 s\n");
	for (int i = 15; i > 0; i--) {
		printk("%d\n", i);
		k_sleep(K_SECONDS(1));
	}
	printk("sys_reboot()\n");
	sys_reboot(SYS_REBOOT_COLD);
	printk("still here?!\n");
	return 0;
}
