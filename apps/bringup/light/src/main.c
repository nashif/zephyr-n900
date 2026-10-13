#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

static const struct device *const light = DEVICE_DT_GET(DT_NODELABEL(light_sensor));

int main(void)
{
	struct sensor_value lux;

	printk("N900 light: %s, cover the sensor, then shine a light on it\n",
	       device_is_ready(light) ? "ready" : "NOT READY");
	for (int i = 0; i < 120; i++) {
		int ret = sensor_sample_fetch(light);

		if (ret == 0) {
			ret = sensor_channel_get(light, SENSOR_CHAN_LIGHT, &lux);
		}
		printk("%3d  %6d.%02d lux  %s\n", i, lux.val1, lux.val2 / 10000,
		       ret == 0 ? "" : "error");
		k_msleep(500);
	}
	return 0;
}
