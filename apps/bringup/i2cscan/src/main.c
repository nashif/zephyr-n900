#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/sys_io.h>

#define I2C2      0x48072000
#define STAT      0x08
#define SYSTEST   0x3c
#define PAD_GPIO41 0x48002088

static const struct device *const i2c2 = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static const struct device *const gpio2 = DEVICE_DT_GET(DT_NODELABEL(gpio2));

/* SCL and SDA as seen in IO test mode, lines released */
static void lines(const char *what)
{
	uint16_t st;

	sys_write16(BIT(15) | (3 << 12) | BIT(2) | BIT(0), I2C2 + SYSTEST);
	k_busy_wait(10);
	st = sys_read16(I2C2 + SYSTEST);
	sys_write16(0, I2C2 + SYSTEST);
	printk("%-12s SCL %u SDA %u  STAT %04x\n", what, (st >> 3) & 1, (st >> 1) & 1,
	       sys_read16(I2C2 + STAT));
}

static void probe(const char *what)
{
	static const uint16_t addrs[] = {0x29, 0x32, 0x55, 0x6b};

	printk("%-12s", what);
	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		uint8_t reg = 0, val = 0;
		uint32_t t = k_uptime_get_32();
		int ret = i2c_write_read(i2c2, addrs[i], &reg, 1, &val, 1);

		printk(" %02x:%d(%ums)", addrs[i], ret, k_uptime_get_32() - t);
	}
	printk("\n");
}

int main(void)
{
	printk("pad gpio41 %04x  gpio2 OE %08x DATAOUT %08x\n", sys_read16(PAD_GPIO41),
	       sys_read32(0x49050034), sys_read32(0x4905003c));
	lines("boot");
	probe("boot");
	printk("recover %d\n", i2c_recover_bus(i2c2));
	lines("recovered");
	probe("recovered");
	gpio_pin_configure(gpio2, 9, GPIO_OUTPUT_LOW);
	k_msleep(10);
	lines("EN low");
	probe("EN low");
	gpio_pin_set(gpio2, 9, 1);
	k_msleep(10);
	lines("EN high");
	probe("EN high");
	probe("again");
	/* Register reset, as the LED driver does */
	{
		uint8_t rst[2] = {0x3d, 0xff};

		printk("reset write %d\n", i2c_write(i2c2, rst, 2, 0x32));
	}
	k_msleep(10);
	probe("after reset");
	printk("done\n");
	return 0;
}
