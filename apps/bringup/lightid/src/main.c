#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>

static const struct device *const i2c2 = DEVICE_DT_GET(DT_NODELABEL(i2c2));

static int rd(uint8_t reg, uint8_t *buf, size_t len)
{
	/* Command bit, word bit for 16-bit reads */
	uint8_t cmd = 0x80 | (len > 1 ? 0x20 : 0) | reg;

	return i2c_write_read(i2c2, 0x29, &cmd, 1, buf, len);
}

int main(void)
{
	uint8_t on[2] = {0x80, 0x03};
	uint8_t id = 0, d[4] = {0};
	int ret = rd(0x0a, &id, 1);

	printk("TSL256x @0x29: id read %d, ID 0x%02x (partno %u rev %u)\n", ret, id, id >> 4,
	       id & 0xf);
	i2c_write(i2c2, on, 2, 0x29);
	for (int i = 0; i < 20; i++) {
		k_msleep(1000);
		ret = rd(0x0c, d, 4);
		printk("%2d ch0 %5u ch1 %5u (%d)\n", i, d[0] | (d[1] << 8), d[2] | (d[3] << 8), ret);
	}
	return 0;
}
