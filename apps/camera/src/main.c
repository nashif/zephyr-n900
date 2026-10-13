/*
 * N900 main camera preview: ET8EK8 sensor over CCP2 into the OMAP3 ISP
 * receiver's memory port, demosaiced in software onto the LCD.
 */
#include <math.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include "et8ek8_regs.h"

static const struct device *const i2c3 = DEVICE_DT_GET(DT_NODELABEL(i2c3));
static const struct device *const gpio4 = DEVICE_DT_GET(DT_NODELABEL(gpio4));
static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* TWL5030 VAUX4: camera analog supply, 2.8 V */
#define VAUX4_DEV_GRP   0x7E
#define VAUX4_DEDICATED 0x81

/* Clock and power management */
#define CM_FCLKEN_CAM    0x48004F00
#define CM_ICLKEN_CAM    0x48004F10
#define CM_CLKSEL_CAM    0x48004F40
#define CM_CLKSTCTRL_CAM 0x48004F48
#define CM_CLKEN_PLL     0x48004D00
#define PWRDN_CAM        BIT(30)
#define PM_PWSTCTRL_CAM  0x48306FE0

/* Pads and PHY routing */
#define PAD_CAM_XCLKA 0x48002110
#define PAD_CAM_D3    0x4800211C
#define PAD_CAM_D6    0x48002122
#define CSIRXFE       0x480022DC

/* ISP and its CCP2 receiver */
#define ISP_BASE           0x480BC000
#define ISP_SYSCONFIG      (ISP_BASE + 0x04)
#define ISP_CTRL           (ISP_BASE + 0x40)
#define ISP_TCTRL          (ISP_BASE + 0x50)
#define CCP2               (ISP_BASE + 0x400)
#define CCP2_SYSCONFIG     (CCP2 + 0x04)
#define CCP2_SYSSTATUS     (CCP2 + 0x08)
#define CCP2_LC01_IRQSTAT  (CCP2 + 0x10)
#define CCP2_CTRL          (CCP2 + 0x40)
#define CCP2_LC0_CTRL      (CCP2 + 0x50)
#define CCP2_LC0_DAT_START (CCP2 + 0x68)
#define CCP2_LC0_DAT_SIZE  (CCP2 + 0x6C)
#define CCP2_LC0_PING      (CCP2 + 0x70)
#define CCP2_LC0_PONG      (CCP2 + 0x74)
#define CCP2_LC0_OFST      (CCP2 + 0x78)
#define LC0_FS             BIT(11)
#define LC0_FE             BIT(8)
#define LC0_ERRORS         0x3FU
/* Memory port format: 10-bit samples in 16-bit words */
#define LC0_FORMAT_RAW10   0x15U

/* Sensor */
#define SENSOR_ADDR   0x3E
#define REG_EXPOSURE  0x1243
#define REG_AGAIN_L   0x1249
#define REG_AGAIN_H   0x124A
#define REG_STREAM    0x1252
#define STREAM_ON     0xB0
#define EXPOSURE_MAX  500U
#define AGAIN_MIN     0x20U
#define AGAIN_MAX     0x100U
#define BLACK_LEVEL   64

#define RAW_W      648
#define RAW_H      492
#define RAW_STRIDE 656 /* 1312 bytes, a multiple of 32 */
#define OUT_W      640
#define OUT_H      480

static uint16_t raw[RAW_H][RAW_STRIDE] __aligned(64);
static uint32_t img[OUT_H * OUT_W];
static uint8_t gamma_lut[1024];

static uint32_t exposure = 200U;
static uint32_t again = AGAIN_MIN;
/* White balance gains, 8.8 fixed point */
static uint32_t wb_r = 256U, wb_b = 256U;

static int sensor_write(uint16_t reg, const uint8_t *val, size_t len)
{
	uint8_t buf[4] = {reg >> 8, reg & 0xFF};

	memcpy(&buf[2], val, len);
	return i2c_write(i2c3, buf, 2 + len, SENSOR_ADDR);
}

static int sensor_write8(uint16_t reg, uint8_t val)
{
	return sensor_write(reg, &val, 1);
}

static int sensor_read(uint16_t reg, uint8_t *val, size_t len)
{
	uint8_t addr[2] = {reg >> 8, reg & 0xFF};

	return i2c_write_read(i2c3, SENSOR_ADDR, addr, 2, val, len);
}

static int sensor_write_list(const struct cam_reg *regs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = sensor_write8(regs[i].addr, regs[i].val);

		if (ret != 0) {
			printk("sensor write %04x failed: %d\n", regs[i].addr, ret);
			return ret;
		}
	}
	return 0;
}

static void sensor_set_exposure(void)
{
	uint8_t e[2] = {exposure & 0xFF, exposure >> 8};

	(void)sensor_write(REG_EXPOSURE, e, 2);
	(void)sensor_write8(REG_AGAIN_H, again >> 8);
	(void)sensor_write8(REG_AGAIN_L, again & 0xFF);
}

