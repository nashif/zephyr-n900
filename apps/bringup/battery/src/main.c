#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/sys/reboot.h>

static const char *const status_str[] = {"unknown", "charging", "discharging",
					 "not charging", "full"};

int main(void)
{
	const struct device *chg = DEVICE_DT_GET(DT_NODELABEL(charger));
	const struct device *fg = DEVICE_DT_GET(DT_NODELABEL(fuel_gauge));

	printk("battery monitor: charger %s, gauge %s\n",
	       device_is_ready(chg) ? "ready" : "NOT READY",
	       device_is_ready(fg) ? "ready" : "NOT READY");

	for (int t = 0;; t += 5) {
		union charger_propval status, ichg;
		union fuel_gauge_prop_val v, i, temp, soc, rem, fcc, tte, ttf;
		int32_t c10;

		charger_get_prop(chg, CHARGER_PROP_STATUS, &status);
		charger_get_prop(chg, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &ichg);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_VOLTAGE_UV, &v);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_CURRENT_UA, &i);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_TEMPERATURE_DK, &temp);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT, &soc);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_REMAINING_CAPACITY_UAH, &rem);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_FULL_CHARGE_CAPACITY_UAH, &fcc);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_RUNTIME_TO_EMPTY_MINS, &tte);
		fuel_gauge_get_prop(fg, FUEL_GAUGE_RUNTIME_TO_FULL_MINS, &ttf);
		c10 = (int32_t)temp.temperature_dk - 2731;

		printk("t=%4ds %-12s ichg=%umA | %u mV %d mA %d.%d C %u%% %u/%u mAh tte=%u ttf=%u\n",
		       t, status_str[MIN(status.status, 4)], ichg.const_charge_current_ua / 1000,
		       v.voltage_uv / 1000, i.current_ua / 1000, c10 / 10, c10 % 10,
		       soc.relative_state_of_charge_pct, rem.remaining_capacity_uah / 1000,
		       fcc.full_charge_capacity_uah / 1000, tte.runtime_to_empty_mins,
		       ttf.runtime_to_full_mins);
		k_sleep(K_SECONDS(5));
		if ((CONFIG_APP_REBOOT_AFTER_S > 0) && (t + 5 >= CONFIG_APP_REBOOT_AFTER_S)) {
			printk("rebooting\n");
			k_sleep(K_SECONDS(2));
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	return 0;
}
