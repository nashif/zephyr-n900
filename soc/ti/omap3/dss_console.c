/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Text console on the LCD. The bootloader leaves the display controller
 * scanning out its graphics layer; this console keeps that timing and
 * geometry, points the layer at an RGB16 framebuffer in the display
 * controller's memory region and draws
 * characters with the character framebuffer fonts.
 *
 * When the memory region holds two copies of the text area, each character
 * goes into both, and the graphics layer shows a window that starts in the
 * first copy: scrolling moves the start of the window by one line instead of
 * moving the text.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/display/cfb.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#define DISPC_BASE DT_REG_ADDR(DT_NODELABEL(dispc))

#define DISPC_CONTROL        (DISPC_BASE + 0x040U)
#define DISPC_GFX_BA0        (DISPC_BASE + 0x080U)
#define DISPC_GFX_BA1        (DISPC_BASE + 0x084U)
#define DISPC_GFX_SIZE       (DISPC_BASE + 0x08CU)
#define DISPC_GFX_ATTRIBUTES (DISPC_BASE + 0x0A0U)
#define DISPC_GFX_ROW_INC    (DISPC_BASE + 0x0ACU)
#define DISPC_GFX_PIXEL_INC  (DISPC_BASE + 0x0B0U)

#define CONTROL_GOLCD     BIT(5)
#define ATTR_ENABLE       BIT(0)
#define ATTR_FORMAT_SHIFT 1U
#define ATTR_FORMAT_MASK  (0xFU << ATTR_FORMAT_SHIFT)
#define FORMAT_RGB16      0x6U

#define FB_BASE DT_REG_ADDR(DT_PHANDLE(DT_NODELABEL(dispc), memory_region))
#define FB_SIZE DT_REG_SIZE(DT_PHANDLE(DT_NODELABEL(dispc), memory_region))

/* RGB565 */
#define COLOR_FG 0xFFFFU
#define COLOR_BG 0x0110U

static uint16_t *const framebuffer = (uint16_t *)FB_BASE;

static struct {
	const struct cfb_font *font;
	uint32_t width;
	uint32_t height;
	uint32_t cols;
	uint32_t rows;
	uint32_t col;
	uint32_t row;
	/* Text row at the top of the window, with two copies of the text area */
	uint32_t top;
	bool ring;
} con;

static struct k_spinlock lock;

static void fill_lines(uint32_t y, uint32_t count)
{
	uint16_t *p = &framebuffer[y * con.width];

	for (uint32_t i = 0U; i < count * con.width; i++) {
		p[i] = COLOR_BG;
	}
}

/* Text row in the framebuffer of the row on the screen */
static uint32_t ring_row(uint32_t row)
{
	return con.ring ? ((con.top + row) % con.rows) : row;
}

static void draw_glyph_at(uint8_t c, uint32_t col, uint32_t row)
{
	const struct cfb_font *font = con.font;
	uint32_t bytes_per_col = DIV_ROUND_UP(font->height, 8U);
	bool msb_first = (font->caps & CFB_FONT_MSB_FIRST) != 0U;
	uint16_t *origin = &framebuffer[(row * font->height * con.width) + (col * font->width)];
	const uint8_t *glyph;

	if ((c < font->first_char) || (c > font->last_char)) {
		c = '?';
	}

	glyph = (const uint8_t *)font->data +
		((c - font->first_char) * font->width * bytes_per_col);

	for (uint32_t x = 0U; x < font->width; x++) {
		for (uint32_t y = 0U; y < font->height; y++) {
			uint8_t bits = glyph[(x * bytes_per_col) + (y / 8U)];
			uint32_t bit = msb_first ? (7U - (y % 8U)) : (y % 8U);

			origin[(y * con.width) + x] =
				((bits & BIT(bit)) != 0U) ? COLOR_FG : COLOR_BG;
		}
	}
}

static void draw_glyph(uint8_t c, uint32_t col, uint32_t row)
{
	uint32_t r = ring_row(row);

	draw_glyph_at(c, col, r);
	if (con.ring) {
		draw_glyph_at(c, col, r + con.rows);
	}
}

