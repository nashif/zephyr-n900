#include <zephyr/kernel.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/display.h>

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(leds));
static const struct device *const panel = DEVICE_DT_GET(DT_NODELABEL(panel));
static const uint8_t kbd[] = {0, 1, 2, 3, 7, 8};

static void kbd_set(uint8_t pct)
{
	for (size_t i = 0; i < ARRAY_SIZE(kbd); i++) {
		led_set_brightness(leds, kbd[i], pct);
	}
}

int main(void)
{
	static const struct {
		uint8_t ch;
		const char *name;
	} rgb[] = {{6, "red"}, {5, "green"}, {4, "blue"}};

	printk("N900 LEDs: %s, panel: %s\n", device_is_ready(leds) ? "ready" : "NOT READY",
	       device_is_ready(panel) ? "ready" : "NOT READY");
	if (!device_is_ready(leds)) {
		return 0;
	}
	for (int round = 0; round < 5; round++) {
		for (size_t i = 0; i < ARRAY_SIZE(rgb); i++) {
			printk("status LED %s\n", rgb[i].name);
			led_set_brightness(leds, rgb[i].ch, 100);
			k_msleep(1500);
			led_set_brightness(leds, rgb[i].ch, 0);
		}
		printk("keyboard backlight fade\n");
		for (int p = 0; p <= 100; p += 5) {
			kbd_set(p);
			k_msleep(60);
		}
		k_msleep(1000);
		for (int p = 100; p >= 0; p -= 5) {
			kbd_set(p);
			k_msleep(60);
		}
		printk("screen brightness 255 -> 10 -> 255: %d\n",
		       display_set_brightness(panel, 255));
		for (int b = 255; b >= 10; b -= 5) {
			display_set_brightness(panel, b);
			k_msleep(40);
		}
		k_msleep(1000);
		for (int b = 10; b <= 255; b += 5) {
			display_set_brightness(panel, b);
			k_msleep(40);
		}
	}
	printk("done\n");
	return 0;
}
