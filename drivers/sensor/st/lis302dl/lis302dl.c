/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ST LIS302DL 3-axis accelerometer on I2C: 8-bit samples at 100 or 400 Hz
 * with a full scale of 2 or 8 g. With the interrupt lines in irq-gpios, INT1
 * signals data ready and INT2 a single click on any axis.
 */

#define DT_DRV_COMPAT st_lis302dl

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lis302dl, CONFIG_SENSOR_LOG_LEVEL);

#define REG_WHO_AM_I         0x0FU
#define REG_CTRL_REG1        0x20U
#define REG_CTRL_REG3        0x22U
#define REG_OUT_X            0x29U
#define REG_CLICK_CFG        0x38U
#define REG_CLICK_SRC        0x39U
#define REG_CLICK_THSY_X     0x3BU
#define REG_CLICK_THSZ       0x3CU
#define REG_CLICK_TIME_LIMIT 0x3DU

/* Register address bit that increments the address on multi-byte reads */
#define REG_AUTO_INC BIT(7)

#define WHO_AM_I_VALUE 0x3BU

#define CTRL_REG1_DR  BIT(7)
#define CTRL_REG1_PD  BIT(6)
#define CTRL_REG1_FS  BIT(5)
#define CTRL_REG1_XYZ GENMASK(2, 0)

/* Interrupt pin sources in CTRL_REG3 */
#define CTRL_REG3_I1_DATA_READY 0x04U
#define CTRL_REG3_I2_CLICK      (0x07U << 3)

/* Single click on Z, Y and X, latched until CLICK_SRC is read */
#define CLICK_CFG_LIR    BIT(6)
#define CLICK_CFG_SINGLE (BIT(4) | BIT(2) | BIT(0))

/* Longest click, in 0.5 ms steps */
#define CLICK_TIME_LIMIT 16U

/* Sensitivity in micro-g per digit, typical */
#define SENSITIVITY_2G_UG 18000
#define SENSITIVITY_8G_UG 72000

struct lis302dl_config {
	struct i2c_dt_spec i2c;
	uint8_t full_scale;
	uint16_t odr;
#ifdef CONFIG_LIS302DL_TRIGGER
	struct gpio_dt_spec int1;
	struct gpio_dt_spec int2;
	uint8_t click_threshold;
#endif
};

struct lis302dl_data {
	int8_t sample[3];
#ifdef CONFIG_LIS302DL_TRIGGER
	const struct device *dev;
	struct gpio_callback int1_cb;
	struct gpio_callback int2_cb;
	struct k_work drdy_work;
	struct k_work tap_work;
	sensor_trigger_handler_t drdy_handler;
	const struct sensor_trigger *drdy_trig;
	sensor_trigger_handler_t tap_handler;
	const struct sensor_trigger *tap_trig;
	uint8_t ctrl_reg3;
#endif
};

static int lis302dl_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct lis302dl_config *config = dev->config;
	struct lis302dl_data *data = dev->data;
	/* OUT_X, OUT_Y and OUT_Z are every other register */
	uint8_t buf[5];
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_ACCEL_XYZ)) {
		return -ENOTSUP;
	}

	ret = i2c_burst_read_dt(&config->i2c, REG_OUT_X | REG_AUTO_INC, buf, sizeof(buf));
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(data->sample); i++) {
		data->sample[i] = (int8_t)buf[i * 2U];
	}

	return 0;
}

static void lis302dl_convert(const struct device *dev, int8_t raw, struct sensor_value *val)
{
	const struct lis302dl_config *config = dev->config;
	int64_t ug =
		(int64_t)raw * ((config->full_scale == 8U) ? SENSITIVITY_8G_UG : SENSITIVITY_2G_UG);

	/* Micro-g to micro-m/s^2 */
	sensor_value_from_micro(val, (ug * SENSOR_G) / 1000000LL);
}

