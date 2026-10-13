/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Keypad module of the TI TWL4030/TWL5030 power management chips. The chip
 * scans the matrix and debounces it; the driver reads the decoded matrix
 * state and reports row and column events, which an input-keymap child
 * turns into key codes.
 *
 * The matrix is read on each key change interrupt the parent dispatches, or
 * polled when the parent has no interrupt line.
 */

#define DT_DRV_COMPAT ti_twl4030_keypad

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(twl4030_keypad, CONFIG_INPUT_LOG_LEVEL);

/* Keypad module registers, at the chip's third I2C address */
#define KEYP_ADDR_IDX  2U
#define KEYP_BASE      0xD2U
#define KEYP_CTRL      (KEYP_BASE + 0x00U)
#define KEYP_DEB       (KEYP_BASE + 0x01U)
#define KEYP_LK_PTV    (KEYP_BASE + 0x03U)
#define KEYP_TIMEOUT_L (KEYP_BASE + 0x04U)
#define KEYP_TIMEOUT_H (KEYP_BASE + 0x05U)
#define KEYP_FULL_CODE (KEYP_BASE + 0x09U)

#define CTRL_SOFT_NRST BIT(0)
#define CTRL_SOFTMODEN BIT(1)
#define CTRL_TOE_EN    BIT(3)
#define CTRL_KBD_ON    BIT(6)

/* Keypad timer: 32 kHz clock divided by 2^(PTV + 1), in periods of 31 cycles */
#define PTV_PRESCALER 4U
#define LK_PTV_SHIFT  5U
#define PERIOD(us)    (((us) / (31U << (PTV_PRESCALER + 1U))) - 1U)
#define DEBOUNCE_US   20000U
#define TIMEOUT_US    200000U

#define MAX_ROWS 8U

struct twl4030_keypad_config {
	const struct device *mfd;
	uint8_t rows;
	uint8_t cols;
	k_timeout_t poll_period;
};

struct twl4030_keypad_data {
	const struct device *dev;
	struct k_work_delayable poll_work;
	struct twl4030_irq_callback irq_cb;
	uint16_t state[MAX_ROWS];
};

/* A row that reads all ones is tied to ground: report it as one extra column */
static uint16_t row_state(const struct twl4030_keypad_config *config, uint8_t raw)
{
	if (raw == 0xFFU) {
		return BIT(config->cols);
	}

	return raw & BIT_MASK(config->cols);
}

/* Several keys in a row sharing a column with another row cannot be decoded */
static bool is_ghosted(const struct twl4030_keypad_config *config, const uint16_t *state)
{
	uint16_t seen = 0U;

	for (uint8_t row = 0U; row < config->rows; row++) {
		if ((POPCOUNT(state[row]) > 1) && ((state[row] & seen) != 0U)) {
			return true;
		}
		seen |= state[row];
	}

	return false;
}

static int read_matrix(const struct device *dev, uint16_t *state)
{
	const struct twl4030_keypad_config *config = dev->config;
	uint8_t raw[MAX_ROWS];
	int ret;

	ret = mfd_twl4030_reg_read_burst(config->mfd, KEYP_ADDR_IDX, KEYP_FULL_CODE, raw,
					 config->rows);
	if (ret != 0) {
		return ret;
	}

	for (uint8_t row = 0U; row < config->rows; row++) {
		state[row] = row_state(config, raw[row]);
	}

	return 0;
}

static void scan(const struct device *dev)
{
	const struct twl4030_keypad_config *config = dev->config;
	struct twl4030_keypad_data *data = dev->data;
	uint16_t state[MAX_ROWS];

	if ((read_matrix(dev, state) != 0) || is_ghosted(config, state)) {
		return;
	}

	for (uint8_t row = 0U; row < config->rows; row++) {
		uint16_t changed = state[row] ^ data->state[row];

		while (changed != 0U) {
			uint8_t col = u32_count_trailing_zeros(changed);
			bool pressed = (state[row] & BIT(col)) != 0U;

			input_report_abs(dev, INPUT_ABS_X, col, false, K_FOREVER);
			input_report_abs(dev, INPUT_ABS_Y, row, false, K_FOREVER);
			input_report_key(dev, INPUT_BTN_TOUCH, pressed, true, K_FOREVER);
			changed &= ~BIT(col);
		}
		data->state[row] = state[row];
	}
}

