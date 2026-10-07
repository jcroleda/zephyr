/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Header file for the extended DAC API of the AD5243/AD5248
 * @ingroup ad524x_interface
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DAC_AD524X_H_
#define ZEPHYR_INCLUDE_DRIVERS_DAC_AD524X_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Analog Devices AD5243/AD5248 dual-channel digital potentiometer
 * @defgroup ad524x_interface AD5243/AD5248
 * @ingroup dac_interface_ext
 * @{
 */

/**
 * @brief Enable or disable device shutdown.
 *
 * Shutdown applies to the whole device: both wipers (RDAC) are connected to
 * their B terminals and the A terminals are open-circuited, placing the resistor
 * strings in a low-power state. The wiper register contents are preserved, so
 * disabling shutdown restores the resistances previously programmed through
 * dac_write_value().
 *
 * @param dev      Pointer to the device structure for the driver instance.
 * @param shutdown @c true to enter shutdown, @c false to resume normal output.
 *
 * @retval 0    On success.
 * @retval -EIO If the I2C transfer fails.
 */
int dac_ad524x_shutdown(const struct device *dev, bool shutdown);

/**
 * @brief Read back the 8-bit wiper code currently stored for a channel.
 *
 * The device returns the wiper code of the most recently addressed channel, so
 * this first issues a one-byte instruction write to point at @p channel and then
 * reads a single byte. The instruction write carries no data byte, so the wiper
 * code is left unchanged.
 *
 * @param dev     Pointer to the device structure for the driver instance.
 * @param channel Channel identifier (1 or 2).
 * @param code    Destination for the 8-bit wiper code (0 to 255).
 *
 * @retval 0       On success.
 * @retval -EINVAL If @p channel is not 1 or 2, or @p code is NULL.
 * @retval -EIO    If the I2C transfer fails.
 */
int dac_ad524x_read_value(const struct device *dev, uint8_t channel, uint8_t *code);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DAC_AD524X_H_ */
