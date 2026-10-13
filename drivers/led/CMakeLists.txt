# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_ADP1653 adp1653.c)
zephyr_library_sources_ifdef(CONFIG_LP5523 lp5523.c)
