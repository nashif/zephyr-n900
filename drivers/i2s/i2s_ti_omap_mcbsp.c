/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI OMAP3 McBSP (multichannel buffered serial port) as an I2S transmitter.
 * The port takes the bit and frame clocks from the codec and sends PCM
 * frames; the interrupt handler keeps the transmit buffer filled from the
 * queued memory blocks.
 */

#define DT_DRV_COMPAT ti_omap3_mcbsp

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(i2s_omap_mcbsp, CONFIG_I2S_LOG_LEVEL);

#define MCBSP_DXR       0x08U
#define MCBSP_SPCR2     0x10U
#define MCBSP_SPCR1     0x14U
#define MCBSP_XCR2      0x20U
#define MCBSP_XCR1      0x24U
#define MCBSP_PCR       0x48U
#define MCBSP_SYSCON    0x8CU
#define MCBSP_THRSH2    0x90U
#define MCBSP_IRQST     0xA0U
#define MCBSP_IRQEN     0xA4U
#define MCBSP_XBUFFSTAT 0xB4U

#define SPCR2_XRST BIT(0)
#define SPCR2_FREE BIT(9)

#define XCR1_XWDLEN1_SHIFT 5U
#define XCR1_XFRLEN1_SHIFT 8U
#define XWDLEN_8           0U
#define XWDLEN_16          2U
#define XWDLEN_32          5U

#define PCR_CLKXP BIT(1)
#define PCR_FSXP  BIT(3)

#define SYSCON_SOFTRESET    BIT(1)
#define SYSCON_SIDLE_NOIDLE (1U << 3)

#define IRQ_XRDY      BIT(10)
#define IRQ_XUNDFL    BIT(11)
#define IRQ_XEMPTYEOF BIT(14)

#define MAX_CHANNELS 128U

struct queue_item {
	void *mem_block;
	size_t size;
};

struct mcbsp_config {
	mm_reg_t base;
	void (*irq_config)(void);
	const struct pinctrl_dev_config *pcfg;
};

struct mcbsp_data {
	struct i2s_config cfg;
	enum i2s_state state;
	struct k_msgq queue;
	struct queue_item queue_buf[CONFIG_I2S_OMAP_MCBSP_TX_BLOCK_COUNT];
	struct queue_item cur;
	size_t pos;
	uint32_t fifo_size;
	uint8_t word_bytes;
	/* Stopping: after the current block, or after all queued blocks */
	bool drain;
	/* All data is in the transmit buffer; waiting for it to empty */
	bool ending;
};

static inline uint32_t rd(const struct device *dev, uint32_t reg)
{
	const struct mcbsp_config *config = dev->config;

	return sys_read32(config->base + reg);
}

static inline void wr(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct mcbsp_config *config = dev->config;

	sys_write32(val, config->base + reg);
}

static void free_blocks(const struct device *dev)
{
	struct mcbsp_data *data = dev->data;
	struct queue_item item;

	if (data->cur.mem_block != NULL) {
		k_mem_slab_free(data->cfg.mem_slab, data->cur.mem_block);
		data->cur.mem_block = NULL;
	}
	while (k_msgq_get(&data->queue, &item, K_NO_WAIT) == 0) {
		k_mem_slab_free(data->cfg.mem_slab, item.mem_block);
	}
}

static void tx_disable(const struct device *dev)
{
	struct mcbsp_data *data = dev->data;

	wr(dev, MCBSP_IRQEN, 0U);
	wr(dev, MCBSP_SPCR2, 0U);
	wr(dev, MCBSP_IRQST, rd(dev, MCBSP_IRQST));
	data->ending = false;
}

/* Last data written: stop once the transmit buffer has emptied */
static void tx_end(const struct device *dev)
{
	struct mcbsp_data *data = dev->data;

	data->ending = true;
	wr(dev, MCBSP_IRQEN, IRQ_XEMPTYEOF);
}

static void tx_fill(const struct device *dev)
{
	struct mcbsp_data *data = dev->data;
	uint32_t room = rd(dev, MCBSP_XBUFFSTAT);

	while (room > 0U) {
		if (data->cur.mem_block == NULL) {
			if ((data->state == I2S_STATE_STOPPING) && !data->drain) {
				tx_end(dev);
				return;
			}
			if (k_msgq_get(&data->queue, &data->cur, K_NO_WAIT) != 0) {
				if (data->state == I2S_STATE_STOPPING) {
					tx_end(dev);
				} else {
					LOG_ERR("transmit underrun");
					tx_disable(dev);
					data->state = I2S_STATE_ERROR;
				}
				return;
			}
			data->pos = 0U;
		}

		const uint8_t *buf = data->cur.mem_block;

		while ((room > 0U) && (data->pos < data->cur.size)) {
			uint32_t word;

			switch (data->word_bytes) {
			case 1:
				word = buf[data->pos];
				break;
			case 2:
				word = *(const uint16_t *)&buf[data->pos];
				break;
			default:
				word = *(const uint32_t *)&buf[data->pos];
				break;
			}
			wr(dev, MCBSP_DXR, word);
			data->pos += data->word_bytes;
			room--;
		}

		if (data->pos >= data->cur.size) {
			k_mem_slab_free(data->cfg.mem_slab, data->cur.mem_block);
			data->cur.mem_block = NULL;
		}
	}
}

