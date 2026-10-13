/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * GPIO banks of TI OMAP3 SoCs: 32 pins each, with the register layout that
 * precedes the one of later OMAP and AM335x parts. Pull resistors belong to
 * the pad configuration and are not handled here.
 */

#define DT_DRV_COMPAT ti_omap3_gpio

#include <errno.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#define GPIO_IRQSTATUS1      0x018U
#define GPIO_OE              0x034U
#define GPIO_DATAIN          0x038U
#define GPIO_DATAOUT         0x03CU
#define GPIO_LEVELDETECT0    0x040U
#define GPIO_LEVELDETECT1    0x044U
#define GPIO_RISINGDETECT    0x048U
#define GPIO_FALLINGDETECT   0x04CU
#define GPIO_CLEARIRQENABLE1 0x060U
#define GPIO_SETIRQENABLE1   0x064U
#define GPIO_CLEARDATAOUT    0x090U
#define GPIO_SETDATAOUT      0x094U

struct gpio_omap3_config {
	struct gpio_driver_config common;
	mm_reg_t base;
	void (*irq_config)(void);
};

struct gpio_omap3_data {
	struct gpio_driver_data common;
	sys_slist_t callbacks;
};

static uint32_t reg_read(const struct device *dev, uint32_t reg)
{
	const struct gpio_omap3_config *config = dev->config;

	return sys_read32(config->base + reg);
}

static void reg_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct gpio_omap3_config *config = dev->config;

	sys_write32(val, config->base + reg);
}

static void reg_update(const struct device *dev, uint32_t reg, uint32_t mask, bool set)
{
	uint32_t val = reg_read(dev, reg);

	reg_write(dev, reg, set ? (val | mask) : (val & ~mask));
}

static int gpio_omap3_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	uint32_t mask = BIT(pin);
	unsigned int key;

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_SINGLE_ENDED)) != 0U) {
		return -ENOTSUP;
	}
	if ((flags & (GPIO_INPUT | GPIO_OUTPUT)) == (GPIO_INPUT | GPIO_OUTPUT)) {
		return -ENOTSUP;
	}

	key = irq_lock();
	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			reg_write(dev, GPIO_SETDATAOUT, mask);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			reg_write(dev, GPIO_CLEARDATAOUT, mask);
		}
		/* OE is active low: 0 enables the output driver */
		reg_update(dev, GPIO_OE, mask, false);
	} else {
		reg_update(dev, GPIO_OE, mask, true);
	}
	irq_unlock(key);

	return 0;
}

static int gpio_omap3_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	*value = reg_read(dev, GPIO_DATAIN);

	return 0;
}

static int gpio_omap3_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					  gpio_port_value_t value)
{
	reg_write(dev, GPIO_SETDATAOUT, mask & value);
	reg_write(dev, GPIO_CLEARDATAOUT, mask & ~value);

	return 0;
}

static int gpio_omap3_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	reg_write(dev, GPIO_SETDATAOUT, pins);

	return 0;
}

static int gpio_omap3_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	reg_write(dev, GPIO_CLEARDATAOUT, pins);

	return 0;
}

static int gpio_omap3_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	uint32_t out = reg_read(dev, GPIO_DATAOUT);

	reg_write(dev, GPIO_SETDATAOUT, pins & ~out);
	reg_write(dev, GPIO_CLEARDATAOUT, pins & out);

	return 0;
}

static int gpio_omap3_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					      enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	uint32_t mask = BIT(pin);
	bool level = (mode == GPIO_INT_MODE_LEVEL);
	bool high = (trig & GPIO_INT_TRIG_HIGH) != 0U;
	bool low = (trig & GPIO_INT_TRIG_LOW) != 0U;
	unsigned int key;

	key = irq_lock();
	reg_write(dev, GPIO_CLEARIRQENABLE1, mask);
	reg_update(dev, GPIO_LEVELDETECT0, mask, false);
	reg_update(dev, GPIO_LEVELDETECT1, mask, false);
	reg_update(dev, GPIO_RISINGDETECT, mask, false);
	reg_update(dev, GPIO_FALLINGDETECT, mask, false);

	if (mode != GPIO_INT_MODE_DISABLED) {
		reg_update(dev, level ? GPIO_LEVELDETECT1 : GPIO_RISINGDETECT, mask, high);
		reg_update(dev, level ? GPIO_LEVELDETECT0 : GPIO_FALLINGDETECT, mask, low);
		reg_write(dev, GPIO_IRQSTATUS1, mask);
		reg_write(dev, GPIO_SETIRQENABLE1, mask);
	}
	irq_unlock(key);

	return 0;
}

static int gpio_omap3_manage_callback(const struct device *dev, struct gpio_callback *callback,
				      bool set)
{
	struct gpio_omap3_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void gpio_omap3_isr(const void *arg)
{
	const struct device *dev = arg;
	struct gpio_omap3_data *data = dev->data;
	uint32_t status = reg_read(dev, GPIO_IRQSTATUS1);

	reg_write(dev, GPIO_IRQSTATUS1, status);
	gpio_fire_callbacks(&data->callbacks, dev, status);
}

static DEVICE_API(gpio, gpio_omap3_api) = {
	.pin_configure = gpio_omap3_pin_configure,
	.port_get_raw = gpio_omap3_port_get_raw,
	.port_set_masked_raw = gpio_omap3_port_set_masked_raw,
	.port_set_bits_raw = gpio_omap3_port_set_bits_raw,
	.port_clear_bits_raw = gpio_omap3_port_clear_bits_raw,
	.port_toggle_bits = gpio_omap3_port_toggle_bits,
	.pin_interrupt_configure = gpio_omap3_pin_interrupt_configure,
	.manage_callback = gpio_omap3_manage_callback,
};

static int gpio_omap3_init(const struct device *dev)
{
	const struct gpio_omap3_config *config = dev->config;

	config->irq_config();

	return 0;
}

#define GPIO_OMAP3_INIT(inst)                                                                      \
	static void gpio_omap3_irq_config_##inst(void)                                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), gpio_omap3_isr,       \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}                                                                                          \
	static const struct gpio_omap3_config gpio_omap3_config_##inst = {                         \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(inst),                                   \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.irq_config = gpio_omap3_irq_config_##inst,                                        \
	};                                                                                         \
	static struct gpio_omap3_data gpio_omap3_data_##inst;                                      \
	DEVICE_DT_INST_DEFINE(inst, gpio_omap3_init, NULL, &gpio_omap3_data_##inst,                \
			      &gpio_omap3_config_##inst, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,  \
			      &gpio_omap3_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_OMAP3_INIT)
