/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI bq24150A single-cell Li-ion charger. Once the host writes to it, the
 * charger keeps the host settings only while the host resets its 32 s timer;
 * otherwise it falls back to its default mode, which charges to 3.54 V.
 * A delayed work item resets the timer and restores the settings after such
 * a fallback.
 */

#define DT_DRV_COMPAT ti_bq24150a

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bq2415x, CONFIG_CHARGER_LOG_LEVEL);

#define REG_STATUS  0x00U
#define REG_CONTROL 0x01U
#define REG_VOLTAGE 0x02U
#define REG_VENDOR  0x03U
#define REG_CURRENT 0x04U

#define STATUS_TMR_RST    BIT(7)
#define STATUS_EN_STAT    BIT(6)
#define STATUS_STAT       GENMASK(5, 4)
#define STATUS_FAULT      GENMASK(2, 0)
#define STAT_READY        0U
#define STAT_CHARGING     1U
#define STAT_DONE         2U
#define FAULT_SLEEP       2U
#define FAULT_POOR_SOURCE 3U
#define FAULT_BATTERY_OVP 4U
#define FAULT_THERMAL     5U
#define FAULT_TIMER       6U
#define FAULT_NO_BATTERY  7U

#define CONTROL_LIMIT GENMASK(7, 6)
#define CONTROL_VLOWV GENMASK(5, 4)
#define CONTROL_TE    BIT(3)
#define CONTROL_CE    BIT(2)

#define VOLTAGE_VO GENMASK(7, 2)

#define VENDOR_VENDOR GENMASK(7, 5)
#define VENDOR_TI     2U

/* Bit 7 (RESET) is always written as 0: 1 resets all registers */
#define CURRENT_VI_CHRG GENMASK(6, 4)
#define CURRENT_VI_TERM GENMASK(2, 0)

#define VO_BASE_UV      3500000U
#define VO_STEP_UV      20000U
#define VO_MAX          47U
#define VI_CHRG_BASE_UV 37400U
#define VI_CHRG_STEP_UV 6800U
#define VI_TERM_BASE_UV 3400U
#define VI_TERM_STEP_UV 3400U

#define TIMER_RESET_PERIOD K_SECONDS(10)

static const uint32_t input_limit_ua[] = {100000U, 500000U, 800000U, 1800000U};

struct bq2415x_config {
	struct i2c_dt_spec i2c;
	uint32_t ichg_max_ua;
	uint32_t vreg_max_uv;
	uint32_t iterm_ua;
	uint32_t ilim_ua;
	uint32_t vweak_uv;
	uint32_t rsense_uohm;
};

struct bq2415x_data {
	const struct device *dev;
	struct k_work_delayable timer_work;
	struct k_mutex lock;
	uint32_t ichg_ua;
	uint32_t vreg_uv;
	uint32_t iterm_ua;
	uint32_t ilim_ua;
	bool charge_enabled;
};

/* Sense voltage, in uV, for a current through the sense resistor */
static uint32_t sense_uv(const struct bq2415x_config *config, uint32_t ua)
{
	return (uint32_t)(((uint64_t)ua * config->rsense_uohm) / 1000000U);
}

static uint32_t sense_ua(const struct bq2415x_config *config, uint32_t uv)
{
	return (uint32_t)(((uint64_t)uv * 1000000U) / config->rsense_uohm);
}

static uint8_t step_code(uint32_t value, uint32_t base, uint32_t step, uint8_t max)
{
	return (value <= base) ? 0U : (uint8_t)MIN((value - base) / step, max);
}

static uint8_t vo_code(uint32_t uv)
{
	return step_code(uv, VO_BASE_UV, VO_STEP_UV, VO_MAX);
}

static uint8_t limit_code(uint32_t ua)
{
	for (uint8_t i = 0U; i < ARRAY_SIZE(input_limit_ua) - 1U; i++) {
		if (ua <= input_limit_ua[i]) {
			return i;
		}
	}

	return ARRAY_SIZE(input_limit_ua) - 1U;
}

static uint8_t vlowv_code(uint32_t uv)
{
	/* 3.4 V to 3.7 V in 100 mV steps, rounded to the nearest */
	return step_code(uv + 50000U, 3400000U, 100000U, 3U);
}

