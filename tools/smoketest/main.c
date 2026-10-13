#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* OMAP3 WDT2 (MPU watchdog) */
#define WDT2_BASE		0x48314000U
#define WDT_WWPS		(WDT2_BASE + 0x34U)
#define WDT_WSPR		(WDT2_BASE + 0x48U)

/* OMAP3 DISPC */
#define DISPC_BASE		0x48050400U
#define DISPC_CONTROL		(DISPC_BASE + 0x040U)
#define DISPC_DEFAULT_COLOR0	(DISPC_BASE + 0x04CU)
#define DISPC_GFX_BA0		(DISPC_BASE + 0x080U)
#define DISPC_GFX_SIZE		(DISPC_BASE + 0x08CU)
#define DISPC_GFX_ATTRIBUTES	(DISPC_BASE + 0x0A0U)
#define DISPC_GFX_ROW_INC	(DISPC_BASE + 0x0ACU)

#define CONTROL_GOLCD		(1U << 5)

static void wdt2_disable(void)
{
	REG(WDT_WSPR) = 0xAAAAU;
	while (REG(WDT_WWPS) != 0U) {
	}
	REG(WDT_WSPR) = 0x5555U;
	while (REG(WDT_WWPS) != 0U) {
	}
}

static void delay(void)
{
	for (volatile uint32_t i = 0U; i < 60000000U; i++) {
	}
}

/* rgb is 0x00RRGGBB */
static void fill(uint32_t rgb)
{
	uint32_t attr = REG(DISPC_GFX_ATTRIBUTES);
	uint32_t fmt = (attr >> 1) & 0xFU;
	uint32_t size = REG(DISPC_GFX_SIZE);
	uint32_t w = (size & 0x7FFU) + 1U;
	uint32_t h = ((size >> 16) & 0x7FFU) + 1U;
	uintptr_t row = REG(DISPC_GFX_BA0);
	uint32_t bpp = (fmt == 0x6U) ? 2U : 4U;
	uint32_t stride = w * bpp + REG(DISPC_GFX_ROW_INC) - 1U;

	/* Background color, visible where no layer covers the panel */
	REG(DISPC_DEFAULT_COLOR0) = rgb;
	REG(DISPC_CONTROL) |= CONTROL_GOLCD;

	if (((attr & 1U) == 0U) || (row == 0U)) {
		return;
	}

	for (uint32_t y = 0U; y < h; y++) {
		if (bpp == 2U) {
			uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800U) |
						((rgb >> 5) & 0x07E0U) |
						((rgb >> 3) & 0x001FU));
			volatile uint16_t *p = (volatile uint16_t *)row;

			for (uint32_t x = 0U; x < w; x++) {
				p[x] = c;
			}
		} else {
			volatile uint32_t *p = (volatile uint32_t *)row;

			for (uint32_t x = 0U; x < w; x++) {
				p[x] = rgb;
			}
		}
		row += stride;
	}
}

void smoke_main(uintptr_t load_addr)
{
	(void)load_addr;
	wdt2_disable();

	for (;;) {
		for (uint32_t i = 0U; i < 4U; i++) {
			/* red, green, blue, white; no table, so no absolute addresses */
			fill((i < 3U) ? (0xFF0000U >> (8U * i)) : 0xFFFFFFU);
			delay();
		}
	}
}