static int lis302dl_channel_get(const struct device *dev, enum sensor_channel chan,
				struct sensor_value *val)
{
	struct lis302dl_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
		lis302dl_convert(dev, data->sample[chan - SENSOR_CHAN_ACCEL_X], val);
		break;
	case SENSOR_CHAN_ACCEL_XYZ:
		for (size_t i = 0U; i < ARRAY_SIZE(data->sample); i++) {
			lis302dl_convert(dev, data->sample[i], &val[i]);
		}
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

#ifdef CONFIG_LIS302DL_TRIGGER

static void lis302dl_drdy_work(struct k_work *work)
{
	struct lis302dl_data *data = CONTAINER_OF(work, struct lis302dl_data, drdy_work);
	const struct lis302dl_config *config = data->dev->config;

	if (data->drdy_handler == NULL) {
		return;
	}

	data->drdy_handler(data->dev, data->drdy_trig);

	/*
	 * Data ready stays active until the samples are read: if the next set
	 * arrived before the handler read this one, no new edge follows.
	 */
	if (gpio_pin_get_dt(&config->int1) == 1) {
		k_work_submit(&data->drdy_work);
	}
}

static void lis302dl_tap_work(struct k_work *work)
{
	struct lis302dl_data *data = CONTAINER_OF(work, struct lis302dl_data, tap_work);
	const struct lis302dl_config *config = data->dev->config;
	uint8_t src;

	/* Reading the source releases the latched interrupt */
	if (i2c_reg_read_byte_dt(&config->i2c, REG_CLICK_SRC, &src) != 0) {
		return;
	}

	if (data->tap_handler != NULL) {
		data->tap_handler(data->dev, data->tap_trig);
	}
}

static void lis302dl_int1(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct lis302dl_data *data = CONTAINER_OF(cb, struct lis302dl_data, int1_cb);

	k_work_submit(&data->drdy_work);
}

static void lis302dl_int2(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct lis302dl_data *data = CONTAINER_OF(cb, struct lis302dl_data, int2_cb);

	k_work_submit(&data->tap_work);
}

static int lis302dl_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
				sensor_trigger_handler_t handler)
{
	const struct lis302dl_config *config = dev->config;
	struct lis302dl_data *data = dev->data;
	const struct gpio_dt_spec *gpio;
	uint8_t source;
	int ret;

	switch (trig->type) {
	case SENSOR_TRIG_DATA_READY:
		gpio = &config->int1;
		source = CTRL_REG3_I1_DATA_READY;
		data->drdy_handler = handler;
		data->drdy_trig = trig;
		break;
	case SENSOR_TRIG_TAP:
		gpio = &config->int2;
		source = CTRL_REG3_I2_CLICK;
		data->tap_handler = handler;
		data->tap_trig = trig;
		break;
	default:
		return -ENOTSUP;
	}

	if (gpio->port == NULL) {
		return -ENOTSUP;
	}

	if (handler != NULL) {
		data->ctrl_reg3 |= source;
	} else {
		data->ctrl_reg3 &= ~source;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, REG_CTRL_REG3, data->ctrl_reg3);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(gpio, (handler != NULL) ? GPIO_INT_EDGE_TO_ACTIVE
								      : GPIO_INT_DISABLE);
	if (ret != 0) {
		return ret;
	}

	if ((handler != NULL) && (trig->type == SENSOR_TRIG_DATA_READY)) {
		/* Data ready stays active until the samples are read */
		uint8_t buf[5];

		(void)i2c_burst_read_dt(&config->i2c, REG_OUT_X | REG_AUTO_INC, buf, sizeof(buf));
	}

	return 0;
}

