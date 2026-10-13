/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

KEEP(*(.omap3_entry))
/* VBAR ignores bits [4:0]: the vector table that follows must be aligned */
. = ALIGN(32);
z_omap3_image_end = LOADADDR(.last_section) + SIZEOF(.last_section);
