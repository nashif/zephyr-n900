#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer));
static atomic_t samples;
static int taps;

static void drdy(const struct device *dev, const struct sensor_trigger *trig)
{
	sensor_sample_fetch(dev);
	atomic_inc(&samples);
}

static void tap(const struct device *dev, const struct sensor_trigger *trig)
{
	struct sensor_value v[3];

	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v);
	printk("TAP %d  (x %.1f y %.1f z %.1f)\n", ++taps, sensor_value_to_double(&v[0]),
	       sensor_value_to_double(&v[1]), sensor_value_to_double(&v[2]));
}

int main(void)
{
	static const struct sensor_trigger t_drdy = {SENSOR_TRIG_DATA_READY, SENSOR_CHAN_ALL};
	static const struct sensor_trigger t_tap = {SENSOR_TRIG_TAP, SENSOR_CHAN_ALL};

	printk("N900 accel interrupts: %s\n", device_is_ready(accel) ? "ready" : "NOT READY");
	printk("data ready trigger: %d\n", sensor_trigger_set(accel, &t_drdy, drdy));
	printk("tap trigger: %d, tap the phone\n", sensor_trigger_set(accel, &t_tap, tap));

	for (int i = 0; i < 60; i++) {
		k_sleep(K_SECONDS(1));
		printk("%2d s: %ld samples/s, %d taps\n", i, (long)atomic_set(&samples, 0), taps);
	}
	return 0;
}
