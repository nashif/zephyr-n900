#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>

#define PRM_CLKSEL       0x48306D40U	/* SYS_CLKIN_SEL [2:0] */
#define PRM_CLKSRC_CTRL  0x48307270U	/* SYSCLKDIV [7:6] */
#define CM_FCLKEN_PER    0x48005000U
#define CM_ICLKEN_PER    0x48005010U
#define CM_CLKSEL_PER    0x48005040U	/* CLKSEL_GPT2 bit 0: 1 = SYS_CLK */
#define PER_EN_GPT2      BIT(3)

#define GPT2_BASE  0x49032000U
#define GPT_TCLR   (GPT2_BASE + 0x24U)
#define GPT_TCRR   (GPT2_BASE + 0x28U)
#define GPT_TLDR   (GPT2_BASE + 0x2CU)
#define GPT_TWPS   (GPT2_BASE + 0x34U)

static const char *const clkin[] = { "12", "13", "19.2", "26", "38.4", "16.8", "?", "?" };

static void gpt2_write(uint32_t v, uint32_t reg)
{
	sys_write32(v, reg);
	while (sys_read32(GPT_TWPS) != 0U) {
	}
}

int main(void)
{
	uint32_t sel = sys_read32(PRM_CLKSEL) & 7U;
	uint32_t div = (sys_read32(PRM_CLKSRC_CTRL) >> 6) & 3U;

	printk("PRM_CLKSEL SYS_CLKIN_SEL=%u (%s MHz), SYSCLKDIV=%u\n", sel, clkin[sel], div);

	sys_set_bits(CM_CLKSEL_PER, BIT(0));
	sys_set_bits(CM_FCLKEN_PER, PER_EN_GPT2);
	sys_set_bits(CM_ICLKEN_PER, PER_EN_GPT2);
	k_busy_wait(100);

	gpt2_write(0, GPT_TLDR);
	gpt2_write(0, GPT_TCRR);
	gpt2_write(BIT(0) | BIT(1), GPT_TCLR);

	for (int i = 0; i < 5; i++) {
		uint32_t s0 = sys_read32(GPT_TCRR);
		uint32_t c0 = k_cycle_get_32();

		k_busy_wait(1000000);

		uint32_t s1 = sys_read32(GPT_TCRR);
		uint32_t c1 = k_cycle_get_32();

		printk("GPT2(sys_clk) %u counts per %u 32k cycles -> %u Hz\n", s1 - s0, c1 - c0,
		       (uint32_t)(((uint64_t)(s1 - s0) * 32768U) / (c1 - c0)));
	}

	return 0;
}
