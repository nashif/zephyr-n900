/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Analog Devices ADP1653 camera flash LED driver on I2C. LED 0 is the
 * high-power LED in torch mode, LED 1 the indicator LED and LED 2 the flash:
 * any non-zero brightness on LED 2 fires one flash, which the chip ends after
 * flash-timeout-us.
 */

#define DT_DRV_COMPAT adi_adp1653

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(adp1653, CONFIG_LED_LOG_LEVEL);

#define REG_OUT_SEL   0x00U
#define REG_CONFIG    0x01U
#define REG_SW_STROBE 0x02U
#define REG_FAULT     0x03U

#define OUT_SEL_HPLED_SHIFT 3
#define OUT_SEL_ILED_MASK   GENMASK(2, 0)
#define CFG_TMR_EN          BIT(4)
#define SW_STROBE           BIT(0)

/* High-power LED current: 35 mA + 15 mA per code */
#define HPLED_BASE_UA   35000U
#define HPLED_STEP_UA   15000U
#define HPLED_TORCH_MAX 0x0BU
#define HPLED_FLASH_MAX 0x1FU
/* Indicator LED current: 2.5 mA per code */
#define ILED_STEP_UA    2500U
/* Flash timer: 820 ms less 54.6 ms per code */
#define TIMER_MAX_US    820000U
#define TIMER_STEP_US   54600U

/* Code of the highest current not above ua, at least the lowest one */
#define HPLED_CODE(ua, max)                                                                        \
	CLAMP(((ua) - HPLED_BASE_UA) / HPLED_STEP_UA, 1, (max))

#define LED_TORCH     0U
#define LED_INDICATOR 1U
#define LED_FLASH     2U

struct adp1653_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec enable_gpio;
	uint8_t torch_max;
	uint8_t flash_code;
	uint8_t timer_code;
	uint8_t iled_max;
	uint32_t timeout_us;
};

struct adp1653_data {
	const struct device *dev;
	struct k_mutex lock;
	struct k_work_delayable flash_end;
	bool flashing;
	uint8_t hpled;
	uint8_t iled;
};

static int write_out_sel(const struct device *dev)
{
	const struct adp1653_config *config = dev->config;
	struct adp1653_data *data = dev->data;

	return i2c_reg_write_byte_dt(&config->bus, REG_OUT_SEL,
				     (data->hpled << OUT_SEL_HPLED_SHIFT) | data->iled);
}

static int adp1653_flash(const struct device *dev)
{
	const struct adp1653_config *config = dev->config;
	struct adp1653_data *data = dev->data;
	uint8_t fault;
	int ret;

	/* Clear faults left by a previous flash, such as the timeout */
	ret = i2c_reg_read_byte_dt(&config->bus, REG_FAULT, &fault);
	if (ret != 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->bus, REG_CONFIG, CFG_TMR_EN | config->timer_code);
	if (ret != 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->bus, REG_OUT_SEL,
				    (config->flash_code << OUT_SEL_HPLED_SHIFT) | data->iled);
	if (ret != 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->bus, REG_SW_STROBE, SW_STROBE);
	if (ret != 0) {
		return ret;
	}

	/* The timer ends the flash; return to torch mode only after it */
	data->flashing = true;
	data->hpled = 0U;
	k_work_schedule(&data->flash_end, K_USEC(config->timeout_us + TIMER_STEP_US));

	return 0;
}

static void adp1653_flash_end(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct adp1653_data *data = CONTAINER_OF(dwork, struct adp1653_data, flash_end);
	const struct adp1653_config *config = data->dev->config;

	k_mutex_lock(&data->lock, K_FOREVER);
	/* LED off before the timer is disabled, which would light it */
	(void)write_out_sel(data->dev);
	(void)i2c_reg_write_byte_dt(&config->bus, REG_CONFIG, 0U);
	data->flashing = false;
	k_mutex_unlock(&data->lock);
}

