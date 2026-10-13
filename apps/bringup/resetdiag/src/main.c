#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>

#define PRM_RSTST      0x48307258U
#define CM_FCLKEN_PER  0x48005000U
#define CM_ICLKEN_PER  0x48005010U
#define CM_IDLEST_PER  0x48005020U
#define PER_EN_WDT3    BIT(12)

#define WDT2_BASE 0x48314000U
#define WDT3_BASE 0x49030000U
#define WCLR      0x24U
#define WCRR      0x28U
#define WLDR      0x2CU
#define WWPS      0x34U
#define WSPR      0x48U

static const char *const rst_names[] = {
	"GLOBAL_COLD", "GLOBAL_SW", "bit2", "bit3", "SECURITY_VIOL", "MPU_WD",
	"SECURE_WD", "EXTERNAL_WARM", "VDD1_VM", "VDD2_VM", "ICEPICK", "ICECRUSHER",
};

static void wdt_stop(uint32_t base)
{
	sys_write32(0xAAAAU, base + WSPR);
	while (sys_read32(base + WWPS) != 0U) {
	}
	sys_write32(0x5555U, base + WSPR);
	while (sys_read32(base + WWPS) != 0U) {
	}
}

int main(void)
{
	uint32_t rstst = sys_read32(PRM_RSTST);

	printk("reset diag on %s\n", CONFIG_BOARD_TARGET);
	printk("PRM_RSTST 0x%08x:", rstst);
	for (int i = 0; i < ARRAY_SIZE(rst_names); i++) {
		if ((rstst & BIT(i)) != 0U) {
			printk(" %s", rst_names[i]);
		}
	}
	printk("\n");
	/* clear so the next boot shows only the next reset */
	sys_write32(rstst, PRM_RSTST);

	sys_set_bits(CM_FCLKEN_PER, PER_EN_WDT3);
	sys_set_bits(CM_ICLKEN_PER, PER_EN_WDT3);
	k_busy_wait(100);

	printk("WDT2 WCLR 0x%08x WLDR 0x%08x\n", sys_read32(WDT2_BASE + WCLR),
	       sys_read32(WDT2_BASE + WLDR));
	printk("WDT3 WCLR 0x%08x WLDR 0x%08x\n", sys_read32(WDT3_BASE + WCLR),
	       sys_read32(WDT3_BASE + WLDR));

	for (int s = 0; ; s++) {
		printk("t=%2ds WDT2 %08x WDT3 %08x%s\n", s, sys_read32(WDT2_BASE + WCRR),
		       sys_read32(WDT3_BASE + WCRR), (s == 5) ? "  -> stopping WDT3" : "");
		if (s == 5) {
			wdt_stop(WDT3_BASE);
		}
		k_msleep(1000);
	}

	return 0;
}
