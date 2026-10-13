/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Power button of the TI TWL4030/TWL5030 power management chips, reported
 * as INPUT_KEY_POWER. The button state is read from the chip's hardware
 * conditions on each power button interrupt the parent dispatches, or
 * polled when the parent has no interrupt line.
 */

#define DT_DRV_COMPAT ti_twl4030_pwrbutton

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#define POLL_PERIOD K_MSEC(50)

struct twl4030_pwrbutton_config {
	const struct device *mfd;
};

struct twl4030_pwrbutton_data {
	const struct device *dev;
	struct k_work_delayable poll_work;
	struct twl4030_irq_callback irq_cb;
	bool pressed;
};

static void update(const struct device *dev)
{
	const struct twl4030_pwrbutton_config *config = dev->config;
	struct twl4030_pwrbutton_data *data = dev->data;
	uint8_t sts;
	bool pressed;

	if (mfd_twl4030_reg_read(config->mfd, TWL4030_ADDR_PM_MASTER, TWL4030_STS_HW_CONDITIONS,
				 &sts) != 0) {
		return;
	}

	pressed = (sts & TWL4030_STS_PWON) != 0U;
	if (pressed != data->pressed) {
		data->pressed = pressed;
		input_report_key(dev, INPUT_KEY_POWER, pressed, true, K_FOREVER);
	}
}

static void poll_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct twl4030_pwrbutton_data *data = CONTAINER_OF(dwork, struct twl4030_pwrbutton_data,
							   poll_work);

	update(data->dev);
	k_work_reschedule(dwork, POLL_PERIOD);
}

static void irq_handler(const struct device *mfd, struct twl4030_irq_callback *cb, uint8_t status)
{
	struct twl4030_pwrbutton_data *data = CONTAINER_OF(cb, struct twl4030_pwrbutton_data,
							   irq_cb);

	ARG_UNUSED(mfd);
	ARG_UNUSED(status);

	update(data->dev);
}

static int twl4030_pwrbutton_init(const struct device *dev)
{
	const struct twl4030_pwrbutton_config *config = dev->config;
	struct twl4030_pwrbutton_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->mfd)) {
		return -ENODEV;
	}

	data->dev = dev;
	k_work_init_delayable(&data->poll_work, poll_handler);
	data->irq_cb.handler = irq_handler;
	data->irq_cb.module = TWL4030_IRQ_POWER;
	data->irq_cb.mask = TWL4030_POWER_IRQ_PWRON;

	ret = mfd_twl4030_add_irq_callback(config->mfd, &data->irq_cb);
	if (ret == -ENOTSUP) {
		k_work_schedule(&data->poll_work, POLL_PERIOD);
		return 0;
	}

	return ret;
}

#define TWL4030_PWRBUTTON_INIT(inst)                                                               \
	static const struct twl4030_pwrbutton_config twl4030_pwrbutton_config_##inst = {           \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
	static struct twl4030_pwrbutton_data twl4030_pwrbutton_data_##inst;                        \
	DEVICE_DT_INST_DEFINE(inst, twl4030_pwrbutton_init, NULL, &twl4030_pwrbutton_data_##inst,  \
			      &twl4030_pwrbutton_config_##inst, POST_KERNEL,                       \
			      CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(TWL4030_PWRBUTTON_INIT)