static void camera_power_on(void)
{
	gpio_pin_configure(gpio4, 6, GPIO_OUTPUT_LOW);
	sys_write16(0x0004, PAD_CAM_D3);

	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEDICATED, 0x09);
	mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, VAUX4_DEV_GRP, 0x20);

	sys_write32((sys_read32(PM_PWSTCTRL_CAM) & ~3U) | 3U, PM_PWSTCTRL_CAM);
	sys_write32(2U, CM_CLKSTCTRL_CAM);
	sys_write32(1U, CM_ICLKEN_CAM);
	sys_write32(3U, CM_FCLKEN_CAM);
	sys_write32(sys_read32(CM_CLKEN_PLL) & ~PWRDN_CAM, CM_CLKEN_PLL);
	/* cam_mclk = 864 MHz / 5 = 172.8 MHz; / 18 = 9.6 MHz on cam_xclka */
	sys_write32((sys_read32(CM_CLKSEL_CAM) & ~0x1FU) | 5U, CM_CLKSEL_CAM);
	k_msleep(1);
	sys_write32(1U << 12, ISP_SYSCONFIG);
	sys_write32((sys_read32(ISP_TCTRL) & ~0x1FU) | 18U, ISP_TCTRL);
	sys_write16(0x0000, PAD_CAM_XCLKA);
	k_msleep(1);
	gpio_pin_set(gpio4, 6, 1);
	k_msleep(5);
}

static void ccp2_init(void)
{
	/* CCP2 data/strobe lanes on cam_d6..d9, PHY powered in strobe mode */
	for (int i = 0; i < 4; i++) {
		sys_write16(0x0100, PAD_CAM_D6 + 2 * i);
	}
	sys_write32(BIT(13) | BIT(12) | BIT(10), CSIRXFE);

	sys_write32(BIT(1), CCP2_SYSCONFIG);
	while ((sys_read32(CCP2_SYSSTATUS) & BIT(0)) == 0U) {
	}
	sys_write32(1U << 12, CCP2_SYSCONFIG);
	/* CCP2 mode, data/strobe PHY, CCP2 output selected, memory port on */
	sys_write32(BIT(4) | BIT(2) | BIT(1), CCP2_CTRL);
	/* Shared buffer logic write port */
	sys_write32(sys_read32(ISP_CTRL) | BIT(19), ISP_CTRL);

	sys_write32((LC0_FORMAT_RAW10 << 3) | BIT(2) | BIT(1), CCP2_LC0_CTRL);
	sys_write32((uint32_t)raw, CCP2_LC0_PING);
	sys_write32((uint32_t)raw, CCP2_LC0_PONG);
	sys_write32(RAW_STRIDE * 2, CCP2_LC0_OFST);
	sys_write32(0, CCP2_LC0_DAT_START);
	sys_write32(RAW_H << 16, CCP2_LC0_DAT_SIZE);
	sys_write32(sys_read32(CCP2_CTRL) | BIT(0), CCP2_CTRL);
}

static bool wait_status(uint32_t bit, int64_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;

	while ((sys_read32(CCP2_LC01_IRQSTAT) & bit) == 0U) {
		if (k_uptime_get() > end) {
			return false;
		}
		k_yield();
	}
	return true;
}

/* Capture one whole frame: enable the channel, wait for a start, then the end */
static int capture(void)
{
	uint32_t st;

	sys_write32(0xFFFFFFFFU, CCP2_LC01_IRQSTAT);
	sys_write32(sys_read32(CCP2_LC0_CTRL) | BIT(0), CCP2_LC0_CTRL);
	if (!wait_status(LC0_FS, 200)) {
		return -ETIMEDOUT;
	}
	sys_write32(0xFFFFFFFFU, CCP2_LC01_IRQSTAT);
	if (!wait_status(LC0_FE, 200)) {
		return -ETIMEDOUT;
	}
	st = sys_read32(CCP2_LC01_IRQSTAT);
	sys_write32(sys_read32(CCP2_LC0_CTRL) & ~BIT(0), CCP2_LC0_CTRL);
	sys_cache_data_invd_range(raw, sizeof(raw));

	return ((st & LC0_ERRORS) != 0U) ? -EIO : 0;
}

static inline uint8_t tone(int32_t v, uint32_t gain)
{
	v -= BLACK_LEVEL;
	if (v < 0) {
		v = 0;
	}
	v = (v * (int32_t)gain) >> 8;
	return gamma_lut[MIN(v, 1023)];
}

