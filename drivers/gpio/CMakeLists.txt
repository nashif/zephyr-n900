# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_GPIO_OMAP3 gpio_omap3.c)
zephyr_library_sources_ifdef(CONFIG_GPIO_TWL4030 gpio_twl4030.c)
