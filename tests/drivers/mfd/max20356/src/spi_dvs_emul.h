/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MAX20356_TEST_SPI_DVS_EMUL_H_
#define MAX20356_TEST_SPI_DVS_EMUL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

/** Bind the SPI DVS target emulator to the MFD I2C emulator for VLT mirroring. */
void spi_dvs_emul_bind(const struct emul *mfd_emul);

/** Last command byte {AD[1:0], VLT[5:0]} clocked to the SPI DVS target. */
uint8_t spi_dvs_emul_last_cmd(void);

/** True once at least one SPI DVS command byte has been received. */
bool spi_dvs_emul_got_cmd(void);

#endif /* MAX20356_TEST_SPI_DVS_EMUL_H_ */
