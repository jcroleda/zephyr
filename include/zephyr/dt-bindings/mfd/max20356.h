/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mfd_max20356
 * @brief Devicetree helper macros for the MAX20356/MAX20358 PMIC.
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_MFD_MAX20356_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_MFD_MAX20356_H_

/**
 * @defgroup mfd_max20356 MAX20356/MAX20358 Devicetree helpers
 * @brief Analog Devices MAX20356/MAX20358 PMIC Devicetree helpers
 * @ingroup devicetree-mfd
 * @{
 */

/**
 * @name Regulator operating modes
 *
 * Values for the regulator @c regulator-initial-mode and
 * @c regulator-allowed-modes properties on the LDO1/LDO2 child nodes, which can
 * act either as a linear regulator or as a load switch (LDO<n>Cfg.LDO<n>Mode).
 * @{
 */
/** Linear-regulator (LDO) mode */
#define MAX20356_MODE_LDO         0
/** Load-switch mode */
#define MAX20356_MODE_LOAD_SWITCH 1
/** @} */

/** @} */

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_MFD_MAX20356_H_ */