static void poll_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct twl4030_keypad_data *data = CONTAINER_OF(dwork, struct twl4030_keypad_data,
							poll_work);
	const struct twl4030_keypad_config *config = data->dev->config;

	scan(data->dev);
	k_work_reschedule(dwork, config->poll_period);
}

static void irq_handler(const struct device *mfd, struct twl4030_irq_callback *cb, uint8_t status)
{
	struct twl4030_keypad_data *data = CONTAINER_OF(cb, struct twl4030_keypad_data, irq_cb);

	ARG_UNUSED(mfd);
	ARG_UNUSED(status);

	scan(data->dev);
}

static int twl4030_keypad_init(const struct device *dev)
{
	const struct twl4030_keypad_config *config = dev->config;
	struct twl4030_keypad_data *data = dev->data;
	const uint8_t setup[][2] = {
		/* Hardware decoding, timeout enabled, no autorepeat */
		{KEYP_CTRL, CTRL_SOFT_NRST | CTRL_SOFTMODEN | CTRL_TOE_EN | CTRL_KBD_ON},
		{KEYP_LK_PTV, PTV_PRESCALER << LK_PTV_SHIFT},
		{KEYP_DEB, PERIOD(DEBOUNCE_US)},
		{KEYP_TIMEOUT_L, PERIOD(TIMEOUT_US) & 0xFFU},
		{KEYP_TIMEOUT_H, PERIOD(TIMEOUT_US) >> 8},
	};
	int ret;

	if (!device_is_ready(config->mfd)) {
		return -ENODEV;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(setup); i++) {
		ret = mfd_twl4030_reg_write(config->mfd, KEYP_ADDR_IDX, setup[i][0], setup[i][1]);
		if (ret != 0) {
			LOG_ERR("keypad setup failed (%d)", ret);
			return ret;
		}
	}

	ret = read_matrix(dev, data->state);
	if (ret != 0) {
		return ret;
	}

	data->dev = dev;
	k_work_init_delayable(&data->poll_work, poll_handler);
	data->irq_cb.handler = irq_handler;
	data->irq_cb.module = TWL4030_IRQ_KEYPAD;
	data->irq_cb.mask = TWL4030_KEYPAD_IRQ_KP;

	ret = mfd_twl4030_add_irq_callback(config->mfd, &data->irq_cb);
	if (ret == -ENOTSUP) {
		LOG_INF("polled");
		k_work_schedule(&data->poll_work, config->poll_period);
		return 0;
	}
	if (ret == 0) {
		LOG_INF("interrupt-driven");
	}

	return ret;
}

#define TWL4030_KEYPAD_INIT(inst)                                                                  \
	BUILD_ASSERT(DT_INST_PROP(inst, row_size) <= MAX_ROWS, "too many rows");                   \
	BUILD_ASSERT(DT_INST_PROP(inst, col_size) <= 8, "too many columns");                       \
	static const struct twl4030_keypad_config twl4030_keypad_config_##inst = {                 \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.rows = DT_INST_PROP(inst, row_size),                                              \
		.cols = DT_INST_PROP(inst, col_size),                                              \
		.poll_period = K_MSEC(DT_INST_PROP(inst, poll_period_ms)),                         \
	};                                                                                         \
	static struct twl4030_keypad_data twl4030_keypad_data_##inst;                              \
	DEVICE_DT_INST_DEFINE(inst, twl4030_keypad_init, NULL, &twl4030_keypad_data_##inst,        \
			      &twl4030_keypad_config_##inst, POST_KERNEL,                          \
			      CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(TWL4030_KEYPAD_INIT)
