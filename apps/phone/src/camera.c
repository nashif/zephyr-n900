/*
 * N900 phone demo: main camera. The ET8EK8 sensor streams 648x492 RAW10 over
 * CCP2 into the OMAP3 ISP receiver, whose memory port writes each frame to RAM.
 * A thread turns frames into previews and photos, runs auto exposure, white
 * balance and contrast autofocus with the AD5820 lens actuator, and fires the
 * ADP1653 flash.
 */
#include <math.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include "camera.h"
#include "et8ek8_regs.h"

static const struct device *const i2c3 = DEVICE_DT_GET(DT_NODELABEL(i2c3));
static const struct device *const gpio4 = DEVICE_DT_GET(DT_NODELABEL(gpio4));
static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));
static const struct device *const flash = DEVICE_DT_GET(DT_NODELABEL(flash));

/* ADP1653 LEDs */
#define FLASH_LED_INDICATOR 1
#define FLASH_LED_FLASH     2

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
#define LC0_FORMAT_RAW10   0x15U

/* Sensor */
#define SENSOR_ADDR  0x3E
#define REG_EXPOSURE 0x1243
#define REG_AGAIN_L  0x1249
#define REG_AGAIN_H  0x124A
#define REG_STREAM   0x1252
#define STREAM_ON    0xB0
#define STREAM_OFF   0x30
#define EXPOSURE_MAX 500U
#define AGAIN_MIN    0x20U
#define AGAIN_MAX    0x100U
#define BLACK_LEVEL  64

/* Lens actuator: 10-bit position at bit 4, bit 15 powers it down */
#define LENS_ADDR  0x0C
#define LENS_SHIFT 4
#define LENS_MAX   1023U

#define RAW_W      648
#define RAW_H      492
#define RAW_STRIDE 656 /* 1312 bytes, a multiple of 32 */
#define CELLS_W    (RAW_W / 2)
#define CELLS_H    (RAW_H / 2)

static uint16_t raw[RAW_H][RAW_STRIDE] __aligned(64);
static uint32_t cells[CELLS_H][CELLS_W];
static uint32_t previews[3][CAM_PREVIEW_W * CAM_PREVIEW_H];
static uint32_t photos[CAM_PHOTOS][CAM_PHOTO_W * CAM_PHOTO_H];
static uint8_t gamma_lut[1024];
static uint16_t preview_xmap[CAM_PREVIEW_W];

enum cam_cmd {
	CMD_START,
	CMD_STOP,
	CMD_FOCUS,
	CMD_SHOOT,
	CMD_SHOOT_FLASH,
};

K_MSGQ_DEFINE(cam_q, sizeof(uint8_t), 8, 1);

static struct k_spinlock lock;
static struct cam_status status = {.last_photo = -1};
static int latest = -1;
static int in_use = -1;
static uint32_t seq;

/* Thread state */
static bool powered;
static uint32_t exposure = 200U;
static uint32_t again = AGAIN_MIN;
static uint32_t wb_r = 256U, wb_b = 256U;
static uint32_t sharpness;

/* Autofocus: a coarse sweep, then a fine one around the best position */
#define AF_COARSE_STEP 85
#define AF_FINE_STEP   16
#define AF_FINE_SPAN   64
static struct {
	bool fine;
	int pos;
	int end;
	int step;
	int best_pos;
	uint32_t best;
	int settle;
} af;

/* Shot in progress: frames to wait with the flash on before capturing */
static int shot_frames = -1;
static bool shot_flash;
static uint32_t saved_exposure, saved_again;

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