static void mcbsp_isr(const struct device *dev)
{
	struct mcbsp_data *data = dev->data;
	uint32_t st = rd(dev, MCBSP_IRQST);

	wr(dev, MCBSP_IRQST, st);

	if (data->ending) {
		if ((st & IRQ_XEMPTYEOF) != 0U) {
			tx_disable(dev);
			data->state = I2S_STATE_READY;
		}
		return;
	}
	if ((st & IRQ_XRDY) != 0U) {
		tx_fill(dev);
	}
}

static int mcbsp_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg)
{
	struct mcbsp_data *data = dev->data;
	uint32_t xwdlen;
	uint32_t pcr = 0U;
	uint32_t delay;

	if (dir != I2S_DIR_TX) {
		return -ENOSYS;
	}
	if ((data->state != I2S_STATE_NOT_READY) && (data->state != I2S_STATE_READY)) {
		return -EINVAL;
	}

	if (cfg->frame_clk_freq == 0U) {
		free_blocks(dev);
		data->state = I2S_STATE_NOT_READY;
		return 0;
	}

	/* The codec drives both clocks */
	if ((cfg->options & (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) !=
	    (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) {
		LOG_ERR("only clock target mode is supported");
		return -EINVAL;
	}
	if ((cfg->options & (I2S_OPT_LOOPBACK | I2S_OPT_PINGPONG)) != 0U) {
		return -EINVAL;
	}

	switch (cfg->format & I2S_FMT_DATA_FORMAT_MASK) {
	case I2S_FMT_DATA_FORMAT_PCM_SHORT:
		delay = 1U;
		break;
	case I2S_FMT_DATA_FORMAT_PCM_LONG:
		delay = 0U;
		break;
	default:
		LOG_ERR("unsupported data format");
		return -EINVAL;
	}
	if ((cfg->format & I2S_FMT_DATA_ORDER_LSB) != 0U) {
		return -EINVAL;
	}
	if ((cfg->format & I2S_FMT_BIT_CLK_INV) != 0U) {
		pcr |= PCR_CLKXP;
	}
	if ((cfg->format & I2S_FMT_FRAME_CLK_INV) != 0U) {
		pcr |= PCR_FSXP;
	}

	switch (cfg->word_size) {
	case 8:
		xwdlen = XWDLEN_8;
		data->word_bytes = 1U;
		break;
	case 16:
		xwdlen = XWDLEN_16;
		data->word_bytes = 2U;
		break;
	case 32:
		xwdlen = XWDLEN_32;
		data->word_bytes = 4U;
		break;
	default:
		LOG_ERR("unsupported word size %u", cfg->word_size);
		return -EINVAL;
	}
	if ((cfg->channels == 0U) || (cfg->channels > MAX_CHANNELS) ||
	    (cfg->block_size % data->word_bytes != 0U) || (cfg->mem_slab == NULL)) {
		return -EINVAL;
	}

	wr(dev, MCBSP_SPCR2, 0U);
	wr(dev, MCBSP_XCR1,
	   (xwdlen << XCR1_XWDLEN1_SHIFT) | ((cfg->channels - 1U) << XCR1_XFRLEN1_SHIFT));
	wr(dev, MCBSP_XCR2, delay);
	wr(dev, MCBSP_PCR, pcr);
	/* Ask for data once half the transmit buffer is free */
	wr(dev, MCBSP_THRSH2, data->fifo_size / 2U);

	data->cfg = *cfg;
	data->state = I2S_STATE_READY;

	return 0;
}

static const struct i2s_config *mcbsp_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct mcbsp_data *data = dev->data;

	if ((dir != I2S_DIR_TX) || (data->state == I2S_STATE_NOT_READY)) {
		return NULL;
	}

	return &data->cfg;
}

static int mcbsp_write(const struct device *dev, void *mem_block, size_t size)
{
	struct mcbsp_data *data = dev->data;
	struct queue_item item = {.mem_block = mem_block, .size = size};

	if ((data->state != I2S_STATE_READY) && (data->state != I2S_STATE_RUNNING)) {
		return -EIO;
	}
	if ((size > data->cfg.block_size) || (size % data->word_bytes != 0U)) {
		return -EINVAL;
	}

	return k_msgq_put(&data->queue, &item, SYS_TIMEOUT_MS(data->cfg.timeout));
}

static int mcbsp_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct mcbsp_data *data = dev->data;
	unsigned int key;
	int ret = 0;

	if (dir != I2S_DIR_TX) {
		return -ENOSYS;
	}

	key = irq_lock();

	switch (cmd) {
	case I2S_TRIGGER_START:
		if ((data->state != I2S_STATE_READY) || (k_msgq_num_used_get(&data->queue) == 0U)) {
			ret = -EIO;
			break;
		}
		data->drain = false;
		data->ending = false;
		wr(dev, MCBSP_IRQST, rd(dev, MCBSP_IRQST));
		wr(dev, MCBSP_SPCR2, SPCR2_FREE | SPCR2_XRST);
		data->state = I2S_STATE_RUNNING;
		tx_fill(dev);
		if (data->state == I2S_STATE_RUNNING && !data->ending) {
			wr(dev, MCBSP_IRQEN, IRQ_XRDY);
		}
		break;
	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		if (data->state != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}
		data->drain = (cmd == I2S_TRIGGER_DRAIN);
		data->state = I2S_STATE_STOPPING;
		break;
	case I2S_TRIGGER_DROP:
		if (data->state == I2S_STATE_NOT_READY) {
			ret = -EIO;
			break;
		}
		tx_disable(dev);
		free_blocks(dev);
		data->state = I2S_STATE_READY;
		break;
	case I2S_TRIGGER_PREPARE:
		if (data->state != I2S_STATE_ERROR) {
			ret = -EIO;
			break;
		}
		free_blocks(dev);
		data->state = I2S_STATE_READY;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	irq_unlock(key);

	return ret;
}

static DEVICE_API(i2s, mcbsp_api) = {
	.configure = mcbsp_configure,
	.config_get = mcbsp_config_get,
	.write = mcbsp_write,
	.trigger = mcbsp_trigger,
};

static int mcbsp_init(const struct device *dev)
{
	const struct mcbsp_config *config = dev->config;
	struct mcbsp_data *data = dev->data;
	int ret;

	if (IS_ENABLED(CONFIG_PINCTRL) && (config->pcfg != NULL)) {
		ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret != 0) {
			return ret;
		}
	}

	wr(dev, MCBSP_SYSCON, SYSCON_SOFTRESET);
	while ((rd(dev, MCBSP_SYSCON) & SYSCON_SOFTRESET) != 0U) {
	}
	wr(dev, MCBSP_SYSCON, SYSCON_SIDLE_NOIDLE);
	wr(dev, MCBSP_SPCR2, 0U);
	wr(dev, MCBSP_SPCR1, 0U);
	/* After reset the whole transmit buffer is free */
	data->fifo_size = rd(dev, MCBSP_XBUFFSTAT);

	k_msgq_init(&data->queue, (char *)data->queue_buf, sizeof(struct queue_item),
		    ARRAY_SIZE(data->queue_buf));
	data->state = I2S_STATE_NOT_READY;

	config->irq_config();

	return 0;
}