/* GRBG 2x2 cells to RGB; returns the mean green level above black */
static uint32_t develop(void)
{
	uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
	/* Map 959 (white above black) to 1023 */
	const uint32_t norm = 273U;
	/* Scale so the smallest gain is 1: all channels clip together at white */
	uint32_t wb_min = MIN(MIN(wb_r, wb_b), 256U);
	uint32_t gr = wb_r * norm / wb_min, gg = 256U * norm / wb_min, gb = wb_b * norm / wb_min;
	const int x0 = (RAW_W - OUT_W) / 2, y0 = (RAW_H - OUT_H) / 2;

	for (int cy = 0; cy < OUT_H / 2; cy++) {
		const uint16_t *r0 = &raw[y0 + 2 * cy][x0];
		const uint16_t *r1 = &raw[y0 + 2 * cy + 1][x0];
		uint32_t *o0 = &img[(2 * cy) * OUT_W];
		uint32_t *o1 = o0 + OUT_W;

		for (int cx = 0; cx < OUT_W / 2; cx++) {
			int32_t g = (r0[2 * cx] + r1[2 * cx + 1]) / 2;
			int32_t r = r0[2 * cx + 1];
			int32_t b = r1[2 * cx];
			uint32_t px;

			sum_r += r;
			sum_g += g;
			sum_b += b;
			px = ((uint32_t)tone(r, gr) << 16) | ((uint32_t)tone(g, gg) << 8) |
			     tone(b, gb);
			o0[2 * cx] = px;
			o0[2 * cx + 1] = px;
			o1[2 * cx] = px;
			o1[2 * cx + 1] = px;
		}
	}

	uint32_t n = (OUT_W / 2) * (OUT_H / 2);
	int32_t mr = (int32_t)(sum_r / n) - BLACK_LEVEL;
	int32_t mg = (int32_t)(sum_g / n) - BLACK_LEVEL;
	int32_t mb = (int32_t)(sum_b / n) - BLACK_LEVEL;

	/* Gray world, smoothed */
	if (mr > 4 && mb > 4 && mg > 4) {
		wb_r = (3 * wb_r + CLAMP((uint32_t)(256 * mg / mr), 128U, 1024U)) / 4;
		wb_b = (3 * wb_b + CLAMP((uint32_t)(256 * mg / mb), 128U, 1024U)) / 4;
	}
	return MAX(mg, 0);
}

/* Exposure first, then analog gain, towards a mid-grey green level */
static void auto_exposure(uint32_t mean)
{
	const uint32_t target = 110U;
	uint32_t old_e = exposure, old_g = again;

	if (mean < 1U) {
		mean = 1U;
	}
	if ((mean * 10U < target * 9U) || (mean * 10U > target * 11U)) {
		uint32_t want = exposure * again * target / mean;

		/* want is in exposure lines times gain/32 units * 32 */
		if (want <= EXPOSURE_MAX * AGAIN_MIN) {
			again = AGAIN_MIN;
			exposure = CLAMP(want / AGAIN_MIN, 2U, EXPOSURE_MAX);
		} else {
			exposure = EXPOSURE_MAX;
			again = CLAMP(want / EXPOSURE_MAX, AGAIN_MIN, AGAIN_MAX);
		}
		/* Move half way to damp oscillation */
		exposure = (exposure + old_e) / 2U;
		again = (again + old_g) / 2U;
		if ((exposure != old_e) || (again != old_g)) {
			sensor_set_exposure();
		}
	}
}

int main(void)
{
	struct display_capabilities caps;
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(img),
		.width = OUT_W,
		.height = OUT_H,
		.pitch = OUT_W,
	};
	uint8_t rev[2] = {0};
	uint32_t frames = 0, errors = 0;
	int64_t t_stat;
	int ret;

	for (int i = 0; i < 1024; i++) {
		gamma_lut[i] = (uint8_t)(powf(i / 1023.0f, 1.0f / 2.2f) * 255.0f + 0.5f);
	}

	display_get_capabilities(disp, &caps);
	printk("camera preview, display %ux%u format %d\n", caps.x_resolution,
	       caps.y_resolution, caps.current_pixel_format);
	display_blanking_off(disp);

	camera_power_on();
	ret = sensor_read(0x1200, rev, 2);
	printk("sensor revision %u (read %d)\n", rev[0] | (rev[1] << 8), ret);
	if (ret != 0) {
		return 0;
	}
	if ((sensor_write_list(poweron_regs, ARRAY_SIZE(poweron_regs)) != 0) ||
	    (sensor_write_list(vga_regs, ARRAY_SIZE(vga_regs)) != 0)) {
		return 0;
	}
	sensor_set_exposure();
	ccp2_init();
	sensor_write8(REG_STREAM, STREAM_ON);

	t_stat = k_uptime_get();
	for (;;) {
		uint32_t mean;

		ret = capture();
		if (ret == -ETIMEDOUT) {
			printk("no frame\n");
			k_msleep(500);
			continue;
		}
		if (ret != 0) {
			errors++;
		}
		mean = develop();
		display_write(disp, (caps.x_resolution - OUT_W) / 2, 0, &desc, img);
		auto_exposure(mean);
		frames++;

		if (k_uptime_get() - t_stat >= 2000) {
			printk("%u fps, %u errors, green %u, exposure %u, gain %u/32, wb %u %u\n",
			       frames / 2, errors, mean, exposure, again, wb_r, wb_b);
			frames = 0;
			t_stat = k_uptime_get();
		}
	}
	return 0;
}
