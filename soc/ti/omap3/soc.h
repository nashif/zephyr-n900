/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_TI_OMAP3_SOC_H_
#define ZEPHYR_SOC_TI_OMAP3_SOC_H_

#ifndef _ASMLANGUAGE

#include <stdbool.h>
#include <stdint.h>

/* Required by the CMSIS Cortex-A core header */
#define __CORTEX_A 8U

/* Address the image was loaded at, recorded by the entry stub */
extern uint32_t omap3_load_addr;

/* SCTLR as left by the bootloader, recorded by soc_reset_hook() */
extern uint32_t omap3_boot_sctlr;

void omap3_dss_console_init(void);

void omap3_boot_stage(uint32_t stage, uint32_t rgb);
void omap3_boot_stage_late(uint32_t rgb);

#if defined(CONFIG_SOC_OMAP3_BOOT_STAGES)
#define OMAP3_BOOT_STAGE(stage, rgb) omap3_boot_stage(stage, rgb)
#define OMAP3_BOOT_STAGE_LATE(rgb)   omap3_boot_stage_late(rgb)
#else
#define OMAP3_BOOT_STAGE(stage, rgb) do { } while (false)
#define OMAP3_BOOT_STAGE_LATE(rgb)   do { } while (false)
#endif

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_SOC_TI_OMAP3_SOC_H_ */
