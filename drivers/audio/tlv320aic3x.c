/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI TLV320AIC3x stereo audio codecs (AIC33, AIC34 and relatives): playback
 * through the DACs to the high-power and line outputs. The PLL derives the
 * 2048 x fs codec clock from MCLK.
 */

#define DT_DRV_COMPAT ti_tlv320aic3x

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tlv320aic3x, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#define REG_PAGE         0U
#define REG_RESET        1U
#define REG_RATE         2U
#define REG_PLL_A        3U
#define REG_PLL_B        4U
#define REG_PLL_C        5U
#define REG_PLL_D        6U
#define REG_DATAPATH     7U
#define REG_IFACE_A      8U
#define REG_IFACE_B      9U
#define REG_IFACE_C      10U
#define REG_PLL_R        11U
#define REG_DAC_PWR      37U
#define REG_HPRCOM_CFG   38U
#define REG_DAC_MUX      41U
#define REG_LDAC_VOL     43U
#define REG_RDAC_VOL     44U
#define REG_DACL1_HPLOUT 47U
#define REG_HPLOUT_CTRL  51U
#define REG_HPLCOM_CTRL  58U
#define REG_DACR1_HPROUT 64U
#define REG_HPROUT_CTRL  65U
#define REG_HPRCOM_CTRL  72U
#define REG_DACL1_LLOPM  82U
#define REG_LLOPM_CTRL   86U
#define REG_DACR1_RLOPM  92U
#define REG_RLOPM_CTRL   93U
#define REG_GPIOB        101U
#define REG_CLKGEN       102U

#define RESET_SOFT         BIT(7)
#define PLL_A_ENABLE       BIT(7)
#define PLL_A_P_MASK       GENMASK(2, 0)
#define PLL_B_J_SHIFT      2U
#define PLL_D_LSB_SHIFT    2U
#define DATAPATH_FS_44K    BIT(7)
/* Left DAC plays the left channel, right DAC the right channel */
#define DATAPATH_LR        ((1U << 3) | (1U << 1))
#define IFACE_A_BCLK_OUT   BIT(7)
#define IFACE_A_WCLK_OUT   BIT(6)
#define IFACE_B_MODE_SHIFT 6U
#define IFACE_B_LEN_SHIFT  4U
#define MODE_I2S           0U
#define MODE_DSP           1U
#define MODE_RJ            2U
#define MODE_LJ            3U
#define DAC_PWR_L          BIT(7)
#define DAC_PWR_R          BIT(6)
#define DAC_VOL_MUTE       BIT(7)
#define DAC_VOL_MASK       GENMASK(6, 0)
#define ROUTE_ON           BIT(7)
/* Output level 0 dB, not muted, powered */
#define OUT_ON             (BIT(3) | BIT(0))
/* Inputs of the PLL and the clock divider: MCLK */
#define CLKGEN_MCLK        0x02U

#define PLL_MAX_P  8U
#define PLL_MIN_J  4U
#define PLL_MAX_J  11U
#define PLL_MIN_IN 10000000U
#define PLL_MAX_IN 20000000U

struct aic3x_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec reset;
	bool hp_out;
	bool line_out;
};

struct aic3x_data {
	uint8_t vol[2];
	bool mute;
};

struct reg_val {
	uint8_t reg;
	uint8_t val;
};

static int reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct aic3x_config *config = dev->config;

	return i2c_reg_write_byte_dt(&config->bus, reg, val);
}

