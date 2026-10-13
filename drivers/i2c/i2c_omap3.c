/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * I2C controller of TI OMAP3 SoCs: the register layout that precedes the
 * one supported by the ti,omap-i2c driver, with 16-bit registers. Transfers
 * are polled.
 */

#define DT_DRV_COMPAT ti_omap3_i2c

#include <errno.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "i2c-priv.h"

LOG_MODULE_REGISTER(i2c_omap3, CONFIG_I2C_LOG_LEVEL);

#define I2C_IE      0x04U
#define I2C_STAT    0x08U
#define I2C_SYSS    0x10U
#define I2C_BUF     0x14U
#define I2C_CNT     0x18U
#define I2C_DATA    0x1CU
#define I2C_SYSC    0x20U
#define I2C_CON     0x24U
#define I2C_OA      0x28U
#define I2C_SA      0x2CU
#define I2C_PSC     0x30U
#define I2C_SCLL    0x34U
#define I2C_SCLH    0x38U
#define I2C_SYSTEST 0x3CU

#define STAT_AL   BIT(0)
#define STAT_NACK BIT(1)
#define STAT_ARDY BIT(2)
#define STAT_RRDY BIT(3)
#define STAT_XRDY BIT(4)
#define STAT_BB   BIT(12)
#define STAT_ALL  0xFFFFU

#define SYSS_RDONE BIT(0)

#define BUF_TXFIFO_CLR BIT(6)
#define BUF_RXFIFO_CLR BIT(14)

#define SYSC_SOFTRESET BIT(1)

#define CON_STT BIT(0)
#define CON_STP BIT(1)
#define CON_TRX BIT(9)
#define CON_MST BIT(10)
#define CON_EN  BIT(15)

/*
 * The 96 MHz functional clock divided by (PSC + 1) gives a 12 MHz internal
 * clock; SCL low is (SCLL + 7) and high (SCLH + 5) internal clock cycles.
 */
/* Test register in loop back and IO mode drives and reads SCL and SDA */
#define SYSTEST_ST_EN    BIT(15)
#define SYSTEST_TMODE_IO (3U << 12)
#define SYSTEST_SCL_I    BIT(3)
#define SYSTEST_SCL_O    BIT(2)
#define SYSTEST_SDA_I    BIT(1)
#define SYSTEST_SDA_O    BIT(0)

/* Clock pulses that release a device holding SDA in the middle of a byte */
#define RECOVERY_PULSES  9
#define RECOVERY_HALF_US 5U

#define I2C_PSC_12MHZ 7U

#define I2C_TIMEOUT_MS 100

struct i2c_omap3_config {
	mm_reg_t base;
	uint32_t bitrate;
};

struct i2c_omap3_data {
	struct k_mutex lock;
	uint32_t dev_config;
	uint16_t scll;
	uint16_t sclh;
};

static uint16_t reg_read(const struct device *dev, uint32_t reg)
{
	const struct i2c_omap3_config *config = dev->config;

	return sys_read16(config->base + reg);
}

static void reg_write(const struct device *dev, uint32_t reg, uint16_t val)
{
	const struct i2c_omap3_config *config = dev->config;

	sys_write16(val, config->base + reg);
}

static uint8_t data_read(const struct device *dev)
{
	const struct i2c_omap3_config *config = dev->config;

	return sys_read8(config->base + I2C_DATA);
}

static void data_write(const struct device *dev, uint8_t val)
{
	const struct i2c_omap3_config *config = dev->config;

	sys_write8(val, config->base + I2C_DATA);
}

static int wait_bus_free(const struct device *dev)
{
	int64_t deadline = k_uptime_get() + I2C_TIMEOUT_MS;

	while ((reg_read(dev, I2C_STAT) & STAT_BB) != 0U) {
		if (k_uptime_get() > deadline) {
			return -EBUSY;
		}
	}

	return 0;
}

static void systest_write(const struct device *dev, uint16_t lines)
{
	reg_write(dev, I2C_SYSTEST, SYSTEST_ST_EN | SYSTEST_TMODE_IO | lines);
	k_busy_wait(RECOVERY_HALF_US);
}

static bool sda_is_low(const struct device *dev)
{
	return (reg_read(dev, I2C_SYSTEST) & SYSTEST_SDA_I) == 0U;
}

/*
 * Clocks SCL until a device stuck in the middle of a byte releases SDA, then
 * generates a stop condition. Called with the lock held.
 */
