/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI bq27200 battery fuel gauge. Its current and capacity registers count
 * in units of 3.57 uV and 3.57 uVh across the sense resistor; the current
 * register holds a magnitude and the CHGS flag gives the direction.
 */

#define DT_DRV_COMPAT ti_bq27200

#include <zephyr/device.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define REG_TEMP  0x06U
#define REG_VOLT  0x08U
#define REG_FLAGS 0x0AU
#define REG_SOC   0x0BU
#define REG_NAC   0x0CU
#define REG_FCC   0x12U
#define REG_AI    0x14U
#define REG_TTE   0x16U
#define REG_TTF   0x18U
#define REG_CYCT  0x2AU

#define FLAG_CHGS BIT(7)

/* Sense voltage of one current or capacity count, in nV and nVh */
#define COUNT_NV 3570U

struct bq27200_config {
	struct i2c_dt_spec i2c;
	uint32_t rsense_uohm;
};

static int read_word(const struct device *dev, uint8_t reg, uint16_t *val)
{
	const struct bq27200_config *config = dev->config;
	uint8_t buf[2];
	int ret;

	ret = i2c_burst_read_dt(&config->i2c, reg, buf, sizeof(buf));
	if (ret == 0) {
		*val = sys_get_le16(buf);
	}

	return ret;
}

/* Counts of 3.57 uV (uVh) across the sense resistor, in uA (uAh) */
static uint32_t counts_to_micro(const struct device *dev, uint16_t counts)
{
	const struct bq27200_config *config = dev->config;

	return (uint32_t)(((uint64_t)counts * COUNT_NV * 1000U) / config->rsense_uohm);
}

static int bq27200_get_prop(const struct device *dev, fuel_gauge_prop_t prop,
			    union fuel_gauge_prop_val *val)
{
	const struct bq27200_config *config = dev->config;
	uint16_t word;
	uint8_t byte;
	int ret;

	switch (prop) {
	case FUEL_GAUGE_VOLTAGE_UV:
		ret = read_word(dev, REG_VOLT, &word);
		val->voltage_uv = (int32_t)word * 1000;
		return ret;
	case FUEL_GAUGE_TEMPERATURE_DK:
		/* 0.25 K per count */
		ret = read_word(dev, REG_TEMP, &word);
		val->temperature_dk = (uint16_t)((word * 5U) / 2U);
		return ret;
	case FUEL_GAUGE_AVG_CURRENT_UA:
	case FUEL_GAUGE_CURRENT_UA:
		ret = read_word(dev, REG_AI, &word);
		if (ret == 0) {
			ret = i2c_reg_read_byte_dt(&config->i2c, REG_FLAGS, &byte);
		}
		if (ret == 0) {
			int32_t ua = (int32_t)counts_to_micro(dev, word);

			ua = ((byte & FLAG_CHGS) != 0U) ? ua : -ua;
			if (prop == FUEL_GAUGE_CURRENT_UA) {
				val->current_ua = ua;
			} else {
				val->avg_current_ua = ua;
			}
		}
		return ret;
	case FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_SOC, &byte);
		val->relative_state_of_charge_pct = byte;
		return ret;
	case FUEL_GAUGE_REMAINING_CAPACITY_UAH:
		ret = read_word(dev, REG_NAC, &word);
		val->remaining_capacity_uah = counts_to_micro(dev, word);
		return ret;
	case FUEL_GAUGE_FULL_CHARGE_CAPACITY_UAH:
		ret = read_word(dev, REG_FCC, &word);
		val->full_charge_capacity_uah = counts_to_micro(dev, word);
		return ret;
	case FUEL_GAUGE_RUNTIME_TO_EMPTY_MINS:
		ret = read_word(dev, REG_TTE, &word);
		val->runtime_to_empty_mins = word;
		return ret;
	case FUEL_GAUGE_RUNTIME_TO_FULL_MINS:
		ret = read_word(dev, REG_TTF, &word);
		val->runtime_to_full_mins = word;
		return ret;
	case FUEL_GAUGE_CYCLE_COUNT:
		ret = read_word(dev, REG_CYCT, &word);
		val->cycle_count = word;
		return ret;
	case FUEL_GAUGE_FLAGS:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_FLAGS, &byte);
		val->flags = byte;
		return ret;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(fuel_gauge, bq27200_api) = {
	.get_property = bq27200_get_prop,
};

static int bq27200_init(const struct device *dev)
{
	const struct bq27200_config *config = dev->config;
	uint8_t flags;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	return i2c_reg_read_byte_dt(&config->i2c, REG_FLAGS, &flags);
}

#define BQ27200_INIT(inst)                                                                         \
	static const struct bq27200_config bq27200_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.rsense_uohm = DT_INST_PROP(inst, ti_sense_resistor_micro_ohms),                   \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, bq27200_init, NULL, NULL, &bq27200_config_##inst,              \
			      POST_KERNEL, CONFIG_FUEL_GAUGE_INIT_PRIORITY, &bq27200_api);

DT_INST_FOREACH_STATUS_OKAY(BQ27200_INIT)