static int reg_write_seq(const struct device *dev, const struct reg_val *seq, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = reg_write(dev, seq[i].reg, seq[i].val);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/*
 * PLL with R = 1: codec clock = MCLK * J.D / P = 2048 * fsref, the input to
 * the fractional multiplier MCLK / P between 10 and 20 MHz.
 */
static int pll_set(const struct device *dev, uint32_t mclk, uint32_t fsref)
{
	uint64_t target = 2048ULL * fsref;

	for (uint32_t p = 1U; p <= PLL_MAX_P; p++) {
		uint32_t in = mclk / p;
		uint64_t k10000;
		uint32_t j;
		uint32_t d;

		if ((in < PLL_MIN_IN) || (in > PLL_MAX_IN)) {
			continue;
		}
		k10000 = (target * p * 10000ULL + mclk / 2U) / mclk;
		j = (uint32_t)(k10000 / 10000U);
		d = (uint32_t)(k10000 % 10000U);
		if ((j < PLL_MIN_J) || (j > PLL_MAX_J)) {
			continue;
		}

		const struct reg_val seq[] = {
			{REG_PLL_A, PLL_A_ENABLE | (p & PLL_A_P_MASK)},
			{REG_PLL_B, j << PLL_B_J_SHIFT},
			{REG_PLL_C, d >> 6},
			{REG_PLL_D, (d & 0x3FU) << PLL_D_LSB_SHIFT},
			{REG_PLL_R, 1U},
		};

		LOG_DBG("PLL P %u J %u D %u", p, j, d);
		return reg_write_seq(dev, seq, ARRAY_SIZE(seq));
	}

	LOG_ERR("no PLL setting for MCLK %u Hz", mclk);
	return -EINVAL;
}

static int aic3x_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	const struct aic3x_config *config = dev->config;
	struct i2s_config *i2s = &cfg->dai_cfg.i2s;
	uint32_t fsref;
	uint32_t div;
	uint8_t mode;
	uint8_t len;
	uint8_t iface_a = 0U;
	uint8_t offset = 0U;
	int ret;

	if (cfg->dai_route != AUDIO_ROUTE_PLAYBACK) {
		return -ENOTSUP;
	}

	switch (cfg->dai_type) {
	case AUDIO_DAI_TYPE_I2S:
		mode = MODE_I2S;
		break;
	case AUDIO_DAI_TYPE_LEFT_JUSTIFIED:
		mode = MODE_LJ;
		break;
	case AUDIO_DAI_TYPE_RIGHT_JUSTIFIED:
		mode = MODE_RJ;
		break;
	case AUDIO_DAI_TYPE_PCMA:
		/* Data one bit clock after the frame sync */
		mode = MODE_DSP;
		offset = 1U;
		break;
	case AUDIO_DAI_TYPE_PCMB:
		mode = MODE_DSP;
		break;
	default:
		return -ENOTSUP;
	}

	switch (i2s->word_size) {
	case 16:
		len = 0U;
		break;
	case 20:
		len = 1U;
		break;
	case 24:
		len = 2U;
		break;
	case 32:
		len = 3U;
		break;
	default:
		return -ENOTSUP;
	}

	/* fs = fsref / div, fsref 48 or 44.1 kHz, integer div 1 to 6 */
	fsref = ((48000U % i2s->frame_clk_freq) == 0U) ? 48000U : 44100U;
	div = fsref / i2s->frame_clk_freq;
	if ((div < 1U) || (div > 6U) || ((fsref % i2s->frame_clk_freq) != 0U)) {
		LOG_ERR("unsupported rate %u Hz", i2s->frame_clk_freq);
		return -ENOTSUP;
	}

	if ((i2s->options & I2S_OPT_BIT_CLK_TARGET) == 0U) {
		iface_a |= IFACE_A_BCLK_OUT;
	}
	if ((i2s->options & I2S_OPT_FRAME_CLK_TARGET) == 0U) {
		iface_a |= IFACE_A_WCLK_OUT;
	}

	if (config->reset.port != NULL) {
		gpio_pin_set_dt(&config->reset, 1);
		k_busy_wait(10);
		gpio_pin_set_dt(&config->reset, 0);
		k_busy_wait(10);
	}

	const struct reg_val reset_seq[] = {
		{REG_PAGE, 0U},
		{REG_RESET, RESET_SOFT},
		{REG_CLKGEN, CLKGEN_MCLK},
		{REG_GPIOB, 0U},
	};
	const struct reg_val setup_seq[] = {
		/* The ADC and DAC rate codes are twice the divider minus two */
		{REG_RATE, ((2U * div - 2U) << 4) | (2U * div - 2U)},
		{REG_DATAPATH, ((fsref == 44100U) ? DATAPATH_FS_44K : 0U) | DATAPATH_LR},
		{REG_IFACE_A, iface_a},
		{REG_IFACE_B, (mode << IFACE_B_MODE_SHIFT) | (len << IFACE_B_LEN_SHIFT)},
		{REG_IFACE_C, offset},
		/* DAC_L1 and DAC_R1 paths, the COM outputs inverting HPLOUT and HPROUT */
		{REG_DAC_MUX, 0U},
		{REG_HPRCOM_CFG, 0U},
		{REG_DACL1_HPLOUT, config->hp_out ? ROUTE_ON : 0U},
		{REG_DACR1_HPROUT, config->hp_out ? ROUTE_ON : 0U},
		{REG_DACL1_LLOPM, config->line_out ? ROUTE_ON : 0U},
		{REG_DACR1_RLOPM, config->line_out ? ROUTE_ON : 0U},
	};

	ret = reg_write_seq(dev, reset_seq, ARRAY_SIZE(reset_seq));
	if (ret != 0) {
		return ret;
	}
	ret = pll_set(dev, cfg->mclk_freq, fsref);
	if (ret != 0) {
		return ret;
	}

	return reg_write_seq(dev, setup_seq, ARRAY_SIZE(setup_seq));
}

