/* N900 audio bring-up: TLV320AIC34 codec over McBSP2, speaker tone */
#include <math.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

static const struct device *const i2c2 = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static const struct device *const gpio2 = DEVICE_DT_GET(DT_NODELABEL(gpio2));
static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));

#define CODEC 0x18

/* TWL: VMMC2 in the power master/receiver block, GPIO in the second address */
#define VMMC2_DEV_GRP   0x86
#define VMMC2_DEDICATED 0x89
#define VMMC2_2V8       0x09
#define PAD_GPIO60      0x480020C6
#define TWL_GPIO_DIR1   0x9B
#define TWL_GPIO_SET1   0xA4
#define TWL_GPIO_CTRL   0xAA

#define CM_FCLKEN_PER 0x48005000
#define CM_ICLKEN_PER 0x48005010
#define CM_IDLEST_PER 0x48005020

#define MCBSP2    0x49022000
#define DXR       (MCBSP2 + 0x08)
#define SPCR2     (MCBSP2 + 0x10)
#define SPCR1     (MCBSP2 + 0x14)
#define RCR2      (MCBSP2 + 0x18)
#define RCR1      (MCBSP2 + 0x1C)
#define XCR2      (MCBSP2 + 0x20)
#define XCR1      (MCBSP2 + 0x24)
#define PCR       (MCBSP2 + 0x48)
#define SYSCON    (MCBSP2 + 0x8C)
#define THRSH2    (MCBSP2 + 0x90)
#define IRQST     (MCBSP2 + 0xA0)
#define XBUFFSTAT (MCBSP2 + 0xB4)

#define PAD_MCBSP2_FSX 0x4800213C

static int codec_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c2, CODEC, reg, val);
}

static uint8_t codec_rd(uint8_t reg)
{
	uint8_t v = 0xEE;

	(void)i2c_reg_read_byte(i2c2, CODEC, reg, &v);
	return v;
}

static int16_t tone[48];

