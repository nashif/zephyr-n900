#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer));

int main(void)
{
	struct sensor_value v[3];

	printk("N900 accel: %s, tilt the phone in each direction\n",
	       device_is_ready(accel) ? "ready" : "NOT READY");
	for (int i = 0; i < 150; i++) {
		int ret = sensor_sample_fetch(accel);

		if (ret == 0) {
			ret = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v);
		}
		if (ret != 0) {
			printk("%3d error %d\n", i, ret);
		} else {
			printk("%3d  x %6.2f  y %6.2f  z %6.2f m/s2\n", i, sensor_value_to_double(&v[0]),
			       sensor_value_to_double(&v[1]), sensor_value_to_double(&v[2]));
		}
		k_msleep(400);
	}
	return 0;
}
