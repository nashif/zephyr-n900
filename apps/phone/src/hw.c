/* N900 phone demo: hardware side */
#include <zephyr/kernel.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/haptics.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <lvgl.h>

#include "phone.h"

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const touch = DEVICE_DT_GET(DT_NODELABEL(touchscreen));
static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(leds));
static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer));
static const struct device *const light = DEVICE_DT_GET(DT_NODELABEL(light_sensor));
static const struct device *const gauge = DEVICE_DT_GET(DT_NODELABEL(fuel_gauge));
static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(charger));
static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));
static const struct device *const flash = DEVICE_DT_GET(DT_NODELABEL(flash));
static const struct device *const vibrator = DEVICE_DT_GET(DT_NODELABEL(vibrator));

/* LP5523 channels */
#define LED_BLUE  4U
#define LED_GREEN 5U
#define LED_RED   6U
static const uint8_t kbd_leds[] = {0, 1, 2, 3, 7, 8};

K_MSGQ_DEFINE(phone_q, sizeof(struct phone_event), 64, 4);

static struct phone_sensors sensors;
static struct k_spinlock sensors_lock;

void phone_post(uint8_t type, uint16_t code, int32_t value)
{
	struct phone_event e = {.type = type, .code = code, .value = value};

	(void)k_msgq_put(&phone_q, &e, K_NO_WAIT);
}

void phone_sensors_get(struct phone_sensors *out)
{
	k_spinlock_key_t key = k_spin_lock(&sensors_lock);

	*out = sensors;
	k_spin_unlock(&sensors_lock, key);
}

/* Keys and switches, everything but the touchscreen, which LVGL reads */
static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->dev == touch) {
		return;
	}

	if (evt->type == INPUT_EV_KEY) {
		phone_post(PEV_KEY, evt->code, evt->value);
	} else if (evt->type == INPUT_EV_SW) {
		phone_post(PEV_SWITCH, evt->code, evt->value);
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static void tap_cb(const struct device *dev, const struct sensor_trigger *trig)
{
	phone_post(PEV_TAP, 0, 0);
}

static bool vbus_present(void)
{
	uint8_t sts = 0U;

	(void)mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, TWL4030_STS_HW_CONDITIONS, &sts);
	return (sts & TWL4030_STS_VBUS) != 0U;
}

static void usb_cb(const struct device *dev, struct twl4030_irq_callback *cb, uint8_t status)
{
	phone_post(PEV_USB, 0, vbus_present());
}

static struct twl4030_irq_callback usb_irq = {
	.handler = usb_cb,
	.module = TWL4030_IRQ_POWER,
	.mask = TWL4030_POWER_IRQ_USB_PRES,
};

/* Accelerometer at 20 Hz */
static void accel_thread(void *a, void *b, void *c)
{
	struct sensor_value v[3];

	while (true) {
		if (device_is_ready(accel) && (sensor_sample_fetch(accel) == 0) &&
		    (sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v) == 0)) {
			k_spinlock_key_t key = k_spin_lock(&sensors_lock);

			sensors.accel_ok = true;
			sensors.ax = v[0].val1 * 100 + v[0].val2 / 10000;
			sensors.ay = v[1].val1 * 100 + v[1].val2 / 10000;
			sensors.az = v[2].val1 * 100 + v[2].val2 / 10000;
			k_spin_unlock(&sensors_lock, key);
		}
		k_msleep(50);
	}
}

