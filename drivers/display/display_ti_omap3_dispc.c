/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Display on the graphics layer of the TI OMAP3 display controller. The
 * bootloader configures the panel, its timing and the LCD output; this
 * driver points the graphics layer at a reserved framebuffer in XRGB8888.
 */

#define DT_DRV_COMPAT ti_omap3_dispc

#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>

#include "display_framebuffer.h"

#define DISPC_CONTROL        0x040U
#define DISPC_GFX_BA0        0x080U
#define DISPC_GFX_BA1        0x084U
#define DISPC_GFX_SIZE       0x08CU
#define DISPC_GFX_ATTRIBUTES 0x0A0U
#define DISPC_GFX_ROW_INC    0x0ACU
#define DISPC_GFX_PIXEL_INC  0x0B0U

#define CONTROL_GOLCD    BIT(5)
#define ATTR_ENABLE      BIT(0)
#define ATTR_FORMAT_MASK GENMASK(4, 1)
#define ATTR_FORMAT_XRGB (0x8U << 1)

struct dispc_config {
	struct display_fb_common_config fb;
	mm_reg_t base;
	uintptr_t fb_base;
	size_t fb_size;
	/* Panel controller handling the brightness, or NULL */
	const struct device *panel;
};

static int dispc_set_pixel_format(const struct device *dev, const enum display_pixel_format format)
{
	ARG_UNUSED(dev);

	return (format == PIXEL_FORMAT_ARGB_8888) ? 0 : -ENOTSUP;
}

static int dispc_set_orientation(const struct device *dev,
				 const enum display_orientation orientation)
{
	ARG_UNUSED(dev);

	return (orientation == DISPLAY_ORIENTATION_NORMAL) ? 0 : -ENOTSUP;
}

static void dispc_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	const struct dispc_config *config = dev->config;

	caps->x_resolution = config->fb.width;
	caps->y_resolution = config->fb.height;
	caps->supported_pixel_formats = PIXEL_FORMAT_ARGB_8888;
	caps->screen_info = 0;
	caps->current_pixel_format = PIXEL_FORMAT_ARGB_8888;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static void *dispc_get_framebuffer(const struct device *dev)
{
	struct display_fb_common_data *data = dev->data;

	return (void *)data->fb_addr;
}

static int dispc_blanking(const struct device *dev, bool on)
{
	const struct dispc_config *config = dev->config;
	uint32_t attr = sys_read32(config->base + DISPC_GFX_ATTRIBUTES);

	attr = on ? (attr & ~ATTR_ENABLE) : (attr | ATTR_ENABLE);
	sys_write32(attr, config->base + DISPC_GFX_ATTRIBUTES);
	sys_set_bits(config->base + DISPC_CONTROL, CONTROL_GOLCD);

	return 0;
}

static int dispc_blanking_on(const struct device *dev)
{
	return dispc_blanking(dev, true);
}

static int dispc_blanking_off(const struct device *dev)
{
	return dispc_blanking(dev, false);
}

static int dispc_set_brightness(const struct device *dev, const uint8_t brightness)
{
	const struct dispc_config *config = dev->config;

	if (config->panel == NULL) {
		return -ENOSYS;
	}

	if (!device_is_ready(config->panel)) {
		return -ENODEV;
	}

	return display_set_brightness(config->panel, brightness);
}

static DEVICE_API(display, dispc_api) = {
	.blanking_on = dispc_blanking_on,
	.blanking_off = dispc_blanking_off,
	.set_brightness = dispc_set_brightness,
	.read = display_fb_read,
	.write = display_fb_write,
	.get_framebuffer = dispc_get_framebuffer,
	.get_capabilities = dispc_get_capabilities,
	.set_pixel_format = dispc_set_pixel_format,
	.set_orientation = dispc_set_orientation,
};

static int dispc_init(const struct device *dev)
{
	const struct dispc_config *config = dev->config;
	struct display_fb_common_data *data = dev->data;
	size_t fb_bytes = (size_t)config->fb.width * config->fb.height * DISPLAY_FB_BYTES_PER_PIXEL;
	uint32_t attr;

	if (fb_bytes > config->fb_size) {
		return -ENOMEM;
	}

	data->fb_addr = config->fb_base;
	data->pitch = config->fb.width;
	memset((void *)data->fb_addr, 0, fb_bytes);

	sys_write32(config->fb_base, config->base + DISPC_GFX_BA0);
	sys_write32(config->fb_base, config->base + DISPC_GFX_BA1);
	sys_write32(1U, config->base + DISPC_GFX_ROW_INC);
	sys_write32(1U, config->base + DISPC_GFX_PIXEL_INC);
	sys_write32(((config->fb.height - 1U) << 16) | (config->fb.width - 1U),
		    config->base + DISPC_GFX_SIZE);

	attr = sys_read32(config->base + DISPC_GFX_ATTRIBUTES);
	attr = (attr & ~ATTR_FORMAT_MASK) | ATTR_FORMAT_XRGB | ATTR_ENABLE;
	sys_write32(attr, config->base + DISPC_GFX_ATTRIBUTES);
	sys_set_bits(config->base + DISPC_CONTROL, CONTROL_GOLCD);

	return 0;
}

#define DISPC_INIT(inst)                                                                           \
	static const struct dispc_config dispc_config_##inst = {                                   \
		.fb = DISPLAY_FB_COMMON_CFG_FROM_DT_INST(inst),                                    \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.fb_base = DT_REG_ADDR(DT_INST_PHANDLE(inst, memory_region)),                      \
		.fb_size = DT_REG_SIZE(DT_INST_PHANDLE(inst, memory_region)),                      \
		.panel = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, display_controller)),         \
	};                                                                                         \
	static struct display_fb_common_data dispc_data_##inst;                                    \
	DEVICE_DT_INST_DEFINE(inst, dispc_init, NULL, &dispc_data_##inst, &dispc_config_##inst,    \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &dispc_api);

DT_INST_FOREACH_STATUS_OKAY(DISPC_INIT)
