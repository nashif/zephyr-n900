#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/sys_io.h>

static const char *name(uint8_t type, uint16_t code)
{
	if (type == INPUT_EV_SW) {
		switch (code) {
		case INPUT_SW_KEYPAD_SLIDE: return "keypad slide out";
		case INPUT_SW_FRONT_PROXIMITY: return "proximity";
		case INPUT_SW_CAMERA_LENS_COVER: return "lens cover closed";
		case INPUT_SW_MACHINE_COVER: return "back cover closed";
		default: return "switch ?";
		}
	}
	switch (code) {
	case INPUT_KEY_CAMERA_FOCUS: return "camera focus";
	case INPUT_KEY_CAMERA: return "camera capture";
	case INPUT_KEY_COFFEE: return "lock";
	case INPUT_KEY_POWER: return "power";
	default: return NULL;
	}
}

static void cb(struct input_event *evt, void *user_data)
{
	const char *n;

	ARG_UNUSED(user_data);
	if ((evt->type != INPUT_EV_KEY) && (evt->type != INPUT_EV_SW)) {
		return;
	}
	n = name(evt->type, evt->code);
	if (n == NULL) {
		return;
	}
	printk("%6u ms  %-18s %s\n", k_uptime_get_32(), n,
	       evt->type == INPUT_EV_SW ? (evt->value ? "ON" : "off")
					: (evt->value ? "pressed" : "released"));
}

INPUT_CALLBACK_DEFINE(NULL, cb, NULL);

int main(void)
{
	printk("N900 switches: slide the keyboard, cover the proximity sensor,\n"
	       "open the lens cover, press the camera key halfway and fully\n");
	/* DATAIN of gpio3, gpio4 and gpio6 */
	printk("gpio3 %08x gpio4 %08x gpio6 %08x\n", sys_read32(0x49052038),
	       sys_read32(0x49054038), sys_read32(0x49058038));
	return 0;
}