/* Light every second (the sensor integrates for 402 ms), battery every 5 s */
static void slow_thread(void *a, void *b, void *c)
{
	for (uint32_t n = 0U;; n++) {
		struct sensor_value lux;
		bool light_ok = device_is_ready(light) && (sensor_sample_fetch(light) == 0) &&
				(sensor_channel_get(light, SENSOR_CHAN_LIGHT, &lux) == 0);
		k_spinlock_key_t key = k_spin_lock(&sensors_lock);

		sensors.light_ok = light_ok;
		if (light_ok) {
			sensors.lux = lux.val1;
		}
		k_spin_unlock(&sensors_lock, key);

		if ((n % 5U) == 0U) {
			union fuel_gauge_prop_val v;
			union charger_propval cv;
			struct phone_sensors s = {0};

			s.gauge_ok = device_is_ready(gauge) &&
				     (fuel_gauge_get_prop(gauge,
							  FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT,
							  &v) == 0);
			if (s.gauge_ok) {
				s.soc = v.relative_state_of_charge_pct;
				fuel_gauge_get_prop(gauge, FUEL_GAUGE_VOLTAGE_UV, &v);
				s.mv = v.voltage_uv / 1000;
				fuel_gauge_get_prop(gauge, FUEL_GAUGE_CURRENT_UA, &v);
				s.ma = v.current_ua / 1000;
				fuel_gauge_get_prop(gauge, FUEL_GAUGE_TEMPERATURE_DK, &v);
				s.temp_dc = (int)v.temperature_dk - 2731;
			}
			s.charger = CHARGER_STATUS_UNKNOWN;
			if (device_is_ready(charger) &&
			    (charger_get_prop(charger, CHARGER_PROP_STATUS, &cv) == 0)) {
				s.charger = cv.status;
			}
			s.vbus = vbus_present();

			key = k_spin_lock(&sensors_lock);
			sensors.gauge_ok = s.gauge_ok;
			sensors.soc = s.soc;
			sensors.mv = s.mv;
			sensors.ma = s.ma;
			sensors.temp_dc = s.temp_dc;
			sensors.charger = s.charger;
			sensors.vbus = s.vbus;
			k_spin_unlock(&sensors_lock, key);
		}
		k_msleep(600);
	}
}

K_THREAD_STACK_DEFINE(accel_stack, 2048);
K_THREAD_STACK_DEFINE(slow_stack, 2048);
static struct k_thread accel_tid;
static struct k_thread slow_tid;

void hw_init(void)
{
	static const struct sensor_trigger tap = {SENSOR_TRIG_TAP, SENSOR_CHAN_ALL};

	if (device_is_ready(accel)) {
		(void)sensor_trigger_set(accel, &tap, tap_cb);
	}
	(void)mfd_twl4030_add_irq_callback(twl, &usb_irq);

	k_thread_create(&accel_tid, accel_stack, K_THREAD_STACK_SIZEOF(accel_stack),
			accel_thread, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
	k_thread_create(&slow_tid, slow_stack, K_THREAD_STACK_SIZEOF(slow_stack), slow_thread,
			NULL, NULL, NULL, 11, 0, K_NO_WAIT);
}

void hw_screen(bool on, uint8_t brightness)
{
	if (on) {
		(void)display_blanking_off(display);
		(void)display_set_brightness(display, brightness);
	} else {
		(void)display_set_brightness(display, 0);
		(void)display_blanking_on(display);
	}
}

void hw_kbd_backlight(uint8_t pct)
{
	static int last = -1;

	if (pct == last) {
		return;
	}
	last = pct;

	for (size_t i = 0; i < ARRAY_SIZE(kbd_leds); i++) {
		led_set_brightness(leds, kbd_leds[i], pct);
	}
}

void hw_status_led(uint8_t r, uint8_t g, uint8_t b)
{
	static uint32_t last = UINT32_MAX;
	uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	uint8_t ch[3] = {b, g, r};

	if (rgb == last) {
		return;
	}
	last = rgb;

	/* Blue, green and red are consecutive channels */
	for (int i = 0; i < 3; i++) {
		ch[i] = (uint8_t)((ch[i] * 255U) / 100U);
	}
	(void)led_write_channels(leds, LED_BLUE, 3, ch);
}

/* The camera flash LED in its continuous torch mode */
void hw_torch(bool on)
{
	(void)led_set_brightness(flash, 0, on ? 100 : 0);
}

static void vibra_off(struct k_work *work)
{
	(void)haptics_stop_output(vibrator);
}

static K_WORK_DELAYABLE_DEFINE(vibra_work, vibra_off);

/* Runs the motor for a while; a new call restarts the time */
void hw_vibrate(uint32_t ms)
{
	if (!device_is_ready(vibrator)) {
		return;
	}
	(void)haptics_start_output(vibrator);
	k_work_reschedule(&vibra_work, K_MSEC(ms));
}

/* Main loop: drain the event queue and run LVGL */
int main(void)
{
	struct phone_event e;

	ui_init();
	hw_init();

	while (true) {
		while (k_msgq_get(&phone_q, &e, K_NO_WAIT) == 0) {
			ui_event(&e);
		}
		uint32_t sleep = lv_timer_handler();

		k_msleep(CLAMP(sleep, 5U, 30U));
	}
	return 0;
}
