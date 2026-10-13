# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_UDC_OMAP3_MUSB udc_omap3_musb.c)
# Gap: udc_common.h, the base of every UDC driver, is private to drivers/usb/udc
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
