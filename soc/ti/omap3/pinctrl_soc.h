/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_TI_OMAP3_PINCTRL_SOC_H_
#define ZEPHYR_SOC_TI_OMAP3_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pinctrl_soc_pin {
	const struct device *dev;
	uint16_t offset;
	uint16_t value;
};

typedef struct pinctrl_soc_pin pinctrl_soc_pin_t;

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	{                                                                                          \
		.dev = DEVICE_DT_GET(DT_PARENT(DT_PROP_BY_IDX(node_id, prop, idx))),               \
		.offset = DT_PROP_BY_IDX(DT_PROP_BY_IDX(node_id, prop, idx), pinmux, 0),           \
		.value = DT_PROP_BY_IDX(DT_PROP_BY_IDX(node_id, prop, idx), pinmux, 1),            \
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_STATE_PIN_INIT)}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SOC_TI_OMAP3_PINCTRL_SOC_H_ */
