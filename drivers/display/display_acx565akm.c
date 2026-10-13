/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Command interface of the Sony ACX565AKM LCD panel controller, a 3-wire SPI
 * port taking 9-bit words whose first bit tells data from commands. The
 * pixels reach the panel over a separate video link from the display
 * controller, which forwards brightness changes to this device.
 */

#define DT_DRV_COMPAT sony_acx565akm

#include <zephyr/device.h>
#include <zephyr/display/mipi_display.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(acx565akm, CONFIG_DISPLAY_LOG_LEVEL);

/* The 9th bit of a word marks it as data */
#define WORD_DATA BIT(8)

#define CTRL_DISP_BRIGHTNESS_CTRL_ON BIT(5)
#define CTRL_DISP_BACKLIGHT_ON       BIT(2)

#define MAX_PARAMS 1U

struct acx565akm_config {
	struct spi_dt_spec bus;
};

/* Sends a command and its parameters with the chip select held */
static int acx565akm_write(const struct device *dev, uint8_t cmd, const uint8_t *params, size_t len)
{
	const struct acx565akm_config *config = dev->config;
	uint16_t words[1U + MAX_PARAMS];
	const struct spi_buf buf = {.buf = words, .len = (1U + len) * sizeof(words[0])};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};

	__ASSERT_NO_MSG(len <= MAX_PARAMS);

	words[0] = cmd;
	for (size_t i = 0U; i < len; i++) {
		words[1U + i] = WORD_DATA | params[i];
	}

	return spi_write_dt(&config->bus, &tx);
}

static int acx565akm_set_brightness(const struct device *dev, const uint8_t brightness)
{
	uint8_t ctrl = 0U;
	int ret;

	ret = acx565akm_write(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, &brightness, 1U);
	if (ret != 0) {
		return ret;
	}

	if (brightness != 0U) {
		ctrl = CTRL_DISP_BRIGHTNESS_CTRL_ON | CTRL_DISP_BACKLIGHT_ON;
	}

	return acx565akm_write(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, &ctrl, 1U);
}

static int acx565akm_blanking_on(const struct device *dev)
{
	return acx565akm_write(dev, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0U);
}

static int acx565akm_blanking_off(const struct device *dev)
{
	return acx565akm_write(dev, MIPI_DCS_SET_DISPLAY_ON, NULL, 0U);
}

static DEVICE_API(display, acx565akm_api) = {
	.blanking_on = acx565akm_blanking_on,
	.blanking_off = acx565akm_blanking_off,
	.set_brightness = acx565akm_set_brightness,
};

static int acx565akm_init(const struct device *dev)
{
	const struct acx565akm_config *config = dev->config;

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR_DEVICE_NOT_READY(config->bus.bus);
		return -ENODEV;
	}

	return 0;
}

#define ACX565AKM_INIT(inst)                                                                       \
	static const struct acx565akm_config acx565akm_config_##inst = {                           \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_CONTROLLER | SPI_TRANSFER_MSB |      \
							  SPI_WORD_SET(9)),                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, acx565akm_init, NULL, NULL, &acx565akm_config_##inst,          \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &acx565akm_api);

DT_INST_FOREACH_STATUS_OKAY(ACX565AKM_INIT)
