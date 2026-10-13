#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/drivers/mfd/twl4030.h>

static bool shift;

static const struct {
	uint16_t code;
	char c;
} keys[] = {
	{INPUT_KEY_A, 'a'}, {INPUT_KEY_B, 'b'}, {INPUT_KEY_C, 'c'}, {INPUT_KEY_D, 'd'},
	{INPUT_KEY_E, 'e'}, {INPUT_KEY_F, 'f'}, {INPUT_KEY_G, 'g'}, {INPUT_KEY_H, 'h'},
	{INPUT_KEY_I, 'i'}, {INPUT_KEY_J, 'j'}, {INPUT_KEY_K, 'k'}, {INPUT_KEY_L, 'l'},
	{INPUT_KEY_M, 'm'}, {INPUT_KEY_N, 'n'}, {INPUT_KEY_O, 'o'}, {INPUT_KEY_P, 'p'},
	{INPUT_KEY_Q, 'q'}, {INPUT_KEY_R, 'r'}, {INPUT_KEY_S, 's'}, {INPUT_KEY_T, 't'},
	{INPUT_KEY_U, 'u'}, {INPUT_KEY_V, 'v'}, {INPUT_KEY_W, 'w'}, {INPUT_KEY_X, 'x'},
	{INPUT_KEY_Y, 'y'}, {INPUT_KEY_Z, 'z'}, {INPUT_KEY_SPACE, ' '}, {INPUT_KEY_DOT, '.'},
	{INPUT_KEY_COMMA, ','}, {INPUT_KEY_ENTER, '\n'}, {INPUT_KEY_BACKSPACE, '\b'},
};

static const struct {
	uint16_t code;
	const char *name;
} named[] = {
	{INPUT_KEY_UP, "<UP>"}, {INPUT_KEY_DOWN, "<DOWN>"}, {INPUT_KEY_LEFT, "<LEFT>"},
	{INPUT_KEY_RIGHT, "<RIGHT>"}, {INPUT_KEY_LEFTCTRL, "<CTRL>"},
	{INPUT_KEY_RIGHTALT, "<FN>"}, {INPUT_KEY_F7, "<F7>"}, {INPUT_KEY_F8, "<F8>"},
	{INPUT_KEY_F9, "<F9>"}, {INPUT_KEY_F10, "<F10>"}, {INPUT_KEY_F11, "<F11>"}, {INPUT_KEY_COFFEE, "<LOCK>"},
};

static void key_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if ((evt->type != INPUT_EV_KEY) || (evt->code == INPUT_BTN_TOUCH)) {
		return;
	}

	if (evt->code == INPUT_KEY_POWER) {
		printk("%s", evt->value ? "<POWER>" : "<power released>");
		return;
	}

	if (evt->code == INPUT_KEY_LEFTSHIFT) {
		shift = (evt->value != 0);
		return;
	}

	if (evt->value == 0) {
		if (evt->code == INPUT_KEY_COFFEE) {
			printk("<unlock>");
		}
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		if (keys[i].code == evt->code) {
			char c = keys[i].c;

			if (shift && (c >= 'a') && (c <= 'z')) {
				c = c - 'a' + 'A';
			}
			printk("%c", c);
			return;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(named); i++) {
		if (named[i].code == evt->code) {
			printk("%s", named[i].name);
			return;
		}
	}

	printk("<%u>", evt->code);
}

INPUT_CALLBACK_DEFINE(NULL, key_cb, NULL);

static const struct device *const twl = DEVICE_DT_GET(DT_NODELABEL(twl));

static bool vbus_present(void)
{
	uint8_t sts = 0;

	mfd_twl4030_reg_read(twl, TWL4030_ADDR_PM_MASTER, TWL4030_STS_HW_CONDITIONS, &sts);
	return (sts & TWL4030_STS_VBUS) != 0;
}

static void usb_cb(const struct device *dev, struct twl4030_irq_callback *cb, uint8_t status)
{
	printk("[USB %s]", vbus_present() ? "plugged" : "unplugged");
}

static struct twl4030_irq_callback usb_irq = {
	.handler = usb_cb,
	.module = TWL4030_IRQ_POWER,
	.mask = TWL4030_POWER_IRQ_USB_PRES,
};

int main(void)
{
	int ret = mfd_twl4030_add_irq_callback(twl, &usb_irq);

	printk("N900 keyboard: type, press power, plug USB (USB %s, events %s)\n\n> ",
	       vbus_present() ? "present" : "absent", ret == 0 ? "on" : "off");
	return 0;
}
