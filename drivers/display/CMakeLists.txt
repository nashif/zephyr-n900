# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_ACX565AKM display_acx565akm.c)
zephyr_library_sources_ifdef(CONFIG_TI_OMAP3_DISPC display_ti_omap3_dispc.c)
# Gap: display_framebuffer.h is private to the in-tree display drivers
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/display)
