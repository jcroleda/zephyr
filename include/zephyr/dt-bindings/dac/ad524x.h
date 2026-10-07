/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_DAC_AD524X_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_DAC_AD524X_H_

/*
 * End-to-end (R_AB) nominal resistance of the AD524x variant, in ohms, for the
 * "max-resistance" devicetree property. Select the value matching the ordered
 * part number.
 */
#define AD524X_RESISTANCE_2K5	2500
#define AD524X_RESISTANCE_10K	10000
#define AD524X_RESISTANCE_50K	50000
#define AD524X_RESISTANCE_100K	100000

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_DAC_AD524X_H_ */
