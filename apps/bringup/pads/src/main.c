#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/reboot.h>

static const struct {
	const char *name;
	uint16_t pad;
} pads[] = {
	{"gpio100 pen", 0x2118}, {"gpio104 rst", 0x2120}, {"gpio113 lock", 0x2136},
	{"mcspi1_clk", 0x21C8}, {"mcspi1_simo", 0x21CA}, {"mcspi1_somi", 0x21CC},
	{"mcspi1_cs0", 0x21CE}, {"mcspi1_cs1", 0x21D0}, {"mcspi1_cs2", 0x21D2},
};

int main(void)
{
	printk("pad configuration left by NOLO\n");
	for (int i = 0; i < ARRAY_SIZE(pads); i++) {
		uint16_t v = sys_read16(0x48000000U + pads[i].pad);

		printk("%-13s 0x%04x = 0x%04x mode%u %s%s%s\n", pads[i].name, pads[i].pad, v,
		       v & 7, (v & BIT(8)) ? "in " : "out ",
		       (v & BIT(3)) ? ((v & BIT(4)) ? "pullup " : "pulldown ") : "",
		       (v & BIT(14)) ? "wakeup" : "");
	}
	/* GPIO4: DATAIN, OE */
	printk("gpio4 DATAIN 0x%08x OE 0x%08x (pen bit4, rst bit8)\n",
	       sys_read32(0x49054038U), sys_read32(0x49054034U));
	printk("rebooting in 25 s\n");
	k_sleep(K_SECONDS(25));
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