static int lis302dl_gpio_init(const struct gpio_dt_spec *gpio, struct gpio_callback *cb,
			      gpio_callback_handler_t handler)
{
	int ret;

	if (gpio->port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(gpio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}

	gpio_init_callback(cb, handler, BIT(gpio->pin));

	return gpio_add_callback_dt(gpio, cb);
}

static int lis302dl_trigger_init(const struct device *dev)
{
	const struct lis302dl_config *config = dev->config;
	struct lis302dl_data *data = dev->data;
	uint8_t thr = config->click_threshold;
	const uint8_t click[][2] = {
		{REG_CLICK_CFG, CLICK_CFG_LIR | CLICK_CFG_SINGLE},
		{REG_CLICK_THSY_X, (uint8_t)((thr << 4) | thr)},
		{REG_CLICK_THSZ, thr},
		{REG_CLICK_TIME_LIMIT, CLICK_TIME_LIMIT},
	};
	int ret;

	data->dev = dev;
	k_work_init(&data->drdy_work, lis302dl_drdy_work);
	k_work_init(&data->tap_work, lis302dl_tap_work);

	ret = lis302dl_gpio_init(&config->int1, &data->int1_cb, lis302dl_int1);
	if (ret != 0) {
		return ret;
	}

	ret = lis302dl_gpio_init(&config->int2, &data->int2_cb, lis302dl_int2);
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(click); i++) {
		ret = i2c_reg_write_byte_dt(&config->i2c, click[i][0], click[i][1]);
		if (ret != 0) {
			return ret;
		}
	}

	/* Interrupts active high, push-pull, both pins grounded until used */
	return i2c_reg_write_byte_dt(&config->i2c, REG_CTRL_REG3, 0U);
}

#endif /* CONFIG_LIS302DL_TRIGGER */

static DEVICE_API(sensor, lis302dl_api) = {
	.sample_fetch = lis302dl_sample_fetch,
	.channel_get = lis302dl_channel_get,
#ifdef CONFIG_LIS302DL_TRIGGER
	.trigger_set = lis302dl_trigger_set,
#endif
};

static int lis302dl_init(const struct device *dev)
{
	const struct lis302dl_config *config = dev->config;
	uint8_t ctrl = CTRL_REG1_PD | CTRL_REG1_XYZ;
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR_DEVICE_NOT_READY(config->i2c.bus);
		return -ENODEV;
	}

	ret = i2c_reg_read_byte_dt(&config->i2c, REG_WHO_AM_I, &id);
	if (ret != 0) {
		LOG_ERR("WHO_AM_I read failed (%d)", ret);
		return ret;
	}

	if (id != WHO_AM_I_VALUE) {
		LOG_ERR("unexpected WHO_AM_I 0x%02x", id);
		return -ENODEV;
	}

	if (config->full_scale == 8U) {
		ctrl |= CTRL_REG1_FS;
	}

	if (config->odr == 400U) {
		ctrl |= CTRL_REG1_DR;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, REG_CTRL_REG1, ctrl);
	if (ret != 0) {
		return ret;
	}

	/* The first samples are ready three output periods after power-up */
	k_msleep(DIV_ROUND_UP(3U * MSEC_PER_SEC, config->odr));

#ifdef CONFIG_LIS302DL_TRIGGER
	ret = lis302dl_trigger_init(dev);
	if (ret != 0) {
		LOG_ERR("interrupt setup failed (%d)", ret);
		return ret;
	}
#endif

	return 0;
}

#define LIS302DL_INIT(inst)                                                                        \
	static const struct lis302dl_config lis302dl_config_##inst = {                             \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.full_scale = DT_INST_PROP(inst, full_scale),                                      \
		.odr = DT_INST_PROP(inst, odr),                                                    \
		IF_ENABLED(CONFIG_LIS302DL_TRIGGER,                                                \
			   (.int1 = GPIO_DT_SPEC_INST_GET_BY_IDX_OR(inst, irq_gpios, 0, {0}),      \
			    .int2 = GPIO_DT_SPEC_INST_GET_BY_IDX_OR(inst, irq_gpios, 1, {0}),      \
			    .click_threshold = DT_INST_PROP(inst, click_threshold),))             \
	};                                                                                         \
	static struct lis302dl_data lis302dl_data_##inst;                                          \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, lis302dl_init, NULL, &lis302dl_data_##inst,             \
				     &lis302dl_config_##inst, POST_KERNEL,                         \
				     CONFIG_SENSOR_INIT_PRIORITY, &lis302dl_api);

DT_INST_FOREACH_STATUS_OKAY(LIS302DL_INIT)
