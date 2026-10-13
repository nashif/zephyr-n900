/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Boot progress on the display for bring-up without a serial port. Before
 * the MMU is on, stage N paints band N of the bootloader's framebuffer.
 * Afterwards only the display controller registers are mapped: a late
 * stage hides the graphics layer and shows a solid background color.
 *
 * omap3_boot_stage() also runs from the entry stub before the image is at
 * its link address, so it uses no data and no absolute code addresses.
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#define DISPC_CONTROL        0x48050440U
#define DISPC_DEFAULT_COLOR0 0x4805044CU
#define DISPC_GFX_BA0        0x48050480U
#define DISPC_GFX_SIZE       0x4805048CU
#define DISPC_GFX_ATTRIBUTES 0x480504A0U
#define DISPC_GFX_ROW_INC    0x480504ACU

#define CONTROL_GOLCD BIT(5)
#define ATTR_ENABLE   BIT(0)
#define FORMAT_RGB16  0x6U
#define NUM_BANDS     8U

void omap3_boot_stage(uint32_t stage, uint32_t rgb)
{
	uint32_t size = sys_read32(DISPC_GFX_SIZE);
	uint32_t format = (sys_read32(DISPC_GFX_ATTRIBUTES) >> 1) & 0xFU;
	uint32_t width = (size & 0x7FFU) + 1U;
	uint32_t height = ((size >> 16) & 0x7FFU) + 1U;
	uint32_t bpp = (format == FORMAT_RGB16) ? 2U : 4U;
	uint32_t stride = (width * bpp) + sys_read32(DISPC_GFX_ROW_INC) - 1U;
	uint32_t band = height / NUM_BANDS;
	uintptr_t line = sys_read32(DISPC_GFX_BA0) + ((stage % NUM_BANDS) * band * stride);
	uint16_t rgb16 = ((rgb >> 8) & 0xF800U) | ((rgb >> 5) & 0x07E0U) | ((rgb >> 3) & 0x001FU);

	for (uint32_t y = 0U; y < band; y++, line += stride) {
		for (uint32_t x = 0U; x < width; x++) {
			if (bpp == 2U) {
				((volatile uint16_t *)line)[x] = rgb16;
			} else {
				((volatile uint32_t *)line)[x] = rgb;
			}
		}
	}
}

void omap3_boot_stage_late(uint32_t rgb)
{
	sys_write32(rgb, DISPC_DEFAULT_COLOR0);
	sys_write32(sys_read32(DISPC_GFX_ATTRIBUTES) & ~ATTR_ENABLE, DISPC_GFX_ATTRIBUTES);
	sys_write32(sys_read32(DISPC_CONTROL) | CONTROL_GOLCD, DISPC_CONTROL);
}
