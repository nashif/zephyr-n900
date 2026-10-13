/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Vibrator driver of the TI TWL4030 and TWL5030 power management chips, a
 * child of the chip's MFD device. The audio block's PWM drives the motor at
 * a fixed duty cycle; the block and its PLL are powered only while the motor
 * runs.
 */

#define DT_DRV_COMPAT ti_twl4030_vibra

#include <zephyr/device.h>
#include <zephyr/drivers/haptics.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>

/* Audio and vibrator registers, at the chip's second I2C address */
#define AUDIO_ADDR_IDX 1U
#define CODEC_MODE     0x01U
#define APLL_CTL       0x3AU
#define VIBRA_CTL      0x45U
#define VIBRA_SET      0x46U

/* Boot configuration, in the power master registers */
#define CFG_BOOT       0x3BU
#define CFG_BOOT_HFCLK GENMASK(1, 0)
#define HFCLK_19P2_MHZ 1U
#define HFCLK_26_MHZ   2U
#define HFCLK_38P4_MHZ 3U

/* Audio PLL input frequency codes */
#define APLL_INFREQ_19P2_MHZ 0x5U
#define APLL_INFREQ_26_MHZ   0x6U
#define APLL_INFREQ_38P4_MHZ 0xFU

/* LED driver registers, at the chip's third I2C address */
#define LED_ADDR_IDX 2U
#define LEDEN        0xEEU

#define CODEC_MODE_CODECPDZ BIT(1)
#define APLL_CTL_EN         BIT(4)
#define APLL_CTL_INFREQ     GENMASK(3, 0)
#define VIBRA_CTL_EN        BIT(0)
#define VIBRA_CTL_DIR       BIT(1)
#define VIBRA_CTL_SEL       BIT(4)
#define VIBRA_CTL_DIR_SEL   BIT(5)
/* LEDA and LEDB share the PWM with the vibrator */
#define LEDEN_LEDAON_LEDBON GENMASK(1, 0)

struct twl4030_vibra_config {
	const struct device *mfd;
	uint8_t duty;
};

struct twl4030_vibra_data {
	struct k_mutex lock;
	uint8_t apll_infreq;
};

static int update_bits(const struct device *mfd, uint8_t addr_idx, uint8_t reg, uint8_t mask,
		       uint8_t val)
{
	uint8_t old;
	int ret;

	ret = mfd_twl4030_reg_read(mfd, addr_idx, reg, &old);
	if (ret != 0) {
		return ret;
	}

	return mfd_twl4030_reg_write(mfd, addr_idx, reg, (old & ~mask) | (val & mask));
}

static int twl4030_vibra_start_output(const struct device *dev)
{
	const struct twl4030_vibra_config *config = dev->config;
	struct twl4030_vibra_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = update_bits(config->mfd, AUDIO_ADDR_IDX, CODEC_MODE, CODEC_MODE_CODECPDZ,
			  CODEC_MODE_CODECPDZ);
	if (ret == 0) {
		ret = update_bits(config->mfd, AUDIO_ADDR_IDX, APLL_CTL,
				  APLL_CTL_EN | APLL_CTL_INFREQ, APLL_CTL_EN | data->apll_infreq);
	}
	if (ret == 0) {
		/* The PWM is low for the set number of 256 cycles */
		ret = mfd_twl4030_reg_write(config->mfd, AUDIO_ADDR_IDX, VIBRA_SET,
					    (uint8_t)(256U - config->duty));
	}
	if (ret == 0) {
		/* Driven by the local PWM, one direction */
		ret = update_bits(config->mfd, AUDIO_ADDR_IDX, VIBRA_CTL,
				  VIBRA_CTL_EN | VIBRA_CTL_DIR | VIBRA_CTL_SEL | VIBRA_CTL_DIR_SEL,
				  VIBRA_CTL_EN);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int twl4030_vibra_stop_output(const struct device *dev)
{
	const struct twl4030_vibra_config *config = dev->config;
	struct twl4030_vibra_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = update_bits(config->mfd, AUDIO_ADDR_IDX, VIBRA_CTL, VIBRA_CTL_EN, 0U);
	if (ret == 0) {
		ret = update_bits(config->mfd, AUDIO_ADDR_IDX, APLL_CTL, APLL_CTL_EN, 0U);
	}
	if (ret == 0) {
		ret = update_bits(config->mfd, AUDIO_ADDR_IDX, CODEC_MODE, CODEC_MODE_CODECPDZ, 0U);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(haptics, twl4030_vibra_api) = {
	.start_output = twl4030_vibra_start_output,
	.stop_output = twl4030_vibra_stop_output,
};

static int twl4030_vibra_init(const struct device *dev)
{
	const struct twl4030_vibra_config *config = dev->config;
	struct twl4030_vibra_data *data = dev->data;
	uint8_t boot;
	int ret;

	if (!device_is_ready(config->mfd)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

	/* The audio PLL runs from HFCLKIN, whose rate the boot configuration records */
	ret = mfd_twl4030_reg_read(config->mfd, TWL4030_ADDR_PM_MASTER, CFG_BOOT, &boot);
	if (ret != 0) {
		return ret;
	}
	switch (boot & CFG_BOOT_HFCLK) {
	case HFCLK_19P2_MHZ:
		data->apll_infreq = APLL_INFREQ_19P2_MHZ;
		break;
	case HFCLK_26_MHZ:
		data->apll_infreq = APLL_INFREQ_26_MHZ;
		break;
	case HFCLK_38P4_MHZ:
		data->apll_infreq = APLL_INFREQ_38P4_MHZ;
		break;
	default:
		return -EINVAL;
	}

	ret = update_bits(config->mfd, LED_ADDR_IDX, LEDEN, LEDEN_LEDAON_LEDBON, 0U);
	if (ret != 0) {
		return ret;
	}

	return update_bits(config->mfd, AUDIO_ADDR_IDX, VIBRA_CTL, VIBRA_CTL_EN, 0U);
}

#define TWL4030_VIBRA_INIT(inst)                                                                   \
	static const struct twl4030_vibra_config twl4030_vibra_config_##inst = {                   \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.duty = (uint8_t)DIV_ROUND_UP(DT_INST_PROP(inst, duty_cycle_percent) * 255U,       \
					      100U),                                               \
	};                                                                                         \
	static struct twl4030_vibra_data twl4030_vibra_data_##inst;                                \
	DEVICE_DT_INST_DEFINE(inst, twl4030_vibra_init, NULL, &twl4030_vibra_data_##inst,          \
			      &twl4030_vibra_config_##inst, POST_KERNEL,                           \
			      CONFIG_HAPTICS_INIT_PRIORITY, &twl4030_vibra_api);

DT_INST_FOREACH_STATUS_OKAY(TWL4030_VIBRA_INIT)
