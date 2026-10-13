/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/mfd/twl4030.h>
#include <zephyr/kernel.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>

/* TWL5030 power management receiver: watchdog timeout in seconds, 0 stops it */
#define TWL_WATCHDOG_CFG 0x5EU
/* VMMC2 regulator: group P1 turns it on; voltage selection 2.8 V */
#define TWL_VMMC2_DEV_GRP   0x86U
#define TWL_VMMC2_DEDICATED 0x89U
#define TWL_DEV_GRP_P1      0x20U
#define TWL_VMMC2_2V8       0x09U

/*
 * The watchdog of the TWL5030 power management chip switches the phone off
 * when it expires and Zephyr does not service it. Stop it, whatever NOLO
 * left it at.
 */
static void twl_watchdog_stop(void)
{
#if defined(CONFIG_MFD_TWL4030)
	const struct device *twl = DEVICE_DT_GET(DT_NODELABEL(twl));
	int ret = -ENODEV;

	if (device_is_ready(twl)) {
		ret = mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, TWL_WATCHDOG_CFG, 0U);
	}

	if (ret != 0) {
		printk("n900: failed to stop the TWL5030 watchdog (%d)\n", ret);
	}
#endif
}

/*
 * VMMC2 supplies the audio codec and the headphone amplifier, whose digital
 * supplies leak into it when it is off. NOLO leaves it off.
 */
static void twl_vmmc2_on(void)
{
#if defined(CONFIG_MFD_TWL4030)
	const struct device *twl = DEVICE_DT_GET(DT_NODELABEL(twl));
	int ret = -ENODEV;

	if (device_is_ready(twl)) {
		ret = mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, TWL_VMMC2_DEDICATED,
					    TWL_VMMC2_2V8);
	}
	if (ret == 0) {
		ret = mfd_twl4030_reg_write(twl, TWL4030_ADDR_PM_MASTER, TWL_VMMC2_DEV_GRP,
					    TWL_DEV_GRP_P1);
	}

	if (ret != 0) {
		printk("n900: failed to turn on VMMC2 (%d)\n", ret);
	}
#endif
}

#if defined(CONFIG_BOARD_N900_CHARGE_TEMP_GUARD)

#define TEMP_GUARD_PERIOD K_SECONDS(10)

/* Degrees Celsius to decikelvin */
#define C_TO_DK(c) (2731 + ((c) * 10))

static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(charger));
static const struct device *const gauge = DEVICE_DT_GET(DT_NODELABEL(fuel_gauge));
static bool charging_allowed = true;

static void temp_guard_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(temp_guard_work, temp_guard_handler);

/*
 * The bq24150A has no temperature input on the N900: stop charging outside
 * the temperature window, or when the battery temperature is unknown.
 */
static void temp_guard_handler(struct k_work *work)
{
	union fuel_gauge_prop_val val;
	int32_t dk = -1;
	bool allow;

	if (fuel_gauge_get_prop(gauge, FUEL_GAUGE_TEMPERATURE_DK, &val) == 0) {
		dk = val.temperature_dk;
	}

	if (dk < 0) {
		allow = false;
	} else if (charging_allowed) {
		allow = (dk > C_TO_DK(CONFIG_BOARD_N900_CHARGE_TEMP_MIN)) &&
			(dk < C_TO_DK(CONFIG_BOARD_N900_CHARGE_TEMP_MAX));
	} else {
		allow = (dk >= C_TO_DK(CONFIG_BOARD_N900_CHARGE_TEMP_MIN +
				       CONFIG_BOARD_N900_CHARGE_TEMP_HYSTERESIS)) &&
			(dk <= C_TO_DK(CONFIG_BOARD_N900_CHARGE_TEMP_MAX -
				       CONFIG_BOARD_N900_CHARGE_TEMP_HYSTERESIS));
	}

	if (allow != charging_allowed) {
		if (charger_charge_enable(charger, allow) == 0) {
			charging_allowed = allow;
			if (dk < 0) {
				printk("n900: battery temperature unknown, charging stopped\n");
			} else {
				int32_t c10 = dk - 2731;

				printk("n900: battery at %d.%d C, charging %s\n", c10 / 10,
				       abs(c10 % 10), allow ? "resumed" : "stopped");
			}
		}
	}

	k_work_reschedule(k_work_delayable_from_work(work), TEMP_GUARD_PERIOD);
}

static void temp_guard_start(void)
{
	if (!device_is_ready(charger)) {
		return;
	}

	/* Without a readable temperature, charging stops at the first check */
	k_work_schedule(&temp_guard_work, K_NO_WAIT);
}

#else

static void temp_guard_start(void)
{
}

#endif /* CONFIG_BOARD_N900_CHARGE_TEMP_GUARD */

#if defined(CONFIG_BOARD_N900_POWER_KEY_REBOOT)

static void power_key_reboot(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("n900: power button held, rebooting\n");
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(power_key_work, power_key_reboot);

static void power_key_handler(const struct device *dev, struct twl4030_irq_callback *cb,
			      uint8_t status)
{
	uint8_t sts = 0U;

	ARG_UNUSED(cb);
	ARG_UNUSED(status);

	if ((mfd_twl4030_reg_read(dev, TWL4030_ADDR_PM_MASTER, TWL4030_STS_HW_CONDITIONS,
				  &sts) == 0) &&
	    ((sts & TWL4030_STS_PWON) != 0U)) {
		k_work_reschedule(&power_key_work,
				  K_MSEC(CONFIG_BOARD_N900_POWER_KEY_REBOOT_HOLD_MS));
	} else {
		k_work_cancel_delayable(&power_key_work);
	}
}

static struct twl4030_irq_callback power_key_cb = {
	.handler = power_key_handler,
	.module = TWL4030_IRQ_POWER,
	.mask = TWL4030_POWER_IRQ_PWRON,
};

static void power_key_reboot_start(void)
{
	const struct device *twl = DEVICE_DT_GET(DT_NODELABEL(twl));
	int ret = -ENODEV;

	if (device_is_ready(twl)) {
		ret = mfd_twl4030_add_irq_callback(twl, &power_key_cb);
	}

	if (ret != 0) {
		printk("n900: power button reboot unavailable (%d)\n", ret);
	}
}

#else

static void power_key_reboot_start(void)
{
}

#endif /* CONFIG_BOARD_N900_POWER_KEY_REBOOT */

void board_late_init_hook(void)
{
	twl_watchdog_stop();
	twl_vmmc2_on();
	temp_guard_start();
	power_key_reboot_start();
}