#define MCBSP_PINCTRL_DEFINE(inst)                                                                 \
	IF_ENABLED(DT_INST_PINCTRL_HAS_NAME(inst, default), (PINCTRL_DT_INST_DEFINE(inst);))

#define MCBSP_PINCTRL_GET(inst)                                                                    \
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(inst, default),                                       \
		    (PINCTRL_DT_INST_DEV_CONFIG_GET(inst)), (NULL))

#define MCBSP_INIT(inst)                                                                           \
	MCBSP_PINCTRL_DEFINE(inst)                                                                 \
	static void mcbsp_irq_config_##inst(void)                                                  \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), mcbsp_isr,            \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}                                                                                          \
	static const struct mcbsp_config mcbsp_config_##inst = {                                   \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.irq_config = mcbsp_irq_config_##inst,                                             \
		.pcfg = MCBSP_PINCTRL_GET(inst),                                                   \
	};                                                                                         \
	static struct mcbsp_data mcbsp_data_##inst;                                                \
	DEVICE_DT_INST_DEFINE(inst, mcbsp_init, NULL, &mcbsp_data_##inst, &mcbsp_config_##inst,    \
			      POST_KERNEL, CONFIG_I2S_INIT_PRIORITY, &mcbsp_api);

DT_INST_FOREACH_STATUS_OKAY(MCBSP_INIT)