static int bus_recover(const struct device *dev)
{
	bool released;

	systest_write(dev, SYSTEST_SCL_O | SYSTEST_SDA_O);

	for (int i = 0; (i < RECOVERY_PULSES) && sda_is_low(dev); i++) {
		systest_write(dev, SYSTEST_SDA_O);
		systest_write(dev, SYSTEST_SCL_O | SYSTEST_SDA_O);
	}

	/* Stop: SDA rises while SCL is high */
	systest_write(dev, 0U);
	systest_write(dev, SYSTEST_SCL_O);
	systest_write(dev, SYSTEST_SCL_O | SYSTEST_SDA_O);

	released = !sda_is_low(dev);
	reg_write(dev, I2C_SYSTEST, 0U);

	if (!released) {
		LOG_ERR("SDA held low");
		return -EBUSY;
	}

	return 0;
}

static void controller_setup(const struct device *dev)
{
	struct i2c_omap3_data *data = dev->data;

	reg_write(dev, I2C_CON, 0U);
	reg_write(dev, I2C_PSC, I2C_PSC_12MHZ);
	reg_write(dev, I2C_SCLL, data->scll);
	reg_write(dev, I2C_SCLH, data->sclh);
	reg_write(dev, I2C_OA, 0U);
	reg_write(dev, I2C_IE, 0U);
	reg_write(dev, I2C_CON, CON_EN);
}

