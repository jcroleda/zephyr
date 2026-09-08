/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_MAX20356_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_MAX20356_H_

/**
 * @defgroup mfd_interface_max20356 MFD MAX20356 Interface
 * @ingroup mfd_interfaces
 * @{
 */

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Device variant.
 *
 * Selected from the devicetree compatible string at build time; there is no
 * runtime part-ID register (RevID reports silicon revision only).
 */
enum max20356_variant {
	/** MAX20356 */
	MAX20356_VARIANT_MAX20356,
	/** MAX20358 (adds the LDO4Cfg.LDO4RTC feature) */
	MAX20356_VARIANT_MAX20358,
};

/**
 * @brief Power-mode / reset commands issued through PwrCmd (0x83).
 */
enum max20356_power_cmd {
	/** Turn the PMIC off (PwrCmd 0xB2) */
	MAX20356_PWR_OFF,
	/** Hard reset (PwrCmd 0xC3) */
	MAX20356_PWR_HARD_RESET,
	/** Soft reset (PwrCmd 0xD4) */
	MAX20356_PWR_SOFT_RESET,
	/** Seal the device (PwrCmd 0xE5) */
	MAX20356_PWR_SEAL,
};

/**
 * @brief Watchdog reset-type action (WDCntl.WDRstType).
 *
 * Fallback for behavior the standard wdt subsystem API cannot express, in
 * particular the charger/limiter-only reset action.
 */
enum max20356_wdt_rsttype {
	/** Watchdog off (WDRstType 0b00) */
	MAX20356_WDT_OFF = 0x0,
	/** Charger + limiter register reset (WDRstType 0b01) */
	MAX20356_WDT_CHG_LIM_RST = 0x1,
	/** Soft reset (WDRstType 0b10) */
	MAX20356_WDT_SOFT_RESET = 0x2,
	/** Hard reset (WDRstType 0b11) */
	MAX20356_WDT_HARD_RESET = 0x3,
};

/**
 * @brief Password-protected lock domains.
 *
 * Each domain corresponds to one bit of a LockMsk register and its matching
 * LockUnlock password register. mfd_max20356_reg_update_locked() unmasks a single
 * domain, unlocks it, performs the write and re-locks. Domains BUCK1..LDO4 live in
 * LockMsk1/LockUnlock1; CHG/LIM/WD live in LockMsk3/LockUnlock3.
 */
enum max20356_lock_domain {
	/** Buck1 registers (LockMsk1.Bk1Lck) */
	MAX20356_LOCK_BUCK1,
	/** Buck2 registers (LockMsk1.Bk2Lck) */
	MAX20356_LOCK_BUCK2,
	/** Buck3 registers (LockMsk1.Bk3Lck) */
	MAX20356_LOCK_BUCK3,
	/** Buck-boost registers (LockMsk1.BbLck) */
	MAX20356_LOCK_BBST,
	/** LDO1 registers (LockMsk1.Ld1Lck) */
	MAX20356_LOCK_LDO1,
	/** LDO2 registers (LockMsk1.Ld2Lck) */
	MAX20356_LOCK_LDO2,
	/** LDO3 registers (LockMsk1.Ld3Lck) */
	MAX20356_LOCK_LDO3,
	/** LDO4 registers (LockMsk1.Ld4Lck) */
	MAX20356_LOCK_LDO4,
	/** Charger registers (LockMsk3.ChgLck) */
	MAX20356_LOCK_CHG,
	/** Input-limiter registers (LockMsk3.LimLck) */
	MAX20356_LOCK_LIM,
	/** Watchdog registers (LockMsk3.WdLck) */
	MAX20356_LOCK_WD,
	/** Number of lock domains */
	MAX20356_LOCK_MAX,
};

/**
 * @brief Interrupt event groups dispatched from the INTB trigger.
 *
 * Consumers subscribe to a group with mfd_max20356_add_callback(); the parent
 * unmasks only the sources for which at least one callback is registered.
 */
enum max20356_event {
	/** ChgStat / JEITA / CC1Tmo changes */
	MAX20356_EVT_CHARGER,
	/** UsbOk / UsbOVP */
	MAX20356_EVT_USB,
	/** ThmSD / ThmStat / thermal LDO/buck shutdown */
	MAX20356_EVT_THERMAL,
	/** UVLO / SC / DRP / buck-boost fault, load-switch timeout */
	MAX20356_EVT_REG_FAULT,
	/** Dedicated DVS / PGOOD transition complete */
	MAX20356_EVT_DVS_DONE,
	/** Watchdog timer */
	MAX20356_EVT_WATCHDOG,
	/** Miscellaneous (I2cTmo, StepChg, ...) */
	MAX20356_EVT_MISC,
	/** Number of event groups */
	MAX20356_EVT_MAX,
};

