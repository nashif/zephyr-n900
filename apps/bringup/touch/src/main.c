#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/sys_io.h>

static int32_t x, y;
static uint32_t samples;
static bool down;

static void touch_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_ABS) {
		if (evt->code == INPUT_ABS_X) {
			x = evt->value;
		} else if (evt->code == INPUT_ABS_Y) {
			y = evt->value;
		}
	} else if ((evt->type == INPUT_EV_KEY) && (evt->code == INPUT_BTN_TOUCH)) {
		if (evt->value != 0) {
			if (!down) {
				printk("down %4d %4d\n", x, y);
				samples = 0;
			}
			down = true;
			samples++;
			if ((samples % 25U) == 0U) {
				printk("     %4d %4d\n", x, y);
			}
		} else {
			printk("up   %4d %4d (%u samples)\n", x, y, samples);
			down = false;
		}
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(touchscreen)), touch_cb, NULL);

int main(void)
{
	printk("N900 touch: screen coordinates, touch anywhere\n");
	return 0;
}
