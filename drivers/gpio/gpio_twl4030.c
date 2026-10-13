/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * GPIO controller of the TI TWL4030 and TWL5030 power management chips, a
 * child of the chip's MFD device: 18 pins, each an input or an output with
 * an optional pull-up or pull-down. Interrupts are not supported.
 */

#define DT_DRV_COMPAT ti_twl4030_gpio

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

/* GPIO registers, at the chip's second I2C address */
#define GPIO_ADDR_IDX 1U
#define GPIO_BASE     0x98U
#define GPIODATAIN1   (GPIO_BASE + 0x00U)
#define GPIODATADIR1  (GPIO_BASE + 0x03U)
#define GPIODATAOUT1  (GPIO_BASE + 0x06U)
#define GPIOCLEAR1    (GPIO_BASE + 0x09U)
#define GPIOSET1      (GPIO_BASE + 0x0CU)
#define GPIO_CTRL     (GPIO_BASE + 0x12U)
#define GPIOPUPDCTR1  (GPIO_BASE + 0x13U)

#define GPIO_CTRL_GPIO_ON BIT(2)
/* Two bits per pin: pull-down, then pull-up */
#define PUPD_DOWN         0x1U
#define PUPD_UP           0x2U

#define NUM_PINS 18U

struct twl4030_gpio_config {
	struct gpio_driver_config common;
	const struct device *mfd;
};

struct twl4030_gpio_data {
	struct gpio_driver_data common;
	struct k_mutex lock;
};

static int read_bank(const struct device *dev, uint8_t reg, uint32_t *val)
{
	const struct twl4030_gpio_config *config = dev->config;
	uint8_t buf[3];
	int ret;

	ret = mfd_twl4030_reg_read_burst(config->mfd, GPIO_ADDR_IDX, reg, buf, sizeof(buf));
	if (ret == 0) {
		*val = sys_get_le24(buf);
	}

	return ret;
}

/* Writes the bytes of a three-register bank that have bits in the mask */
static int write_bank(const struct device *dev, uint8_t reg, uint32_t val, uint32_t mask)
{
	const struct twl4030_gpio_config *config = dev->config;

	for (uint8_t i = 0U; i < 3U; i++) {
		if (((mask >> (8U * i)) & 0xFFU) != 0U) {
			int ret = mfd_twl4030_reg_write(config->mfd, GPIO_ADDR_IDX, reg + i,
							(uint8_t)(val >> (8U * i)));

			if (ret != 0) {
				return ret;
			}
		}
	}

	return 0;
}

static int set_clear(const struct device *dev, uint32_t set, uint32_t clear)
{
	int ret = 0;

	if (set != 0U) {
		ret = write_bank(dev, GPIOSET1, set, set);
	}
	if ((ret == 0) && (clear != 0U)) {
		ret = write_bank(dev, GPIOCLEAR1, clear, clear);
	}

	return ret;
}

static int twl4030_gpio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct twl4030_gpio_config *config = dev->config;
	struct twl4030_gpio_data *data = dev->data;
	uint8_t pupd_reg = GPIOPUPDCTR1 + pin / 4U;
	uint8_t shift = 2U * (pin % 4U);
	uint8_t pupd = 0U;
	uint32_t dir;
	uint8_t old;
	int ret;

	if (pin >= NUM_PINS) {
		return -EINVAL;
	}
	if ((flags & (GPIO_SINGLE_ENDED | GPIO_INT_ENABLE)) != 0U) {
		return -ENOTSUP;
	}
	if ((flags & GPIO_PULL_UP) != 0U) {
		pupd = PUPD_UP;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pupd = PUPD_DOWN;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = mfd_twl4030_reg_read(config->mfd, GPIO_ADDR_IDX, pupd_reg, &old);
	if (ret == 0) {
		old = (old & ~(0x3U << shift)) | (pupd << shift);
		ret = mfd_twl4030_reg_write(config->mfd, GPIO_ADDR_IDX, pupd_reg, old);
	}

	if ((ret == 0) && ((flags & GPIO_OUTPUT) != 0U)) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			ret = set_clear(dev, BIT(pin), 0U);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			ret = set_clear(dev, 0U, BIT(pin));
		}
	}

	if (ret == 0) {
		ret = read_bank(dev, GPIODATADIR1, &dir);
	}
	if (ret == 0) {
		dir = ((flags & GPIO_OUTPUT) != 0U) ? (dir | BIT(pin)) : (dir & ~BIT(pin));
		ret = write_bank(dev, GPIODATADIR1, dir, BIT(pin));
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int twl4030_gpio_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	uint32_t in;
	int ret;

	ret = read_bank(dev, GPIODATAIN1, &in);
	if (ret == 0) {
		*value = in & BIT_MASK(NUM_PINS);
	}

	return ret;
}

static int twl4030_gpio_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					    gpio_port_value_t value)
{
	return set_clear(dev, value & mask, ~value & mask);
}

static int twl4030_gpio_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return set_clear(dev, pins, 0U);
}

static int twl4030_gpio_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return set_clear(dev, 0U, pins);
}

static int twl4030_gpio_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	struct twl4030_gpio_data *data = dev->data;
	uint32_t out;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = read_bank(dev, GPIODATAOUT1, &out);
	if (ret == 0) {
		ret = set_clear(dev, ~out & pins, out & pins);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(gpio, twl4030_gpio_api) = {
	.pin_configure = twl4030_gpio_pin_configure,
	.port_get_raw = twl4030_gpio_port_get_raw,
	.port_set_masked_raw = twl4030_gpio_port_set_masked_raw,
	.port_set_bits_raw = twl4030_gpio_port_set_bits_raw,
	.port_clear_bits_raw = twl4030_gpio_port_clear_bits_raw,
	.port_toggle_bits = twl4030_gpio_port_toggle_bits,
};

static int twl4030_gpio_init(const struct device *dev)
{
	const struct twl4030_gpio_config *config = dev->config;
	struct twl4030_gpio_data *data = dev->data;
	uint8_t ctrl;
	int ret;

	if (!device_is_ready(config->mfd)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

	ret = mfd_twl4030_reg_read(config->mfd, GPIO_ADDR_IDX, GPIO_CTRL, &ctrl);
	if (ret != 0) {
		return ret;
	}

	return mfd_twl4030_reg_write(config->mfd, GPIO_ADDR_IDX, GPIO_CTRL,
				     ctrl | GPIO_CTRL_GPIO_ON);
}

#define TWL4030_GPIO_INIT(inst)                                                                    \
	static const struct twl4030_gpio_config twl4030_gpio_config_##inst = {                     \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(inst)},                \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
	static struct twl4030_gpio_data twl4030_gpio_data_##inst;                                  \
	DEVICE_DT_INST_DEFINE(inst, twl4030_gpio_init, NULL, &twl4030_gpio_data_##inst,            \
			      &twl4030_gpio_config_##inst, POST_KERNEL,                            \
			      CONFIG_GPIO_TWL4030_INIT_PRIORITY, &twl4030_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(TWL4030_GPIO_INIT)
