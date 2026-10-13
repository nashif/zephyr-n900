/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_TWL4030_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_TWL4030_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup mfd_interface_twl4030 MFD TWL4030 Interface
 * @ingroup mfd_interfaces
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

/**
 * @brief Interrupt modules of the chip
 *
 * The value of each module is its bit in the primary interrupt status.
 */
enum twl4030_irq_module {
	TWL4030_IRQ_GPIO = 0,
	TWL4030_IRQ_KEYPAD = 1,
	TWL4030_IRQ_BCI = 2,
	TWL4030_IRQ_MADC = 3,
	TWL4030_IRQ_USB = 4,
	TWL4030_IRQ_POWER = 5,
};

/** Keypad module interrupt: key state change */
#define TWL4030_KEYPAD_IRQ_KP      BIT(0)
/** Power module interrupt: power button */
#define TWL4030_POWER_IRQ_PWRON    BIT(0)
/** Power module interrupt: USB VBUS or ID change */
#define TWL4030_POWER_IRQ_USB_PRES BIT(2)

/** Index of the chip's I2C address that holds the power master registers */
#define TWL4030_ADDR_PM_MASTER    3U
/** Hardware conditions register, in the power master registers */
#define TWL4030_STS_HW_CONDITIONS 0x45U
/** Hardware condition: power button pressed */
#define TWL4030_STS_PWON          BIT(0)
/** Hardware condition: USB ID grounded */
#define TWL4030_STS_USB_ID        BIT(2)
/** Hardware condition: USB VBUS present */
#define TWL4030_STS_VBUS          BIT(7)

struct twl4030_irq_callback;

/**
 * @brief Interrupt callback
 *
 * Called from the system work queue.
 *
 * @param dev TWL4030 MFD device
 * @param cb Registered callback
 * @param status Pending interrupts of the module, limited to the callback's mask
 */
typedef void (*twl4030_irq_handler_t)(const struct device *dev, struct twl4030_irq_callback *cb,
				      uint8_t status);

/** @brief Interrupt callback registration */
struct twl4030_irq_callback {
	/** @cond INTERNAL_HIDDEN */
	sys_snode_t node;
	/** @endcond */
	/** Function called when an interrupt in @ref mask is pending */
	twl4030_irq_handler_t handler;
	/** Module the interrupts belong to: keypad or power */
	enum twl4030_irq_module module;
	/** Interrupts of the module, enabled on both edges */
	uint8_t mask;
};

/**
 * @brief Read a register
 *
 * @param dev TWL4030 MFD device
 * @param addr_idx Index of the chip's I2C address, 0 to 3
 * @param reg Register address
 * @param val Read value
 * @retval 0 Success
 * @retval -EIO Bus error
 */
int mfd_twl4030_reg_read(const struct device *dev, uint8_t addr_idx, uint8_t reg, uint8_t *val);

/**
 * @brief Read consecutive registers
 *
 * @param dev TWL4030 MFD device
 * @param addr_idx Index of the chip's I2C address, 0 to 3
 * @param reg Address of the first register
 * @param buf Buffer for the read values
 * @param len Number of registers
 * @retval 0 Success
 * @retval -EIO Bus error
 */
int mfd_twl4030_reg_read_burst(const struct device *dev, uint8_t addr_idx, uint8_t reg, void *buf,
			       size_t len);

/**
 * @brief Write a register
 *
 * @param dev TWL4030 MFD device
 * @param addr_idx Index of the chip's I2C address, 0 to 3
 * @param reg Register address
 * @param val Value to write
 * @retval 0 Success
 * @retval -EIO Bus error
 */
int mfd_twl4030_reg_write(const struct device *dev, uint8_t addr_idx, uint8_t reg, uint8_t val);

/**
 * @brief Register an interrupt callback and enable its interrupts
 *
 * The interrupts in the callback's mask are enabled on both edges.
 *
 * @param dev TWL4030 MFD device
 * @param cb Callback to register, which must stay valid while registered
 * @retval 0 Success
 * @retval -ENOTSUP The device has no interrupt line, or the module is not supported
 * @retval -EIO Bus error
 */
int mfd_twl4030_add_irq_callback(const struct device *dev, struct twl4030_irq_callback *cb);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_TWL4030_H_ */
