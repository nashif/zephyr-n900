# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_INPUT_TSC2005 input_tsc2005.c)
zephyr_library_sources_ifdef(CONFIG_INPUT_TWL4030_KEYPAD input_twl4030_keypad.c)
zephyr_library_sources_ifdef(CONFIG_INPUT_TWL4030_PWRBUTTON input_twl4030_pwrbutton.c)