static void show_from(uint32_t line)
{
	uint32_t ba = (uint32_t)&framebuffer[line * con.width];

	sys_write32(ba, DISPC_GFX_BA0);
	sys_write32(ba, DISPC_GFX_BA1);
	/* The new address takes effect at the next frame */
	sys_set_bits(DISPC_CONTROL, CONTROL_GOLCD);
}

static void newline(void)
{
	uint32_t line_pixels = con.width * con.font->height;

	con.col = 0U;
	if (con.row + 1U < con.rows) {
		con.row++;
		return;
	}

	if (con.ring) {
		uint32_t r;

		con.top = (con.top + 1U) % con.rows;
		r = ring_row(con.row);
		fill_lines(r * con.font->height, con.font->height);
		fill_lines((r + con.rows) * con.font->height, con.font->height);
		show_from(con.top * con.font->height);
		return;
	}

	memmove(framebuffer, &framebuffer[line_pixels],
		line_pixels * (con.rows - 1U) * sizeof(framebuffer[0]));
	fill_lines(con.row * con.font->height, con.font->height);
}

static int console_out(int c)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (c == '\n') {
		newline();
	} else if (c == '\r') {
		con.col = 0U;
	} else if (c == '\b') {
		if (con.col > 0U) {
			con.col--;
			draw_glyph(' ', con.col, con.row);
		}
	} else {
		if (con.col >= con.cols) {
			newline();
		}
		draw_glyph((uint8_t)c, con.col, con.row);
		con.col++;
	}

	k_spin_unlock(&lock, key);

	return c;
}

static const struct cfb_font *font_select(void)
{
	const struct cfb_font *best = NULL;

	/* The vertically packed font closest to 16 pixels tall */
	STRUCT_SECTION_FOREACH(cfb_font, font) {
		if ((font->caps & CFB_FONT_MONO_VPACKED) == 0U) {
			continue;
		}
		if ((best == NULL) || (abs((int)font->height - 16) < abs((int)best->height - 16))) {
			best = font;
		}
	}

	return best;
}

void omap3_dss_console_init(void)
{
	uint32_t size = sys_read32(DISPC_GFX_SIZE);
	uint32_t attr = sys_read32(DISPC_GFX_ATTRIBUTES);
	uint32_t ring_lines;

	con.font = font_select();
	if (con.font == NULL) {
		return;
	}

	con.width = (size & 0x7FFU) + 1U;
	con.height = MIN(((size >> 16) & 0x7FFU) + 1U, FB_SIZE / (con.width * sizeof(uint16_t)));
	con.cols = con.width / con.font->width;
	con.rows = con.height / con.font->height;

	/*
	 * With two copies of the text area the layer is exactly as tall as the
	 * text rows; the display controller fills the lines below with its
	 * background color.
	 */
	ring_lines = 2U * con.rows * con.font->height;
	con.ring = (ring_lines * con.width * sizeof(uint16_t)) <= FB_SIZE;
	if (con.ring) {
		con.height = con.rows * con.font->height;
	}

	fill_lines(0U, con.ring ? ring_lines : con.height);

	sys_write32((uint32_t)framebuffer, DISPC_GFX_BA0);
	sys_write32((uint32_t)framebuffer, DISPC_GFX_BA1);
	sys_write32(1U, DISPC_GFX_ROW_INC);
	sys_write32(1U, DISPC_GFX_PIXEL_INC);
	sys_write32(((con.height - 1U) << 16) | (con.width - 1U), DISPC_GFX_SIZE);
	attr = (attr & ~ATTR_FORMAT_MASK) | (FORMAT_RGB16 << ATTR_FORMAT_SHIFT) | ATTR_ENABLE;
	sys_write32(attr, DISPC_GFX_ATTRIBUTES);
	sys_set_bits(DISPC_CONTROL, CONTROL_GOLCD);

	__printk_hook_install(console_out);
	__stdout_hook_install(console_out);
}