static int sensor_write_list(const struct cam_reg *regs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = sensor_write8(regs[i].addr, regs[i].val);

		if (ret != 0) {
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

static void lens_set(uint16_t pos)
{
	uint16_t v = pos << LENS_SHIFT;
	uint8_t buf[2] = {v >> 8, v & 0xFF};

	(void)i2c_write(i2c3, buf, 2, LENS_ADDR);
	status.lens = pos;
}

static void hw_power_on(void)
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
	sys_write32(sys_read32(ISP_CTRL) | BIT(19), ISP_CTRL);
	sys_write32((LC0_FORMAT_RAW10 << 3) | BIT(2) | BIT(1), CCP2_LC0_CTRL);
	sys_write32((uint32_t)raw, CCP2_LC0_PING);
	sys_write32((uint32_t)raw, CCP2_LC0_PONG);
	sys_write32(RAW_STRIDE * 2, CCP2_LC0_OFST);
	sys_write32(0, CCP2_LC0_DAT_START);
	sys_write32(RAW_H << 16, CCP2_LC0_DAT_SIZE);
	sys_write32(sys_read32(CCP2_CTRL) | BIT(0), CCP2_CTRL);
}

static bool start(void)
{
	if (!powered) {
		hw_power_on();
		if ((sensor_write_list(poweron_regs, ARRAY_SIZE(poweron_regs)) != 0) ||
		    (sensor_write_list(vga_regs, ARRAY_SIZE(vga_regs)) != 0)) {
			return false;
		}
		powered = true;
		lens_set(LENS_MAX / 3);
	}
	sensor_set_exposure();
	sensor_write8(REG_STREAM, STREAM_ON);
	(void)led_set_brightness(flash, FLASH_LED_INDICATOR, 40);
	return true;
}

static void stop(void)
{
	if (powered) {
		sensor_write8(REG_STREAM, STREAM_OFF);
	}
	(void)led_set_brightness(flash, FLASH_LED_INDICATOR, 0);
}

static bool wait_status(uint32_t bit, int64_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;

	while ((sys_read32(CCP2_LC01_IRQSTAT) & bit) == 0U) {
		if (k_uptime_get() > end) {
			return false;
		}
		k_msleep(1);
	}
	return true;
}

/* One whole frame: enable the channel, wait for a start, then the end */
static int capture(void)
{
	uint32_t st;

	sys_write32(0xFFFFFFFFU, CCP2_LC01_IRQSTAT);
	sys_write32(sys_read32(CCP2_LC0_CTRL) | BIT(0), CCP2_LC0_CTRL);
	if (!wait_status(LC0_FS, 200)) {
		sys_write32(sys_read32(CCP2_LC0_CTRL) & ~BIT(0), CCP2_LC0_CTRL);
		return -ETIMEDOUT;
	}
	sys_write32(0xFFFFFFFFU, CCP2_LC01_IRQSTAT);
	if (!wait_status(LC0_FE, 200)) {
		sys_write32(sys_read32(CCP2_LC0_CTRL) & ~BIT(0), CCP2_LC0_CTRL);
		return -ETIMEDOUT;
	}
	st = sys_read32(CCP2_LC01_IRQSTAT);
	sys_write32(sys_read32(CCP2_LC0_CTRL) & ~BIT(0), CCP2_LC0_CTRL);
	sys_cache_data_invd_range(raw, sizeof(raw));

	return ((st & LC0_ERRORS) != 0U) ? -EIO : 0;
}

/* Channel gains, scaled so the smallest is 1 and 959 above black maps to 1023 */
static void channel_gains(uint32_t *gr, uint32_t *gg, uint32_t *gb)
{
	const uint32_t norm = 273U;
	uint32_t wb_min = MIN(MIN(wb_r, wb_b), 256U);

	*gr = wb_r * norm / wb_min;
	*gg = 256U * norm / wb_min;
	*gb = wb_b * norm / wb_min;
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

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/*
 * One colour per GRBG 2x2 cell, turned 180 degrees since the sensor is
 * mounted upside down, scaled up into a preview buffer. Also measures the
 * mean green level, the channel averages for white balance and the
 * contrast in the middle of the frame for autofocus. Returns the mean green.
 */
static uint32_t develop_preview(uint32_t *out)
{
	uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sharp = 0;
	uint32_t gr, gg, gb;

	channel_gains(&gr, &gg, &gb);
	for (int cy = 0; cy < CELLS_H; cy++) {
		const uint16_t *r0 = raw[2 * cy];
		const uint16_t *r1 = raw[2 * cy + 1];
		uint32_t *dst = cells[CELLS_H - 1 - cy];
		bool mid_y = (cy > CELLS_H / 3) && (cy < 2 * CELLS_H / 3);

		for (int cx = 0; cx < CELLS_W; cx++) {
			int32_t g = (r0[2 * cx] + r1[2 * cx + 1]) / 2;
			int32_t r = r0[2 * cx + 1];
			int32_t b = r1[2 * cx];

			sum_r += r;
			sum_g += g;
			sum_b += b;
			if (mid_y && (cx > CELLS_W / 3) && (cx < 2 * CELLS_W / 3)) {
				int32_t gx = r0[2 * cx + 2] - r0[2 * cx];
				int32_t gy = raw[2 * cy + 2][2 * cx] - r0[2 * cx];

				sharp += (uint32_t)(gx * gx + gy * gy);
			}
			dst[CELLS_W - 1 - cx] = rgb(tone(r, gr), tone(g, gg), tone(b, gb));
		}
	}

	for (int oy = 0; oy < CAM_PREVIEW_H; oy++) {
		const uint32_t *src = cells[oy * CELLS_H / CAM_PREVIEW_H];
		uint32_t *dst = &out[oy * CAM_PREVIEW_W];

		for (int ox = 0; ox < CAM_PREVIEW_W; ox++) {
			dst[ox] = src[preview_xmap[ox]];
		}
	}

	uint32_t n = CELLS_W * CELLS_H;
	int32_t mr = (int32_t)(sum_r / n) - BLACK_LEVEL;
	int32_t mg = (int32_t)(sum_g / n) - BLACK_LEVEL;
	int32_t mb = (int32_t)(sum_b / n) - BLACK_LEVEL;

	/* Gray world, smoothed */
	if ((mr > 4) && (mb > 4) && (mg > 4)) {
		wb_r = (3 * wb_r + CLAMP((uint32_t)(256 * mg / mr), 128U, 1024U)) / 4;
		wb_b = (3 * wb_b + CLAMP((uint32_t)(256 * mg / mb), 128U, 1024U)) / 4;
	}
	/* Contrast relative to brightness, so exposure changes matter less */
	sharpness = (uint32_t)(sharp / (uint64_t)MAX(mg, 8));
	return MAX(mg, 0);
}

static inline int32_t px(int x, int y)
{
	return raw[y][x];
}

/* Full-resolution bilinear demosaic of the middle 640x480, turned 180 degrees */
static void develop_photo(uint32_t *out)
{
	const int x0 = (RAW_W - CAM_PHOTO_W) / 2, y0 = (RAW_H - CAM_PHOTO_H) / 2;
	uint32_t gr, gg, gb;

	channel_gains(&gr, &gg, &gb);
	for (int y = 0; y < CAM_PHOTO_H; y++) {
		int Y = y0 + y;
		uint32_t *dst = &out[(CAM_PHOTO_H - 1 - y) * CAM_PHOTO_W + CAM_PHOTO_W - 1];

		for (int x = 0; x < CAM_PHOTO_W; x++, dst--) {
			int X = x0 + x;
			int32_t r, g, b;
			int32_t cross = (px(X - 1, Y) + px(X + 1, Y) + px(X, Y - 1) + px(X, Y + 1)) / 4;
			int32_t diag = (px(X - 1, Y - 1) + px(X + 1, Y - 1) + px(X - 1, Y + 1) +
					px(X + 1, Y + 1)) / 4;
			int32_t horiz = (px(X - 1, Y) + px(X + 1, Y)) / 2;
			int32_t vert = (px(X, Y - 1) + px(X, Y + 1)) / 2;

			if ((Y & 1) == 0) {
				if ((X & 1) == 0) { /* G on a red row */
					g = px(X, Y);
					r = horiz;
					b = vert;
				} else { /* R */
					r = px(X, Y);
					g = cross;
					b = diag;
				}
			} else {
				if ((X & 1) == 0) { /* B */
					b = px(X, Y);
					g = cross;
					r = diag;
				} else { /* G on a blue row */
					g = px(X, Y);
					b = horiz;
					r = vert;
				}
			}
			*dst = rgb(tone(r, gr), tone(g, gg), tone(b, gb));
		}
	}
}

/* Exposure first, then analog gain, towards a mid-grey green level */
static void auto_exposure(uint32_t mean, bool fast)
{
	const uint32_t target = 110U;
	uint32_t old_e = exposure, old_g = again;
	uint32_t want;

	mean = MAX(mean, 1U);
	if ((mean * 10U >= target * 9U) && (mean * 10U <= target * 11U)) {
		return;
	}
	want = exposure * again * target / mean;
	if (want <= EXPOSURE_MAX * AGAIN_MIN) {
		again = AGAIN_MIN;
		exposure = CLAMP(want / AGAIN_MIN, 2U, EXPOSURE_MAX);
	} else {
		exposure = EXPOSURE_MAX;
		again = CLAMP(want / EXPOSURE_MAX, AGAIN_MIN, AGAIN_MAX);
	}
	if (!fast) {
		/* Move half way to damp oscillation */
		exposure = (exposure + old_e) / 2U;
		again = (again + old_g) / 2U;
	}
	if ((exposure != old_e) || (again != old_g)) {
		sensor_set_exposure();
	}
}

static void af_begin(void)
{
	af.fine = false;
	af.pos = 0;
	af.end = LENS_MAX;
	af.step = AF_COARSE_STEP;
	af.best = 0;
	af.best_pos = 0;
	af.settle = 2;
	lens_set(0);
	status.focus = CAM_FOCUS_SCANNING;
}

/* One frame of the sweep: the lens moved two frames ago, so this one is sharp */
static void af_frame(void)
{
	if (af.settle > 0) {
		af.settle--;
		return;
	}
	if (sharpness > af.best) {
		af.best = sharpness;
		af.best_pos = af.pos;
	}
	af.pos += af.step;
	if (af.pos > af.end) {
		if (!af.fine) {
			af.fine = true;
			af.step = AF_FINE_STEP;
			af.pos = MAX(af.best_pos - AF_FINE_SPAN, 0);
			af.end = MIN(af.best_pos + AF_FINE_SPAN, (int)LENS_MAX);
		} else {
			lens_set(af.best_pos);
			status.focus = CAM_FOCUS_LOCKED;
			return;
		}
	}
	lens_set(af.pos);
	af.settle = 1;
}

static void shot_begin(bool use_flash)
{
	shot_flash = use_flash;
	status.shooting = true;
	if (use_flash) {
		saved_exposure = exposure;
		saved_again = again;
		(void)led_set_brightness(flash, FLASH_LED_FLASH, 100);
		/* Let exposure settle on the flash-lit scene, within its 500 ms */
		shot_frames = 4;
	} else {
		shot_frames = 0;
	}
}

static void shot_frame(void)
{
	int slot;

	if (shot_frames > 0) {
		shot_frames--;
		return;
	}
	slot = (status.last_photo + 1) % CAM_PHOTOS;
	develop_photo(photos[slot]);
	shot_frames = -1;
	if (shot_flash) {
		exposure = saved_exposure;
		again = saved_again;
		sensor_set_exposure();
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	status.last_photo = slot;
	status.photos++;
	status.shooting = false;
	k_spin_unlock(&lock, key);
}

static void handle(uint8_t cmd)
{
	switch (cmd) {
	case CMD_START:
		if (!status.running) {
			status.sensor_ok = start();
			status.running = status.sensor_ok;
		}
		break;
	case CMD_STOP:
		if (status.running) {
			stop();
			status.running = false;
			status.focus = CAM_FOCUS_IDLE;
			shot_frames = -1;
			status.shooting = false;
		}
		break;
	case CMD_FOCUS:
		if (status.running) {
			af_begin();
		}
		break;
	case CMD_SHOOT:
	case CMD_SHOOT_FLASH:
		if (status.running && (shot_frames < 0)) {
			shot_begin(cmd == CMD_SHOOT_FLASH);
		}
		break;
	}
}

static int next_buffer(void)
{
	for (int i = 0; i < 3; i++) {
		if ((i != latest) && (i != in_use)) {
			return i;
		}
	}
	return 0;
}

static void cam_thread(void *a, void *b, void *c)
{
	uint32_t frames = 0;
	int64_t t_fps = k_uptime_get();

	for (;;) {
		k_timeout_t wait = status.running ? K_NO_WAIT : K_FOREVER;
		uint8_t cmd;
		uint32_t mean;
		int buf;

		while (k_msgq_get(&cam_q, &cmd, wait) == 0) {
			handle(cmd);
			wait = K_NO_WAIT;
		}
		if (!status.running) {
			continue;
		}

		if (capture() == -ETIMEDOUT) {
			k_msleep(100);
			continue;
		}

		buf = next_buffer();
		mean = develop_preview(previews[buf]);

		if (shot_frames >= 0) {
			shot_frame();
		}
		if (status.focus == CAM_FOCUS_SCANNING) {
			af_frame();
		} else {
			auto_exposure(mean, shot_frames >= 0);
		}

		k_spinlock_key_t key = k_spin_lock(&lock);

		latest = buf;
		seq++;
		status.exposure = exposure;
		status.gain_x10 = again * 10U / AGAIN_MIN;
		k_spin_unlock(&lock, key);

		frames++;
		if (k_uptime_get() - t_fps >= 1000) {
			status.fps = frames;
			frames = 0;
			t_fps = k_uptime_get();
		}
	}
}

K_THREAD_STACK_DEFINE(cam_stack, 4096);
static struct k_thread cam_tid;

void cam_init(void)
{
	for (int i = 0; i < 1024; i++) {
		gamma_lut[i] = (uint8_t)(powf(i / 1023.0f, 1.0f / 2.2f) * 255.0f + 0.5f);
	}
	for (int ox = 0; ox < CAM_PREVIEW_W; ox++) {
		preview_xmap[ox] = (uint16_t)(ox * CELLS_W / CAM_PREVIEW_W);
	}
	/* Below the UI thread, so the preview never starves the interface */
	k_thread_create(&cam_tid, cam_stack, K_THREAD_STACK_SIZEOF(cam_stack), cam_thread, NULL,
			NULL, NULL, 12, 0, K_NO_WAIT);
}

static void post(uint8_t cmd)
{
	(void)k_msgq_put(&cam_q, &cmd, K_NO_WAIT);
}

void cam_start(void)
{
	post(CMD_START);
}

void cam_stop(void)
{
	post(CMD_STOP);
}

void cam_focus(void)
{
	post(CMD_FOCUS);
}

void cam_shoot(bool use_flash)
{
	post(use_flash ? CMD_SHOOT_FLASH : CMD_SHOOT);
}

const uint32_t *cam_preview(uint32_t *seq_out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint32_t *p = (latest >= 0) ? previews[latest] : NULL;

	*seq_out = seq;
	k_spin_unlock(&lock, key);
	return p;
}

void cam_preview_in_use(const uint32_t *buf)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	in_use = -1;
	for (int i = 0; i < 3; i++) {
		if (buf == previews[i]) {
			in_use = i;
		}
	}
	k_spin_unlock(&lock, key);
}

const uint32_t *cam_photo(int slot)
{
	return ((slot >= 0) && (slot < CAM_PHOTOS)) ? photos[slot] : NULL;
}

void cam_status_get(struct cam_status *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = status;
	k_spin_unlock(&lock, key);
}
