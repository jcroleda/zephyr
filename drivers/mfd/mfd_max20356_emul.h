/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_MAX20356_EMUL_H_
#define ZEPHYR_DRIVERS_MFD_MFD_MAX20356_EMUL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

/**
 * @brief Back-door write of a register in the emulator.
 *
 * Bypasses the read-only protection applied to the I2C write path, letting a
 * test seed status/read-only registers to a chosen value.
 *
 * @param target Emulator instance.
 * @param reg Register address.
 * @param val Value to store.
 */
void mfd_max20356_emul_set_reg(const struct emul *target, uint8_t reg, uint8_t val);

/**
 * @brief Back-door read of a register in the emulator.
 *
 * @param target Emulator instance.
 * @param reg Register address.
 * @param val Destination for the stored value.
 */
void mfd_max20356_emul_get_reg(const struct emul *target, uint8_t reg, uint8_t *val);

/**
 * @brief Reset the emulator register file to power-on defaults.
 *
 * @param target Emulator instance.
 */
void mfd_max20356_emul_reset(const struct emul *target);

/**
 * @brief Force every subsequent I2C transfer to fail.
 *
 * Lets a test exercise the driver's bus-error paths.
 *
 * @param target Emulator instance.
 * @param fail True to make transfers return -EIO, false to resume normal
 *             operation.
 */
void mfd_max20356_emul_set_fail(const struct emul *target, bool fail);

/**
 * @brief Engage or release the lock state for a set of LockMsk1 domains.
 *
 * Lets a test start from a locked state so the unlock/write/re-lock sequence in
 * mfd_max20356_reg_update_locked() can be validated. Writes to a register whose
 * domain is engaged are dropped on the I2C path until it is unlocked.
 *
 * @param target Emulator instance.
 * @param lockmsk1_bits Mask of LockMsk1 bits to lock (set) or unlock (clear).
 * @param locked True to lock the given bits, false to unlock them.
 */
void mfd_max20356_emul_set_locked(const struct emul *target, uint8_t lockmsk1_bits, bool locked);

/**
 * @brief Engage or release the lock state for a set of LockMsk3 domains.
 *
 * Same as mfd_max20356_emul_set_locked() but for the LockMsk3 bank
 * (charger/limiter/watchdog domains), letting a test validate the unlock/write/
 * re-lock sequence on the watchdog register.
 *
 * @param target Emulator instance.
 * @param lockmsk3_bits Mask of LockMsk3 bits to lock (set) or unlock (clear).
 * @param locked True to lock the given bits, false to unlock them.
 */
void mfd_max20356_emul_set_locked3(const struct emul *target, uint8_t lockmsk3_bits, bool locked);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_MAX20356_EMUL_H_ */