/**
 * @brief INTB event callback.
 *
 * Invoked from the trigger workqueue (never from ISR context) when a subscribed
 * event fires.
 *
 * @param dev MAX20356 MFD parent device.
 * @param evt Event group that fired.
 * @param user User data supplied at registration.
 */
typedef void (*max20356_cb_t)(const struct device *dev, enum max20356_event evt, void *user);

/**
 * @brief Read a single register.
 *
 * @param dev MAX20356 MFD parent device.
 * @param reg Register address.
 * @param val Destination for the read byte.
 *
 * @retval 0 On success.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_reg_read(const struct device *dev, uint8_t reg, uint8_t *val);

/**
 * @brief Write a single register.
 *
 * @param dev MAX20356 MFD parent device.
 * @param reg Register address.
 * @param val Byte to write.
 *
 * @retval 0 On success.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_reg_write(const struct device *dev, uint8_t reg, uint8_t val);

/**
 * @brief Read-modify-write selected bits of a register.
 *
 * The read-modify-write is performed as a single bus-locked I2C transaction.
 *
 * @param dev MAX20356 MFD parent device.
 * @param reg Register address.
 * @param mask Mask of bits to modify.
 * @param val New value for the masked bits.
 *
 * @retval 0 On success.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_reg_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Read-modify-write a password-protected register.
 *
 * Performs the REQ-LOCK-001 sequence: unmask @p domain in its LockMsk register,
 * write the unlock password (0x55) to the matching LockUnlock register, apply the
 * masked update, then re-apply the lock password (0xAA). The whole sequence is
 * serialized against other lock users by the parent.
 *
 * @param dev MAX20356 MFD parent device.
 * @param domain Lock domain guarding @p reg.
 * @param reg Register address.
 * @param mask Mask of bits to modify.
 * @param val New value for the masked bits.
 *
 * @retval 0 On success.
 * @retval -EINVAL Invalid lock domain.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_reg_update_locked(const struct device *dev, enum max20356_lock_domain domain,
				   uint8_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Get the device variant.
 *
 * @param dev MAX20356 MFD parent device.
 *
 * @return The variant selected by the devicetree compatible string.
 */
enum max20356_variant mfd_max20356_get_variant(const struct device *dev);

/**
 * @brief Issue a power-mode / reset command.
 *
 * @param dev MAX20356 MFD parent device.
 * @param cmd Command to issue.
 *
 * @retval 0 On success.
 * @retval -EINVAL Invalid command.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_power_command(const struct device *dev, enum max20356_power_cmd cmd);

/**
 * @brief Select the IVMON monitor-mux channel.
 *
 * @param dev MAX20356 MFD parent device.
 * @param channel Monitor channel (MONCfg.MONCtr, 0..15).
 * @param ratio Divider ratio selection (MONCfg.MONRatioCfg, 0..3).
 *
 * @retval 0 On success.
 * @retval -EINVAL Channel or ratio out of range.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_mon_select(const struct device *dev, uint8_t channel, uint8_t ratio);

/**
 * @brief Set the watchdog reset-type action (WDCntl.WDRstType).
 *
 * @param dev MAX20356 MFD parent device.
 * @param rsttype Reset-type action.
 *
 * @retval 0 On success.
 * @retval -errno Negative errno propagated from the I2C bus.
 */
int mfd_max20356_wdt_set_rsttype(const struct device *dev, enum max20356_wdt_rsttype rsttype);


#ifdef CONFIG_MFD_MAX20356_TRIGGER
/**
 * @brief Register an INTB event callback.
 *
 * The parent unmasks the hardware sources backing @p evt when the first callback
 * for that group is registered.
 *
 * @param dev MAX20356 MFD parent device.
 * @param evt Event group to subscribe to.
 * @param cb Callback to invoke.
 * @param user User data passed back to the callback.
 *
 * @retval 0 On success.
 * @retval -EINVAL Invalid event or callback.
 * @retval -ENOTSUP Trigger support not enabled (CONFIG_MFD_MAX20356_TRIGGER).
 * @retval -errno Negative errno from the I2C bus while unmasking.
 */
int mfd_max20356_add_callback(const struct device *dev, enum max20356_event evt,
			      max20356_cb_t cb, void *user);

/**
 * @brief Remove an INTB event callback.
 *
 * @param dev MAX20356 MFD parent device.
 * @param evt Event group the callback was registered for.
 * @param cb Callback to remove.
 *
 * @retval 0 On success.
 * @retval -EINVAL Callback not found.
 * @retval -ENOTSUP Trigger support not enabled (CONFIG_MFD_MAX20356_TRIGGER).
 */
int mfd_max20356_remove_callback(const struct device *dev, enum max20356_event evt,
				 max20356_cb_t cb);
#endif /* CONFIG_MFD_MAX20356_TRIGGER */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_MAX20356_H_ */
