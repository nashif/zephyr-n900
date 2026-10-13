/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TI_OMAP3_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TI_OMAP3_PINCTRL_H_

#define OMAP3_PULL_ENABLE  (1 << 3)
#define OMAP3_PULL_UP      (1 << 4)
#define OMAP3_INPUT_ENABLE (1 << 8)

#define PIN_OUTPUT          0
#define PIN_OUTPUT_PULLUP   (OMAP3_PULL_ENABLE | OMAP3_PULL_UP)
#define PIN_OUTPUT_PULLDOWN OMAP3_PULL_ENABLE
#define PIN_INPUT           OMAP3_INPUT_ENABLE
#define PIN_INPUT_PULLUP    (OMAP3_INPUT_ENABLE | OMAP3_PULL_ENABLE | OMAP3_PULL_UP)
#define PIN_INPUT_PULLDOWN  (OMAP3_INPUT_ENABLE | OMAP3_PULL_ENABLE)

#define MUX_MODE_0 0
#define MUX_MODE_1 1
#define MUX_MODE_2 2
#define MUX_MODE_3 3
#define MUX_MODE_4 4
#define MUX_MODE_5 5
#define MUX_MODE_6 6
#define MUX_MODE_7 7

/* Core pad registers start at 0x2030 in the system control module */
#define OMAP3_CORE_PADCONF_BASE 0x2030

#define OMAP3_PINMUX(pad, value, mux_mode)                                                         \
	((pad) - OMAP3_CORE_PADCONF_BASE) ((value) | (mux_mode))

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TI_OMAP3_PINCTRL_H_ */