static int apply_config(const struct device *dev)
{
	const struct bq2415x_config *config = dev->config;
	struct bq2415x_data *data = dev->data;
	uint8_t current = FIELD_PREP(CURRENT_VI_CHRG,
				     step_code(sense_uv(config, data->ichg_ua), VI_CHRG_BASE_UV,
					       VI_CHRG_STEP_UV, 7U)) |
			  FIELD_PREP(CURRENT_VI_TERM,
				     step_code(sense_uv(config, data->iterm_ua), VI_TERM_BASE_UV,
					       VI_TERM_STEP_UV, 7U));
	uint8_t control = FIELD_PREP(CONTROL_LIMIT, limit_code(data->ilim_ua)) |
			  FIELD_PREP(CONTROL_VLOWV, vlowv_code(config->vweak_uv)) | CONTROL_TE |
			  (data->charge_enabled ? 0U : CONTROL_CE);
	int ret;

	ret = i2c_reg_write_byte_dt(&config->i2c, REG_CURRENT, current);
	if (ret == 0) {
		ret = i2c_reg_write_byte_dt(&config->i2c, REG_VOLTAGE,
					    FIELD_PREP(VOLTAGE_VO, vo_code(data->vreg_uv)));
	}
	if (ret == 0) {
		ret = i2c_reg_write_byte_dt(&config->i2c, REG_CONTROL, control);
	}
	if (ret == 0) {
		ret = i2c_reg_write_byte_dt(&config->i2c, REG_STATUS, STATUS_TMR_RST);
	}

	return ret;
}

static void timer_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct bq2415x_data *data = CONTAINER_OF(dwork, struct bq2415x_data, timer_work);
	const struct device *dev = data->dev;
	const struct bq2415x_config *config = dev->config;
	uint8_t status;
	uint8_t voltage;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = i2c_reg_read_byte_dt(&config->i2c, REG_STATUS, &status);
	if (ret == 0) {
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_VOLTAGE, &voltage);
	}

	if (ret != 0) {
		LOG_WRN("charger not responding (%d)", ret);
	} else if ((FIELD_GET(VOLTAGE_VO, voltage) != vo_code(data->vreg_uv)) ||
		   (FIELD_GET(STATUS_FAULT, status) == FAULT_TIMER)) {
		/* Back in default mode: the timer expired */
		LOG_WRN("restoring charger settings");
		(void)apply_config(dev);
	} else {
		(void)i2c_reg_write_byte_dt(&config->i2c, REG_STATUS,
					    (status & STATUS_EN_STAT) | STATUS_TMR_RST);
	}

	k_mutex_unlock(&data->lock);

	k_work_reschedule(dwork, TIMER_RESET_PERIOD);
}

static int bq2415x_get_prop(const struct device *dev, charger_prop_t prop,
			    union charger_propval *val)
{
	const struct bq2415x_config *config = dev->config;
	uint8_t status;
	uint8_t reg;
	uint8_t stat;
	uint8_t fault;
	bool online;
	int ret;

	ret = i2c_reg_read_byte_dt(&config->i2c, REG_STATUS, &status);
	if (ret != 0) {
		return ret;
	}

	stat = FIELD_GET(STATUS_STAT, status);
	fault = FIELD_GET(STATUS_FAULT, status);
	online = (fault != FAULT_SLEEP) && (fault != FAULT_POOR_SOURCE);

	switch (prop) {
	case CHARGER_PROP_ONLINE:
		val->online = online ? CHARGER_ONLINE_FIXED : CHARGER_ONLINE_OFFLINE;
		return 0;
	case CHARGER_PROP_PRESENT:
		val->present = (fault != FAULT_NO_BATTERY);
		return 0;
	case CHARGER_PROP_STATUS:
		if (!online) {
			val->status = CHARGER_STATUS_DISCHARGING;
		} else if (stat == STAT_CHARGING) {
			val->status = CHARGER_STATUS_CHARGING;
		} else if (stat == STAT_DONE) {
			val->status = CHARGER_STATUS_FULL;
		} else {
			val->status = CHARGER_STATUS_NOT_CHARGING;
		}
		return 0;
	case CHARGER_PROP_HEALTH:
		switch (fault) {
		case FAULT_BATTERY_OVP:
			val->health = CHARGER_HEALTH_OVERVOLTAGE;
			break;
		case FAULT_THERMAL:
			val->health = CHARGER_HEALTH_OVERHEAT;
			break;
		case FAULT_TIMER:
			val->health = CHARGER_HEALTH_WATCHDOG_TIMER_EXPIRE;
			break;
		case FAULT_NO_BATTERY:
			val->health = CHARGER_HEALTH_NO_BATTERY;
			break;
		default:
			val->health = CHARGER_HEALTH_GOOD;
			break;
		}
		return 0;
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_CURRENT, &reg);
		val->const_charge_current_ua =
			sense_ua(config, VI_CHRG_BASE_UV +
						 FIELD_GET(CURRENT_VI_CHRG, reg) * VI_CHRG_STEP_UV);
		return ret;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_CURRENT, &reg);
		val->charge_term_current_ua =
			sense_ua(config, VI_TERM_BASE_UV +
						 FIELD_GET(CURRENT_VI_TERM, reg) * VI_TERM_STEP_UV);
		return ret;
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_VOLTAGE, &reg);
		val->const_charge_voltage_uv = VO_BASE_UV + FIELD_GET(VOLTAGE_VO, reg) * VO_STEP_UV;
		return ret;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		ret = i2c_reg_read_byte_dt(&config->i2c, REG_CONTROL, &reg);
		val->input_current_regulation_current_ua =
			input_limit_ua[FIELD_GET(CONTROL_LIMIT, reg)];
		return ret;
	default:
		return -ENOTSUP;
	}
}