static int adp1653_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	const struct adp1653_config *config = dev->config;
	struct adp1653_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (led) {
	case LED_TORCH:
		data->hpled = (uint8_t)DIV_ROUND_UP(value * config->torch_max, LED_BRIGHTNESS_MAX);
		ret = data->flashing ? -EBUSY : write_out_sel(dev);
		break;
	case LED_INDICATOR:
		data->iled = (uint8_t)DIV_ROUND_UP(value * config->iled_max, LED_BRIGHTNESS_MAX);
		ret = data->flashing ? -EBUSY : write_out_sel(dev);
		break;
	case LED_FLASH:
		if (value == 0U) {
			ret = 0;
		} else if (data->flashing) {
			ret = -EBUSY;
		} else {
			ret = adp1653_flash(dev);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(led, adp1653_api) = {
	.set_brightness = adp1653_set_brightness,
};

static int adp1653_init(const struct device *dev)
{
	const struct adp1653_config *config = dev->config;
	struct adp1653_data *data = dev->data;
	uint8_t fault;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->lock);
	k_work_init_delayable(&data->flash_end, adp1653_flash_end);

	if (!i2c_is_ready_dt(&config->bus)) {
		LOG_ERR_DEVICE_NOT_READY(config->bus.bus);
		return -ENODEV;
	}

	if (config->enable_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->enable_gpio)) {
			LOG_ERR_DEVICE_NOT_READY(config->enable_gpio.port);
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->enable_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			return ret;
		}

		k_msleep(1);
	}

	/* Both LEDs off, then read the faults to clear them */
	ret = write_out_sel(dev);
	if (ret != 0) {
		LOG_ERR("not responding (%d)", ret);
		return ret;
	}

	ret = i2c_reg_read_byte_dt(&config->bus, REG_FAULT, &fault);
	if ((ret == 0) && (fault != 0U)) {
		LOG_WRN("faults 0x%02x cleared", fault);
	}

	return i2c_reg_write_byte_dt(&config->bus, REG_CONFIG, 0U);
}

#define ADP1653_INIT(inst)                                                                         \
	BUILD_ASSERT(DT_INST_PROP(inst, flash_timeout_us) <= TIMER_MAX_US,                         \
		     "flash-timeout-us above 820 ms");                                             \
	BUILD_ASSERT(DT_INST_PROP(inst, torch_max_microamp) >= HPLED_BASE_UA + HPLED_STEP_UA,      \
		     "torch-max-microamp below 50 mA");                                            \
	BUILD_ASSERT(DT_INST_PROP(inst, flash_max_microamp) >= HPLED_BASE_UA + HPLED_STEP_UA,      \
		     "flash-max-microamp below 50 mA");                                            \
	static const struct adp1653_config adp1653_config_##inst = {                               \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.enable_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, enable_gpios, {0}),                  \
		.torch_max = HPLED_CODE(DT_INST_PROP(inst, torch_max_microamp), HPLED_TORCH_MAX),  \
		.flash_code = HPLED_CODE(DT_INST_PROP(inst, flash_max_microamp), HPLED_FLASH_MAX), \
		.timer_code =                                                                      \
			(TIMER_MAX_US - DT_INST_PROP(inst, flash_timeout_us)) / TIMER_STEP_US,     \
		.iled_max = MIN(DT_INST_PROP(inst, indicator_max_microamp) / ILED_STEP_UA,         \
				OUT_SEL_ILED_MASK),                                                \
		.timeout_us = DT_INST_PROP(inst, flash_timeout_us),                                \
	};                                                                                         \
	static struct adp1653_data adp1653_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, adp1653_init, NULL, &adp1653_data_##inst,                      \
			      &adp1653_config_##inst, POST_KERNEL, CONFIG_LED_INIT_PRIORITY,       \
			      &adp1653_api);

DT_INST_FOREACH_STATUS_OKAY(ADP1653_INIT)