static int outputs_set(const struct device *dev, bool on)
{
	const struct aic3x_config *config = dev->config;
	uint8_t hp = (on && config->hp_out) ? OUT_ON : 0U;
	uint8_t line = (on && config->line_out) ? OUT_ON : 0U;
	const struct reg_val seq[] = {
		{REG_DAC_PWR, on ? (DAC_PWR_L | DAC_PWR_R) : 0U},
		{REG_HPLOUT_CTRL, hp},
		{REG_HPROUT_CTRL, hp},
		{REG_HPLCOM_CTRL, hp},
		{REG_HPRCOM_CTRL, hp},
		{REG_LLOPM_CTRL, line},
		{REG_RLOPM_CTRL, line},
	};

	return reg_write_seq(dev, seq, ARRAY_SIZE(seq));
}

static int aic3x_apply_properties(const struct device *dev)
{
	struct aic3x_data *data = dev->data;
	uint8_t mute = data->mute ? DAC_VOL_MUTE : 0U;
	const struct reg_val seq[] = {
		{REG_LDAC_VOL, mute | data->vol[0]},
		{REG_RDAC_VOL, mute | data->vol[1]},
	};

	return reg_write_seq(dev, seq, ARRAY_SIZE(seq));
}

static void aic3x_start_output(const struct device *dev)
{
	(void)outputs_set(dev, true);
	(void)aic3x_apply_properties(dev);
}

static void aic3x_stop_output(const struct device *dev)
{
	(void)reg_write(dev, REG_LDAC_VOL, DAC_VOL_MUTE);
	(void)reg_write(dev, REG_RDAC_VOL, DAC_VOL_MUTE);
	(void)outputs_set(dev, false);
}

/*
 * Output volume in 0.5 dB steps of DAC attenuation: 0 is full scale, -127
 * is -63.5 dB.
 */
static int aic3x_set_property(const struct device *dev, audio_property_t property,
			      audio_channel_t channel, audio_property_value_t val)
{
	struct aic3x_data *data = dev->data;
	uint8_t att;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		if ((val.vol > 0) || (val.vol < -(int)DAC_VOL_MASK)) {
			return -EINVAL;
		}
		att = (uint8_t)(-val.vol);
		switch (channel) {
		case AUDIO_CHANNEL_FRONT_LEFT:
			data->vol[0] = att;
			break;
		case AUDIO_CHANNEL_FRONT_RIGHT:
			data->vol[1] = att;
			break;
		case AUDIO_CHANNEL_ALL:
			data->vol[0] = att;
			data->vol[1] = att;
			break;
		default:
			return -EINVAL;
		}
		return 0;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		if (channel != AUDIO_CHANNEL_ALL) {
			return -EINVAL;
		}
		data->mute = val.mute;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(audio_codec, aic3x_api) = {
	.configure = aic3x_configure,
	.start_output = aic3x_start_output,
	.stop_output = aic3x_stop_output,
	.set_property = aic3x_set_property,
	.apply_properties = aic3x_apply_properties,
};

static int aic3x_init(const struct device *dev)
{
	const struct aic3x_config *config = dev->config;

	if (!i2c_is_ready_dt(&config->bus)) {
		return -ENODEV;
	}
	if (config->reset.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset)) {
			return -ENODEV;
		}
		/* Release the codec from reset so that it answers on the bus */
		return gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_INACTIVE);
	}

	return 0;
}

#define AIC3X_INIT(inst)                                                                           \
	static const struct aic3x_config aic3x_config_##inst = {                                   \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                         \
		.hp_out = DT_INST_PROP(inst, hp_out),                                              \
		.line_out = DT_INST_PROP(inst, line_out),                                          \
	};                                                                                         \
	static struct aic3x_data aic3x_data_##inst;                                                \
	DEVICE_DT_INST_DEFINE(inst, aic3x_init, NULL, &aic3x_data_##inst, &aic3x_config_##inst,    \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY, &aic3x_api);

DT_INST_FOREACH_STATUS_OKAY(AIC3X_INIT)
