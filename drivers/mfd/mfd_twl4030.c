/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI TWL4030/TWL5030 power management chip. The chip answers at four
 * consecutive I2C addresses and raises the interrupts of all its modules on
 * one line. This driver masks every module at init, owns that line, and
 * dispatches the clear-on-read status of the keypad and power modules to
 * the callbacks registered for them.
 */

#define DT_DRV_COMPAT ti_twl4030

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mfd_twl4030, CONFIG_MFD_LOG_LEVEL);

#define ADDR_COUNT 4U

/* Primary interrupt status: one bit per module */
#define PIH_ADDR_IDX 1U
#define PIH_ISR_P1   0x81U

#define SIH_CTRL_COR BIT(2)

/* Line activations without anything to clear before the line is left masked */
#define MAX_SPURIOUS 10U

/* Secondary interrupt handler registers of a module, for INT1 */
struct sih {
	uint8_t addr_idx;
	uint8_t isr;
	uint8_t imr;
	uint8_t edr;
	uint8_t ctrl;
};

/* Modules whose interrupts can be dispatched: one byte of clear-on-read status */
static const struct sih sih_keypad = {2U, 0xE3U, 0xE4U, 0xE8U, 0xE9U};
static const struct sih sih_power = {3U, 0x2EU, 0x2FU, 0x33U, 0x35U};

/* Interrupt masks of all modules on both lines, and the USB transceiver enables */
static const uint8_t mask_all[][3] = {
	/* GPIO: IMR1A, IMR1B */
	{1U, 0xB4U, 0xFFU}, {1U, 0xB5U, 0xFFU}, {1U, 0xB6U, 0x03U},
	{1U, 0xBAU, 0xFFU}, {1U, 0xBBU, 0xFFU}, {1U, 0xBCU, 0x03U},
	/* Keypad: IMR1, IMR2 */
	{2U, 0xE4U, 0x0FU}, {2U, 0xE6U, 0x0FU},
	/* MADC: IMR1, IMR2 */
	{2U, 0x62U, 0x0FU}, {2U, 0x64U, 0x0FU},
	/* BCI: IMR1A, IMR2A, IMR1B, IMR2B */
	{2U, 0xBBU, 0xFFU}, {2U, 0xBCU, 0xFFU}, {2U, 0xBFU, 0xFFU}, {2U, 0xC0U, 0xFFU},
	/* Power: IMR1, IMR2 */
	{3U, 0x2FU, 0xFFU}, {3U, 0x31U, 0xFFU},
	/* USB transceiver: clear the rising and falling edge enables */
	{0U, 0x0FU, 0x1FU}, {0U, 0x12U, 0x1FU},
	/* Keypad and power status clear on read */
	{2U, 0xE9U, SIH_CTRL_COR}, {3U, 0x35U, SIH_CTRL_COR},
};

struct mfd_twl4030_config {
	struct i2c_dt_spec i2c;
	void (*irq_config)(void);
	int irq;
};

struct mfd_twl4030_data {
	const struct device *dev;
	struct k_work irq_work;
	struct k_mutex lock;
	sys_slist_t callbacks;
	uint8_t spurious;
};

static uint16_t addr(const struct device *dev, uint8_t addr_idx)
{
	const struct mfd_twl4030_config *config = dev->config;

	return config->i2c.addr + addr_idx;
}

int mfd_twl4030_reg_read(const struct device *dev, uint8_t addr_idx, uint8_t reg, uint8_t *val)
{
	const struct mfd_twl4030_config *config = dev->config;

	if (addr_idx >= ADDR_COUNT) {
		return -EINVAL;
	}

	return i2c_reg_read_byte(config->i2c.bus, addr(dev, addr_idx), reg, val);
}

int mfd_twl4030_reg_read_burst(const struct device *dev, uint8_t addr_idx, uint8_t reg, void *buf,
			       size_t len)
{
	const struct mfd_twl4030_config *config = dev->config;

	if (addr_idx >= ADDR_COUNT) {
		return -EINVAL;
	}

	return i2c_burst_read(config->i2c.bus, addr(dev, addr_idx), reg, buf, len);
}

int mfd_twl4030_reg_write(const struct device *dev, uint8_t addr_idx, uint8_t reg, uint8_t val)
{
	const struct mfd_twl4030_config *config = dev->config;

	if (addr_idx >= ADDR_COUNT) {
		return -EINVAL;
	}

	return i2c_reg_write_byte(config->i2c.bus, addr(dev, addr_idx), reg, val);
}

static const struct sih *module_sih(enum twl4030_irq_module module)
{
	switch (module) {
	case TWL4030_IRQ_KEYPAD:
		return &sih_keypad;
	case TWL4030_IRQ_POWER:
		return &sih_power;
	default:
		return NULL;
	}
}

/* Enable both edges and unmask the interrupts in mask */
static int sih_enable(const struct device *dev, const struct sih *sih, uint8_t mask)
{
	uint8_t edr[2];
	uint8_t imr;
	int ret;

	ret = mfd_twl4030_reg_read_burst(dev, sih->addr_idx, sih->edr, edr, sizeof(edr));
	if (ret == 0) {
		ret = mfd_twl4030_reg_read(dev, sih->addr_idx, sih->imr, &imr);
	}
	if (ret != 0) {
		return ret;
	}

	/* Two bits per interrupt, four interrupts per register: falling, rising */
	for (uint8_t bit = 0U; bit < 8U; bit++) {
		if ((mask & BIT(bit)) != 0U) {
			edr[bit / 4U] |= BIT(2U * (bit % 4U)) | BIT((2U * (bit % 4U)) + 1U);
		}
	}

	ret = mfd_twl4030_reg_write(dev, sih->addr_idx, sih->edr, edr[0]);
	if (ret == 0) {
		ret = mfd_twl4030_reg_write(dev, sih->addr_idx, sih->edr + 1U, edr[1]);
	}
	if (ret == 0) {
		ret = mfd_twl4030_reg_write(dev, sih->addr_idx, sih->imr, imr & ~mask);
	}

	return ret;
}

