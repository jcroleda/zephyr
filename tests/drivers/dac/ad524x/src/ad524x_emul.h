/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TESTS_DRIVERS_DAC_AD524X_EMUL_H_
#define TESTS_DRIVERS_DAC_AD524X_EMUL_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/emul.h>

/* Number of wiper channels on the AD524x family. */
#define AD524X_EMUL_NUM_CHANNELS 2U

/**
 * @brief Read back the 8-bit wiper code stored for a channel.
 *
 * This inspects the emulator state directly, independent of the device's
 * instruction-then-read readback protocol.
 *
 * @param target  Emulator instance.
 * @param channel Channel identifier (1 or 2).
 * @param code    Destination for the stored wiper code.
 *
 * @retval 0       On success.
 * @retval -EINVAL Invalid channel or NULL destination.
 */
int ad524x_emul_get_code(const struct emul *target, uint8_t channel, uint8_t *code);

/**
 * @brief Read back the device-wide shutdown state last captured.
 *
 * @param target   Emulator instance.
 * @param shutdown Destination for the captured shutdown state.
 *
 * @retval 0       On success.
 * @retval -EINVAL NULL destination.
 */
int ad524x_emul_get_shutdown(const struct emul *target, bool *shutdown);

#endif /* TESTS_DRIVERS_DAC_AD524X_EMUL_H_ */
