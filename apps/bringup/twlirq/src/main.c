#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/reboot.h>

#define INTC_ITR0 0x48200080U	/* raw interrupt inputs 0-31 */

static const struct device *i2c;

static void dump(const char *name, uint8_t addr, uint8_t reg, int n)
{
	uint8_t v[4] = {0};
	int ret = i2c_burst_read(i2c, addr, reg, v, n);

	printk("%-14s %02x:%02x =", name, addr, reg);
	for (int i = 0; i < n; i++) {
		printk(" %02x", v[i]);
	}
	printk("%s\n", ret ? " (read failed)" : "");
}

int main(void)
{
	i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	for (int pass = 0; pass < 2; pass++) {
		printk("--- pass %d: sys_nirq raw %s\n", pass,
		       (sys_read32(INTC_ITR0) & BIT(7)) ? "ASSERTED" : "idle");
		dump("PIH_ISR_P1", 0x49, 0x81, 1);
		dump("GPIO IMR1A", 0x49, 0xB4, 3);
		dump("GPIO ISR1A", 0x49, 0xB1, 3);
		dump("KEYP IMR1", 0x4A, 0xE4, 1);
		dump("KEYP ISR1", 0x4A, 0xE3, 1);
		dump("MADC IMR1", 0x4A, 0x62, 1);
		dump("MADC ISR1", 0x4A, 0x61, 1);
		dump("BCI IMR1A/B", 0x4A, 0xBB, 1);
		dump("BCI IMR1B", 0x4A, 0xBF, 1);
		dump("BCI ISR1A", 0x4A, 0xB9, 1);
		dump("PWR IMR1", 0x4B, 0x2F, 1);
		dump("PWR ISR1", 0x4B, 0x2E, 1);
		dump("USB EN_RISE", 0x48, 0x0D, 1);
		dump("USB EN_FALL", 0x48, 0x10, 1);
		k_sleep(K_SECONDS(3));
	}

	printk("rebooting in 20 s\n");
	k_sleep(K_SECONDS(20));
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