static int bq2415x_set_prop(const struct device *dev, charger_prop_t prop,
			    const union charger_propval *val)
{
	const struct bq2415x_config *config = dev->config;
	struct bq2415x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (prop) {
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		data->ichg_ua = MIN(val->const_charge_current_ua, config->ichg_max_ua);
		break;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		data->iterm_ua = val->charge_term_current_ua;
		break;
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		data->vreg_uv = MIN(val->const_charge_voltage_uv, config->vreg_max_uv);
		break;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		data->ilim_ua = val->input_current_regulation_current_ua;
		break;
	default:
		k_mutex_unlock(&data->lock);
		return -ENOTSUP;
	}

	ret = apply_config(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int bq2415x_charge_enable(const struct device *dev, const bool enable)
{
	struct bq2415x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->charge_enabled = enable;
	ret = apply_config(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(charger, bq2415x_api) = {
	.get_property = bq2415x_get_prop,
	.set_property = bq2415x_set_prop,
	.charge_enable = bq2415x_charge_enable,
};

static int bq2415x_init(const struct device *dev)
{
	const struct bq2415x_config *config = dev->config;
	struct bq2415x_data *data = dev->data;
	uint8_t vendor;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	ret = i2c_reg_read_byte_dt(&config->i2c, REG_VENDOR, &vendor);
	if (ret != 0) {
		return ret;
	}
	if (FIELD_GET(VENDOR_VENDOR, vendor) != VENDOR_TI) {
		LOG_ERR("unexpected vendor register 0x%02x", vendor);
		return -ENODEV;
	}

	data->dev = dev;
	k_mutex_init(&data->lock);
	data->ichg_ua = config->ichg_max_ua;
	data->vreg_uv = config->vreg_max_uv;
	data->iterm_ua = config->iterm_ua;
	data->ilim_ua = config->ilim_ua;
	data->charge_enabled = true;

	ret = apply_config(dev);
	if (ret != 0) {
		return ret;
	}

	k_work_init_delayable(&data->timer_work, timer_work_handler);
	k_work_schedule(&data->timer_work, TIMER_RESET_PERIOD);

	return 0;
}

#define BQ2415X_INIT(inst)                                                                         \
	static const struct bq2415x_config bq2415x_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.ichg_max_ua = DT_INST_PROP(inst, constant_charge_current_max_microamp),           \
		.vreg_max_uv = DT_INST_PROP(inst, constant_charge_voltage_max_microvolt),          \
		.iterm_ua = DT_INST_PROP(inst, charge_term_current_microamp),                      \
		.ilim_ua = DT_INST_PROP(inst, input_current_limit_microamp),                       \
		.vweak_uv = DT_INST_PROP(inst, ti_weak_battery_microvolt),                         \
		.rsense_uohm = DT_INST_PROP(inst, ti_sense_resistor_micro_ohms),                   \
	};                                                                                         \
	static struct bq2415x_data bq2415x_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, bq2415x_init, NULL, &bq2415x_data_##inst,                      \
			      &bq2415x_config_##inst, POST_KERNEL, CONFIG_CHARGER_INIT_PRIORITY,   \
			      &bq2415x_api);

DT_INST_FOREACH_STATUS_OKAY(BQ2415X_INIT)
