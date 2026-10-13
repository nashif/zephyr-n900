# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_I2C_OMAP3 i2c_omap3.c)
# Gap: i2c-priv.h is private to the in-tree I2C drivers
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/i2c)
