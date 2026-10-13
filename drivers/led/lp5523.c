/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI (National Semiconductor) LP5523 9-channel LED driver on I2C. The PWM
 * registers set the brightness directly; the program engines are not used.
 */

#define DT_DRV_COMPAT national_lp5523

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lp5523, CONFIG_LED_LOG_LEVEL);

#define LP5523_NUM_LEDS 9U

#define REG_ENABLE          0x00U
#define REG_OP_MODE         0x01U
#define REG_ENABLE_LEDS_MSB 0x04U
#define REG_ENABLE_LEDS_LSB 0x05U
#define REG_LED_PWM_BASE    0x16U
#define REG_LED_CURRENT     0x26U
#define REG_CONFIG          0x36U

#define ENABLE_CHIP_EN BIT(6)

#define CFG_AUTO_INC     BIT(6)
#define CFG_PWR_SAVE     BIT(5)
#define CFG_CP_AUTO      (BIT(4) | BIT(3))
#define CFG_PWM_PWR_SAVE BIT(2)
#define CFG_CLK_DET_EN   BIT(1)

/* LED current registers count in 100 uA steps */
#define CURRENT_STEP_UA 100U

struct lp5523_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec enable_gpio;
	uint8_t current;
};

static int lp5523_write_channels(const struct device *dev, uint32_t start_channel,
				 uint32_t num_channels, const uint8_t *buf)
{
	const struct lp5523_config *config = dev->config;
	uint8_t msg[LP5523_NUM_LEDS + 1U];

	if (((uint64_t)start_channel + num_channels) > LP5523_NUM_LEDS) {
		return -EINVAL;
	}

	msg[0] = REG_LED_PWM_BASE + start_channel;
	memcpy(&msg[1], buf, num_channels);

	return i2c_write_dt(&config->bus, msg, num_channels + 1U);
}

static int lp5523_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	uint8_t pwm;

	if (led >= LP5523_NUM_LEDS) {
		return -EINVAL;
	}

	pwm = (uint8_t)((value * 255U) / LED_BRIGHTNESS_MAX);

	return lp5523_write_channels(dev, led, 1U, &pwm);
}

static DEVICE_API(led, lp5523_api) = {
	.set_brightness = lp5523_set_brightness,
	.write_channels = lp5523_write_channels,
};

static int lp5523_init(const struct device *dev)
{
	const struct lp5523_config *config = dev->config;
	uint8_t current[LP5523_NUM_LEDS + 1U];
	uint8_t off[LP5523_NUM_LEDS] = {0};
	/*
	 * The reset register is not used: the chip disturbs the bus as it
	 * resets. Disabling the program engines and setting every register the
	 * driver relies on gives the same state.
	 */
	const uint8_t setup[][2] = {
		{REG_ENABLE, ENABLE_CHIP_EN},
		{REG_OP_MODE, 0x00U},
		{REG_CONFIG,
		 CFG_AUTO_INC | CFG_PWR_SAVE | CFG_CP_AUTO | CFG_PWM_PWR_SAVE | CFG_CLK_DET_EN},
		{REG_ENABLE_LEDS_MSB, 0x01U},
		{REG_ENABLE_LEDS_LSB, 0xFFU},
	};
	int ret;

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

		/* Start-up time after the enable pin goes high */
		k_msleep(1);
	}

	for (size_t i = 0U; i < ARRAY_SIZE(setup); i++) {
		ret = i2c_reg_write_byte_dt(&config->bus, setup[i][0], setup[i][1]);
		if (ret != 0) {
			LOG_ERR("setup failed (%d)", ret);
			return ret;
		}

		if (setup[i][0] == REG_ENABLE) {
			/* The chip leaves standby within 500 us */
			k_msleep(1);
		}
	}

	current[0] = REG_LED_CURRENT;
	memset(&current[1], config->current, LP5523_NUM_LEDS);

	ret = i2c_write_dt(&config->bus, current, sizeof(current));
	if (ret != 0) {
		return ret;
	}

	return lp5523_write_channels(dev, 0U, LP5523_NUM_LEDS, off);
}

#define LP5523_INIT(inst)                                                                          \
	BUILD_ASSERT(DT_INST_PROP(inst, current_microamp) <= (255 * CURRENT_STEP_UA),              \
		     "current-microamp above the 25.5 mA maximum");                                \
	static const struct lp5523_config lp5523_config_##inst = {                                 \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.enable_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, enable_gpios, {0}),                  \
		.current = DT_INST_PROP(inst, current_microamp) / CURRENT_STEP_UA,                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, lp5523_init, NULL, NULL, &lp5523_config_##inst, POST_KERNEL,   \
			      CONFIG_LED_INIT_PRIORITY, &lp5523_api);

DT_INST_FOREACH_STATUS_OKAY(LP5523_INIT)
