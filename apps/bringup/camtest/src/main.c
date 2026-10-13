/* N900 camera bring-up: flash LED, then sensor power and identification */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/sys/sys_io.h>

static const struct device *const flash = DEVICE_DT_GET(DT_NODELABEL(flash));
static const struct device *const i2c3 = DEVICE_DT_GET(DT_NODELABEL(i2c3));
static const struct device *const gpio4 = DEVICE_DT_GET(DT_NODELABEL(gpio4));
static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));

/* TWL5030 VAUX4, power receiver at 0x4B */
#define VAUX2_DEV_GRP   0x76
#define VAUX2_DEDICATED 0x79
#define VAUX2_1V8       0x05
#define VAUX4_DEV_GRP   0x7E
#define VAUX4_DEDICATED 0x81
#define DEV_GRP_P1      0x20
#define VAUX4_2V8       0x09

/* Clock and power management, camera domain */
#define CM_FCLKEN_CAM   0x48004F00
#define CM_ICLKEN_CAM   0x48004F10
#define CM_IDLEST_CAM   0x48004F20
#define CM_CLKSEL_CAM   0x48004F40
#define CM_CLKSEL2_PLL  0x48004D44
#define CM_CLKSTCTRL_CAM 0x48004F48
#define CM_CLKEN_PLL     0x48004D00
#define PWRDN_CAM        BIT(30)
#define PM_PWSTCTRL_CAM 0x48306FE0
#define PM_PWSTST_CAM   0x48306FE4

/* Image signal processor */
#define ISP_BASE        0x480BC000
#define ISP_REVISION    (ISP_BASE + 0x00)
#define ISP_SYSCONFIG   (ISP_BASE + 0x04)
#define ISP_TCTRL_CTRL  (ISP_BASE + 0x50)

#define PAD_CAM_XCLKA   0x48002110
#define PAD_CAM_D3      0x4800211C /* GPIO 102, sensor reset */

#define SENSOR 0x3E
#define LENS   0x0C

static void probe(const char *when)
{
	uint8_t reg[2] = {0x12, 0x00};
	uint8_t lo = 0, hi = 0, lens[2] = {0};
	int r1, r2, r3;

	r1 = i2c_write(i2c3, reg, 2, SENSOR);
	if (r1 == 0) {
		r1 = i2c_read(i2c3, &lo, 1, SENSOR);
	}
	reg[1] = 0x01;
	r2 = i2c_write(i2c3, reg, 2, SENSOR);
	if (r2 == 0) {
		r2 = i2c_read(i2c3, &hi, 1, SENSOR);
	}
	r3 = i2c_read(i2c3, lens, 2, LENS);
	printk("%-10s sensor %d/%d rev 0x%02x%02x  lens %d (%02x %02x)\n", when, r1, r2, hi, lo,
	       r3, lens[0], lens[1]);
}

static void scan(void)
{
	printk("I2C3:");
	for (uint16_t a = 0x08; a < 0x78; a++) {
		uint8_t b;

		if (i2c_read(i2c3, &b, 1, a) == 0) {
			printk(" %02x", a);
		}
	}
	printk("\n");
}

static void flash_test(void)
{
	/* Flash already verified */
	return;
	printk("flash: %s\n", device_is_ready(flash) ? "ready" : "NOT READY");
	if (!device_is_ready(flash)) {
		return;
	}
	printk("torch ramp\n");
	for (int p = 0; p <= 100; p += 10) {
		led_set_brightness(flash, 0, p);
		k_msleep(200);
	}
	k_msleep(1000);
	led_set_brightness(flash, 0, 0);
	for (int i = 1; i <= 3; i++) {
		k_msleep(1500);
		printk("flash %d: %d\n", i, led_set_brightness(flash, 2, 100));
	}
	k_msleep(1500);
}

static void camera_power(void)
{
	uint8_t grp = 0, ded = 0;
	uint32_t sel2, clksel, m, n, dpll4, mclk, div;
	int64_t t;

	probe("unpowered");

	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEV_GRP, &grp);
	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEDICATED, &ded);
	printk("VAUX4 was DEV_GRP %02x VSEL %02x\n", grp, ded);
	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEDICATED, VAUX4_2V8);
	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEV_GRP, DEV_GRP_P1);
	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEV_GRP, &grp);
	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEDICATED, &ded);
	printk("VAUX4 now DEV_GRP %02x VSEL %02x\n", grp, ded);
	k_msleep(5);

	printk("CAM domain state %x, ctrl %x\n", sys_read32(PM_PWSTST_CAM) & 3,
	       sys_read32(PM_PWSTCTRL_CAM) & 3);
	sys_write32((sys_read32(PM_PWSTCTRL_CAM) & ~3U) | 3U, PM_PWSTCTRL_CAM);
	sys_set_bit(CM_ICLKEN_CAM, 0);
	sys_set_bit(CM_FCLKEN_CAM, 0);
	t = k_uptime_get();
	while ((sys_read32(CM_IDLEST_CAM) & 1U) != 0U && (k_uptime_get() - t) < 100) {
	}
	printk("CAM domain %x, IDLEST %x, ISP revision %08x\n", sys_read32(PM_PWSTST_CAM) & 3,
	       sys_read32(CM_IDLEST_CAM), sys_read32(ISP_REVISION));

	sel2 = sys_read32(CM_CLKSEL2_PLL);
	clksel = sys_read32(CM_CLKSEL_CAM) & 0x1F;
	m = (sel2 >> 8) & 0x7FF;
	n = sel2 & 0x7F;
	dpll4 = (uint32_t)((19200000ULL * m) / (n + 1));
	mclk = (clksel != 0) ? (2U * dpll4) / clksel : 0;
	div = (mclk + 4800000U) / 9600000U;
	printk("DPLL4 M %u N %u = %u Hz, CLKSEL_CAM %u, cam_mclk %u Hz, DIVA %u -> %u Hz\n", m,
	       n, dpll4, clksel, mclk, div, (div != 0) ? mclk / div : 0);
	div = CLAMP(div, 2U, 31U);
	sys_write32((sys_read32(ISP_TCTRL_CTRL) & ~0x1FU) | div, ISP_TCTRL_CTRL);

	printk("pads: xclka %04x, gpio102 %04x\n", sys_read16(PAD_CAM_XCLKA),
	       sys_read16(PAD_CAM_D3));
	sys_write16(0x0000, PAD_CAM_XCLKA); /* mode 0, output */
	sys_write16(0x0004, PAD_CAM_D3);    /* mode 4, GPIO output */

	k_busy_wait(10);
	gpio_pin_configure(gpio4, 6, GPIO_OUTPUT_HIGH);
	k_msleep(5);

	probe("powered");
	scan();
}