int mfd_twl4030_add_irq_callback(const struct device *dev, struct twl4030_irq_callback *cb)
{
	const struct mfd_twl4030_config *config = dev->config;
	struct mfd_twl4030_data *data = dev->data;
	const struct sih *sih = module_sih(cb->module);
	int ret;

	if ((config->irq_config == NULL) || (sih == NULL)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	sys_slist_append(&data->callbacks, &cb->node);
	ret = sih_enable(dev, sih, cb->mask);
	k_mutex_unlock(&data->lock);

	return ret;
}

static void dispatch(const struct device *dev, enum twl4030_irq_module module)
{
	struct mfd_twl4030_data *data = dev->data;
	const struct sih *sih = module_sih(module);
	struct twl4030_irq_callback *cb;
	uint8_t status;

	/* Clear on read */
	if (mfd_twl4030_reg_read(dev, sih->addr_idx, sih->isr, &status) != 0) {
		return;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&data->callbacks, cb, node) {
		if ((cb->module == module) && ((cb->mask & status) != 0U)) {
			cb->handler(dev, cb, cb->mask & status);
		}
	}
}

static void irq_work_handler(struct k_work *work)
{
	struct mfd_twl4030_data *data = CONTAINER_OF(work, struct mfd_twl4030_data, irq_work);
	const struct device *dev = data->dev;
	const struct mfd_twl4030_config *config = dev->config;
	const uint8_t handled = BIT(TWL4030_IRQ_KEYPAD) | BIT(TWL4030_IRQ_POWER);
	uint8_t pih = 0U;

	(void)mfd_twl4030_reg_read(dev, PIH_ADDR_IDX, PIH_ISR_P1, &pih);

	k_mutex_lock(&data->lock, K_FOREVER);
	if ((pih & BIT(TWL4030_IRQ_KEYPAD)) != 0U) {
		dispatch(dev, TWL4030_IRQ_KEYPAD);
	}
	if ((pih & BIT(TWL4030_IRQ_POWER)) != 0U) {
		dispatch(dev, TWL4030_IRQ_POWER);
	}
	k_mutex_unlock(&data->lock);

	if (((pih & handled) == 0U) && (++data->spurious >= MAX_SPURIOUS)) {
		/* Something keeps the line asserted that no callback clears */
		LOG_ERR("interrupt line stuck, primary status 0x%02x: line disabled", pih);
		return;
	}
	if ((pih & handled) != 0U) {
		data->spurious = 0U;
	}

	irq_enable(config->irq);
}

static void __maybe_unused mfd_twl4030_isr(const void *arg)
{
	const struct device *dev = arg;
	const struct mfd_twl4030_config *config = dev->config;
	struct mfd_twl4030_data *data = dev->data;

	/* The line stays asserted until the module status is read over I2C */
	irq_disable(config->irq);
	k_work_submit(&data->irq_work);
}

static int mfd_twl4030_init(const struct device *dev)
{
	const struct mfd_twl4030_config *config = dev->config;
	struct mfd_twl4030_data *data = dev->data;
	uint8_t status;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	data->dev = dev;
	k_mutex_init(&data->lock);
	sys_slist_init(&data->callbacks);
	k_work_init(&data->irq_work, irq_work_handler);

	for (size_t i = 0U; i < ARRAY_SIZE(mask_all); i++) {
		ret = mfd_twl4030_reg_write(dev, mask_all[i][0], mask_all[i][1], mask_all[i][2]);
		if (ret != 0) {
			LOG_ERR("interrupt setup failed (%d)", ret);
			return ret;
		}
	}

	/* Discard status latched before the modules were masked */
	(void)mfd_twl4030_reg_read(dev, sih_keypad.addr_idx, sih_keypad.isr, &status);
	(void)mfd_twl4030_reg_read(dev, sih_power.addr_idx, sih_power.isr, &status);

	if (config->irq_config != NULL) {
		config->irq_config();
		irq_enable(config->irq);
	}

	return 0;
}

#define MFD_TWL4030_IRQ_CONFIG(inst)                                                               \
	static void mfd_twl4030_irq_config_##inst(void)                                            \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), mfd_twl4030_isr,      \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
	}

#define MFD_TWL4030_DEFINE(inst)                                                                   \
	IF_ENABLED(DT_INST_IRQ_HAS_IDX(inst, 0), (MFD_TWL4030_IRQ_CONFIG(inst)))                   \
	static const struct mfd_twl4030_config mfd_twl4030_config_##inst = {                       \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.irq_config = COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0),                           \
					  (mfd_twl4030_irq_config_##inst), (NULL)),                \
		.irq = COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0), (DT_INST_IRQN(inst)), (-1)),      \
	};                                                                                         \
	static struct mfd_twl4030_data mfd_twl4030_data_##inst;                                    \
	DEVICE_DT_INST_DEFINE(inst, mfd_twl4030_init, NULL, &mfd_twl4030_data_##inst,              \
			      &mfd_twl4030_config_##inst, POST_KERNEL, CONFIG_MFD_INIT_PRIORITY,   \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_TWL4030_DEFINE)
