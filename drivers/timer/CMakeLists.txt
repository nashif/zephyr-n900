# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_TI_OMAP_GPTIMER ti_omap_gptimer.c)
# Gap: system_timer_generic.h, the generic timer core, is private to drivers/timer
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/timer)