/* Second round: keep the camera clock domain and the ISP awake, exact 9.6 MHz */
static void camera_clock_awake(void)
{
	printk("CLKSTCTRL_CAM %x, ISP_SYSCONFIG %08x, TCTRL %08x\n",
	       sys_read32(CM_CLKSTCTRL_CAM), sys_read32(ISP_SYSCONFIG),
	       sys_read32(ISP_TCTRL_CTRL));
	/* Force the domain awake, ISP master never in standby */
	sys_write32(0x2, CM_CLKSTCTRL_CAM);
	sys_write32(1U << 12, ISP_SYSCONFIG);
	/* cam_mclk = 2 * 432 MHz / 5 = 172.8 MHz, / 18 = 9.6 MHz */
	sys_write32((sys_read32(CM_CLKSEL_CAM) & ~0x1FU) | 5U, CM_CLKSEL_CAM);
	sys_write32((sys_read32(ISP_TCTRL_CTRL) & ~0x1FU) | 18U, ISP_TCTRL_CTRL);
	k_msleep(2);
	printk("now CLKSTCTRL %x IDLEST %x CLKSEL_CAM %u TCTRL %08x\n",
	       sys_read32(CM_CLKSTCTRL_CAM), sys_read32(CM_IDLEST_CAM),
	       sys_read32(CM_CLKSEL_CAM) & 0x1F, sys_read32(ISP_TCTRL_CTRL));

	gpio_pin_set(gpio4, 6, 0);
	k_msleep(10);
	gpio_pin_set(gpio4, 6, 1);
	k_msleep(10);
	probe("awake 10ms");
	k_msleep(100);
	probe("awake 110ms");
	scan();
}

/* Third round: VAUX2 (VCSI, 1.8 V) */
static void camera_vcsi(void)
{
	uint8_t grp = 0, ded = 0;

	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX2_DEV_GRP, &grp);
	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, VAUX2_DEDICATED, &ded);
	printk("VAUX2 was DEV_GRP %02x VSEL %02x\n", grp, ded);
	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX2_DEDICATED, VAUX2_1V8);
	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX2_DEV_GRP, DEV_GRP_P1);
	k_msleep(5);
	gpio_pin_set(gpio4, 6, 0);
	k_msleep(10);
	gpio_pin_set(gpio4, 6, 1);
	k_msleep(10);
	probe("VCSI on");
	scan();
}

/* Fourth round: power up the camera output (M5) of DPLL4 */
static void camera_m5(void)
{
	uint32_t en = sys_read32(CM_CLKEN_PLL);

	printk("CM_CLKEN_PLL %08x, PWRDN_CAM %u\n", en, (en & PWRDN_CAM) ? 1U : 0U);
	sys_write32(en & ~PWRDN_CAM, CM_CLKEN_PLL);
	k_msleep(2);
	printk("CM_CLKEN_PLL now %08x\n", sys_read32(CM_CLKEN_PLL));
	gpio_pin_set(gpio4, 6, 0);
	k_msleep(10);
	gpio_pin_set(gpio4, 6, 1);
	k_msleep(10);
	probe("M5 on");
	scan();
}

/* Fifth round: sample cam_xclka through GPIO 96 (gpio3 pin 0) */
#define GPIO3_DATAIN 0x49052038

static void xclk_sample(uint32_t diva)
{
	unsigned int ones = 0, edges = 0, last = 0;

	sys_write32((sys_read32(ISP_TCTRL_CTRL) & ~0x1FU) | diva, ISP_TCTRL_CTRL);
	k_busy_wait(100);
	for (int i = 0; i < 2000; i++) {
		unsigned int v = sys_read32(GPIO3_DATAIN) & 1U;

		ones += v;
		edges += (i > 0) && (v != last);
		last = v;
	}
	printk("DIVA %2u: %4u/2000 high, %4u changes\n", diva, ones, edges);
}

static void camera_xclk_probe(void)
{
	/* Mode 0 (cam_xclka) with the input buffer on */
	sys_write16(0x0100, PAD_CAM_XCLKA);
	printk("pad xclka %04x\n", sys_read16(PAD_CAM_XCLKA));
	xclk_sample(0);
	xclk_sample(1);
	xclk_sample(18);
	xclk_sample(31);
	sys_write32((sys_read32(ISP_TCTRL_CTRL) & ~0x1FU) | 18U, ISP_TCTRL_CTRL);
}

int main(void)
{
	printk("N900 camera bring-up\n");
	flash_test();
	camera_power();
	camera_clock_awake();
	camera_vcsi();
	camera_m5();
	camera_xclk_probe();
	printk("done\n");
	return 0;
}