static int wait_reset_done(const struct device *dev)
{
	int64_t deadline = k_uptime_get() + I2C_TIMEOUT_MS;

	while ((reg_read(dev, I2C_SYSS) & SYSS_RDONE) == 0U) {
		if (k_uptime_get() > deadline) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/*
 * Frees the bus and resets the controller after a transfer failed to start or
 * to complete. A glitch on the lines, such as a device powering up, can leave
 * the controller waiting for a stop condition with the bus busy, and a device
 * interrupted in a byte can hold SDA low. Called with the lock held.
 */
static int controller_recover(const struct device *dev)
{
	int ret = bus_recover(dev);

	/* The reset completes once the controller is enabled again */
	reg_write(dev, I2C_SYSC, SYSC_SOFTRESET);
	controller_setup(dev);
	if (wait_reset_done(dev) != 0) {
		LOG_ERR("controller reset timed out");
		ret = -ETIMEDOUT;
	}

	return ret;
}

static int i2c_omap3_recover_bus(const struct device *dev)
{
	struct i2c_omap3_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = controller_recover(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_omap3_configure(const struct device *dev, uint32_t dev_config)
{
	struct i2c_omap3_data *data = dev->data;
	uint16_t scll;
	uint16_t sclh;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		/* 5 us low, 5 us high */
		scll = 53U;
		sclh = 55U;
		break;
	case I2C_SPEED_FAST:
		/* 1.5 us low, 1 us high */
		scll = 11U;
		sclh = 7U;
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->scll = scll;
	data->sclh = sclh;
	controller_setup(dev);
	data->dev_config = dev_config;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int i2c_omap3_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_omap3_data *data = dev->data;

	*dev_config = data->dev_config;

	return 0;
}

/*
 * Transfer the messages [first, last] as one bus transaction: consecutive
 * messages without I2C_MSG_RESTART continue the previous one.
 */
static int xfer_segment(const struct device *dev, struct i2c_msg *first, struct i2c_msg *last,
			uint16_t addr)
{
	bool read = (first->flags & I2C_MSG_READ) != 0U;
	/* XRDY stays set while the transmit FIFO has room, also in a read */
	uint16_t ready = read ? STAT_RRDY : STAT_XRDY;
	struct i2c_msg *msg = first;
	uint32_t total = 0U;
	uint32_t pos = 0U;
	uint16_t con = CON_EN | CON_MST | CON_STT;
	int64_t deadline = k_uptime_get() + I2C_TIMEOUT_MS;

	for (struct i2c_msg *m = first; m <= last; m++) {
		total += m->len;
	}
	if ((total == 0U) || (total > UINT16_MAX)) {
		return -EINVAL;
	}

	if (!read) {
		con |= CON_TRX;
	}
	if ((last->flags & I2C_MSG_STOP) != 0U) {
		con |= CON_STP;
	}

	reg_write(dev, I2C_SA, addr);
	reg_write(dev, I2C_CNT, (uint16_t)total);
	reg_write(dev, I2C_BUF, BUF_RXFIFO_CLR | BUF_TXFIFO_CLR);
	reg_write(dev, I2C_STAT, STAT_ALL);
	reg_write(dev, I2C_CON, con);

	for (;;) {
		uint16_t stat = reg_read(dev, I2C_STAT);

		if (k_uptime_get() > deadline) {
			reg_write(dev, I2C_CON, CON_EN | CON_MST | CON_STP);
			return -ETIMEDOUT;
		}

		if ((stat & (STAT_NACK | STAT_AL)) != 0U) {
			reg_write(dev, I2C_STAT, stat);
			reg_write(dev, I2C_CON, CON_EN | CON_MST | CON_STP);
			return -EIO;
		}

		if ((stat & ready) != 0U) {
			if (msg <= last) {
				if (read) {
					msg->buf[pos] = data_read(dev);
				} else {
					data_write(dev, msg->buf[pos]);
				}
				if (++pos == msg->len) {
					msg++;
					pos = 0U;
				}
			}
			reg_write(dev, I2C_STAT, ready);
			continue;
		}

		if ((stat & STAT_ARDY) != 0U) {
			reg_write(dev, I2C_STAT, STAT_ARDY);
			return (msg > last) ? 0 : -EIO;
		}
	}
}

static int i2c_omap3_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			      uint16_t addr)
{
	struct i2c_omap3_data *data = dev->data;
	int ret;

	if (num_msgs == 0U) {
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = wait_bus_free(dev);
	if (ret != 0) {
		LOG_WRN("bus busy, recovering");
		(void)controller_recover(dev);
		ret = wait_bus_free(dev);
	}

	for (uint8_t i = 0U; (ret == 0) && (i < num_msgs);) {
		uint8_t j = i;

		/* Extend the segment over messages that continue it */
		while (((j + 1U) < num_msgs) &&
		       ((msgs[j].flags & I2C_MSG_STOP) == 0U) &&
		       ((msgs[j + 1U].flags & I2C_MSG_RESTART) == 0U) &&
		       ((msgs[j + 1U].flags & I2C_MSG_RW_MASK) ==
			(msgs[i].flags & I2C_MSG_RW_MASK))) {
			j++;
		}

		ret = xfer_segment(dev, &msgs[i], &msgs[j], addr);
		i = j + 1U;
	}

	if ((ret == 0) && ((msgs[num_msgs - 1U].flags & I2C_MSG_STOP) != 0U)) {
		ret = wait_bus_free(dev);
	} else if (ret == -ETIMEDOUT) {
		LOG_WRN("transfer to 0x%02x timed out, recovering", addr);
		(void)controller_recover(dev);
	} else if (ret != 0) {
		(void)wait_bus_free(dev);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(i2c, i2c_omap3_api) = {
	.configure = i2c_omap3_configure,
	.get_config = i2c_omap3_get_config,
	.transfer = i2c_omap3_transfer,
	.recover_bus = i2c_omap3_recover_bus,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

static int i2c_omap3_init(const struct device *dev)
{
	const struct i2c_omap3_config *config = dev->config;
	struct i2c_omap3_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	ret = i2c_omap3_configure(dev, I2C_MODE_CONTROLLER | i2c_map_dt_bitrate(config->bitrate));
	if (ret != 0) {
		return ret;
	}

	ret = wait_reset_done(dev);
	if (ret != 0) {
		LOG_ERR("controller not ready");
		return ret;
	}

	/* A warm reset leaves the devices on the bus as they were */
	systest_write(dev, SYSTEST_SCL_O | SYSTEST_SDA_O);
	if (sda_is_low(dev)) {
		LOG_WRN("SDA held low, recovering the bus");
		(void)bus_recover(dev);
	}
	reg_write(dev, I2C_SYSTEST, 0U);

	return 0;
}

#define I2C_OMAP3_INIT(inst)                                                                       \
	static const struct i2c_omap3_config i2c_omap3_config_##inst = {                           \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.bitrate = DT_INST_PROP(inst, clock_frequency),                                    \
	};                                                                                         \
	static struct i2c_omap3_data i2c_omap3_data_##inst;                                        \
	I2C_DEVICE_DT_INST_DEFINE(inst, i2c_omap3_init, NULL, &i2c_omap3_data_##inst,              \
				  &i2c_omap3_config_##inst, POST_KERNEL,                           \
				  CONFIG_I2C_INIT_PRIORITY, &i2c_omap3_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_OMAP3_INIT)