int main(void)
{
	uint8_t grp = 0, ded = 0;
	int ret;

	k_msleep(3000);
	printk("audio test\n");
	for (int i = 0; i < 48; i++) {
		tone[i] = (int16_t)(8000.0f * sinf(2.0f * 3.14159265f * i / 48.0f));
	}

	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEV_GRP, &grp);
	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEDICATED, &ded);
	printk("VMMC2 DEV_GRP %02x VSEL %02x\n", grp, ded);
	if ((grp & 0xE0) == 0U) {
		mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEDICATED, VMMC2_2V8);
		mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEV_GRP, 0x20);
		k_msleep(5);
		mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEV_GRP, &grp);
		mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VMMC2_DEDICATED, &ded);
		printk("VMMC2 now DEV_GRP %02x VSEL %02x\n", grp, ded);
	}
	printk("pad gpio60 %04x\n", sys_read16(PAD_GPIO60));
	sys_write16(0x0004, PAD_GPIO60);
	for (uint8_t a = 0x18; a <= 0x60; a = (a == 0x19) ? 0x60 : a + 1) {
		uint8_t b;

		printk("before reset: 0x%02x %s\n", a,
		       i2c_reg_read_byte(i2c2, a, 0, &b) == 0 ? "ACK" : "-");
		if (a == 0x60) {
			break;
		}
	}

	/* Codec reset on GPIO 60, active low */
	gpio_pin_configure(gpio2, 28, GPIO_OUTPUT_LOW);
	k_msleep(2);
	gpio_pin_set(gpio2, 28, 1);
	k_msleep(2);

	printk("GPIO60 level %d\n", gpio_pin_get_raw(gpio2, 28));
	for (uint8_t a = 0x18; a <= 0x19; a++) {
		uint8_t b;

		printk("after reset: 0x%02x %s\n", a,
		       i2c_reg_read_byte(i2c2, a, 0, &b) == 0 ? "ACK" : "-");
	}
	ret = codec_wr(0, 0);
	printk("codec page select: %d\n", ret);
	if (ret != 0) {
		return 0;
	}
	codec_wr(1, 0x80); /* soft reset */
	k_msleep(2);
	printk("codec regs 2..12:");
	for (int r = 2; r <= 12; r++) {
		printk(" %02x", codec_rd(r));
	}
	printk("\n");

	/* 19.2 MHz MCLK -> PLL: P=1, J=5, D=1200, R=1 -> 98.304 MHz = 2048 x 48 kHz */
	codec_wr(102, 0x02);  /* PLL and CLKDIV inputs: MCLK */
	codec_wr(101, 0x00);  /* CODEC_CLKIN = PLLDIV_OUT */
	codec_wr(3, 0x81);    /* PLL on, P = 1 */
	codec_wr(4, 5 << 2);  /* J = 5 */
	codec_wr(5, 1200 >> 6);
	codec_wr(6, (1200 & 0x3F) << 2);
	codec_wr(11, 0x01);   /* R = 1 */
	codec_wr(2, 0x00);    /* fs = fsref */
	codec_wr(7, 0x0A);    /* fsref 48 kHz, left DAC <- left, right DAC <- right */
	codec_wr(8, 0xC0);    /* BCLK and WCLK outputs */
	codec_wr(9, 0x40);    /* DSP mode, 16-bit words */
	codec_wr(10, 0x01);   /* one bit clock of data delay */

	/* DACs to HPLOUT/HPROUT, the COM outputs as their inverse, for the speakers */
	codec_wr(37, 0xC0);   /* both DACs powered, HPLCOM differential */
	codec_wr(38, 0x00);   /* HPRCOM differential */
	codec_wr(41, 0x00);   /* DAC_L1/R1 paths */
	codec_wr(43, 0x00);   /* left DAC 0 dB, unmuted */
	codec_wr(44, 0x00);
	codec_wr(47, 0x80);   /* DAC_L1 -> HPLOUT, 0 dB */
	codec_wr(64, 0x80);   /* DAC_R1 -> HPROUT */
	codec_wr(51, 0x09);   /* HPLOUT unmuted, powered */
	codec_wr(65, 0x09);
	codec_wr(58, 0x09);   /* HPLCOM */
	codec_wr(72, 0x09);   /* HPRCOM */
	k_msleep(10);
	printk("codec 3..11:");
	for (int r = 3; r <= 11; r++) {
		printk(" %02x", codec_rd(r));
	}
	printk("  37 %02x 51 %02x 65 %02x 94 %02x 95 %02x\n", codec_rd(37), codec_rd(51),
	       codec_rd(65), codec_rd(94), codec_rd(95));

	/* McBSP2: clocks, pads, slave transmitter, DSP A, 2 x 16-bit words per frame */
	sys_set_bit(CM_ICLKEN_PER, 0);
	sys_set_bit(CM_FCLKEN_PER, 0);
	k_busy_wait(100);
	printk("McBSP2 IDLEST %u pads %08x %08x\n", sys_read32(CM_IDLEST_PER) & 1,
	       sys_read32(PAD_MCBSP2_FSX), sys_read32(PAD_MCBSP2_FSX + 4));
	sys_write32(0x01000100, PAD_MCBSP2_FSX);     /* fsx, clkx inputs, mode 0 */
	sys_write32(0x00000100, PAD_MCBSP2_FSX + 4); /* dr input, dx output */
	sys_write32(BIT(1), SYSCON);
	while (sys_read32(SYSCON) & BIT(1)) {
	}
	sys_write32(1U << 3, SYSCON); /* no idle */
	sys_write32(0, SPCR2);
	sys_write32(0, SPCR1);
	sys_write32(0, PCR);
	sys_write32((1U << 8) | (2U << 5), XCR1);
	sys_write32(1U, XCR2);
	sys_write32((1U << 8) | (2U << 5), RCR1);
	sys_write32(1U, RCR2);
	sys_write32(0x100, THRSH2);
	sys_write32(BIT(9) | BIT(0), SPCR2); /* FREE, XRST */
	printk("XBUFFSTAT after enable %u\n", sys_read32(XBUFFSTAT));

	/* Speaker amplifier on TWL GPIO 7 */
	uint8_t v;

	mfd_twl4030_reg_read(twl, 1, TWL_GPIO_CTRL, &v);
	mfd_twl4030_reg_write(twl, 1, TWL_GPIO_CTRL, v | BIT(2));
	mfd_twl4030_reg_read(twl, 1, TWL_GPIO_DIR1, &v);
	mfd_twl4030_reg_write(twl, 1, TWL_GPIO_DIR1, v | BIT(7));
	mfd_twl4030_reg_write(twl, 1, TWL_GPIO_SET1, BIT(7));

	/* Feed the tone for 6 s: on for 0.5 s, off for 0.5 s */
	uint32_t pos = 0, words = 0;
	int64_t start = k_uptime_get(), last = start;

	while (k_uptime_get() - start < 6000) {
		uint32_t free = sys_read32(XBUFFSTAT);
		bool on = ((k_uptime_get() - start) % 1000) < 500;

		while (free >= 2) {
			int16_t s = on ? tone[pos % 48] : 0;

			sys_write32((uint16_t)s, DXR);
			sys_write32((uint16_t)s, DXR);
			pos++;
			words += 2;
			free -= 2;
		}
		if (k_uptime_get() - last >= 1000) {
			last = k_uptime_get();
			printk("words %u, XBUFFSTAT %u, IRQST %08x, SPCR2 %08x, codec 94 %02x 95 %02x\n",
			       words, sys_read32(XBUFFSTAT), sys_read32(IRQST), sys_read32(SPCR2),
			       codec_rd(94), codec_rd(95));
		}
		k_msleep(2);
	}
	printk("done: %u words in 6 s (%u/s)\n", words, words / 6);
	sys_write32(0, SPCR2);
	mfd_twl4030_reg_write(twl, 1, 0xA1, BIT(7)); /* CLEARGPIODATAOUT1 */
	return 0;
}
