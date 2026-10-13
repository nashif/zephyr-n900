/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Device ID and reset cause of the TI OMAP3 SoCs: the 128-bit die ID of the
 * control module and the reset status of the power and reset manager.
 */

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* Control module, die ID words 0 to 3 */
#define CONTROL_DIE_ID(n) (0x4830A218U + ((n) * 4U))
#define DIE_ID_WORDS      4U

/* Power and reset manager, global reset status: write 1 to clear */
#define PRM_RSTST 0x48307258U

#define RSTST_GLOBAL_COLD_RST   BIT(0)
#define RSTST_GLOBAL_SW_RST     BIT(1)
#define RSTST_MPU_WD_RST        BIT(3)
#define RSTST_SECURE_WD_RST     BIT(4)
#define RSTST_EXTERNAL_WARM_RST BIT(5)
#define RSTST_VDD1_VOLTAGE_MGR  BIT(6)
#define RSTST_VDD2_VOLTAGE_MGR  BIT(7)
#define RSTST_ICEPICK_RST       BIT(9)
#define RSTST_ICECRUSHER_RST    BIT(10)

static const struct {
	uint32_t rstst;
	uint32_t cause;
} reset_map[] = {
	{RSTST_GLOBAL_COLD_RST, RESET_POR},       {RSTST_GLOBAL_SW_RST, RESET_SOFTWARE},
	{RSTST_MPU_WD_RST, RESET_WATCHDOG},       {RSTST_SECURE_WD_RST, RESET_WATCHDOG},
	{RSTST_EXTERNAL_WARM_RST, RESET_PIN},     {RSTST_VDD1_VOLTAGE_MGR, RESET_HARDWARE},
	{RSTST_VDD2_VOLTAGE_MGR, RESET_HARDWARE}, {RSTST_ICEPICK_RST, RESET_DEBUG},
	{RSTST_ICECRUSHER_RST, RESET_DEBUG},
};

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	uint8_t id[DIE_ID_WORDS * sizeof(uint32_t)];

	/* Most significant word first */
	for (size_t i = 0U; i < DIE_ID_WORDS; i++) {
		sys_put_be32(sys_read32(CONTROL_DIE_ID(DIE_ID_WORDS - 1U - i)),
			     &id[i * sizeof(uint32_t)]);
	}

	length = MIN(length, sizeof(id));
	memcpy(buffer, id, length);

	return length;
}

int z_impl_hwinfo_get_reset_cause(uint32_t *cause)
{
	uint32_t rstst = sys_read32(PRM_RSTST);

	*cause = 0U;
	for (size_t i = 0U; i < ARRAY_SIZE(reset_map); i++) {
		if ((rstst & reset_map[i].rstst) != 0U) {
			*cause |= reset_map[i].cause;
		}
	}

	return 0;
}

int z_impl_hwinfo_clear_reset_cause(void)
{
	sys_write32(sys_read32(PRM_RSTST), PRM_RSTST);

	return 0;
}

int z_impl_hwinfo_get_supported_reset_cause(uint32_t *supported)
{
	*supported = 0U;
	for (size_t i = 0U; i < ARRAY_SIZE(reset_map); i++) {
		*supported |= reset_map[i].cause;
	}

	return 0;
}
