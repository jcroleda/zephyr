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

/**
 * @name MPC-pin enable-routing selectors
 *
 * Cell values for the @c adi,mpc-enable-map property on the regulators node.
 * Each cell (indexed by MPC pin 0..7) selects the rail whose enable that pin
 * controls (<rail>Ctr.<rail>MPC<pin>), or MAX20356_MPC_NONE to leave the pin
 * unrouted.
 * @{
 */
/** MPC pin not routed to any rail */
#define MAX20356_MPC_NONE      0
/** Route to BUCK1 */
#define MAX20356_MPC_BUCK1     1
/** Route to BUCK2 */
#define MAX20356_MPC_BUCK2     2
/** Route to BUCK3 */
#define MAX20356_MPC_BUCK3     3
/** Route to the buck-boost */
#define MAX20356_MPC_BUCKBOOST 4
/** Route to LDO1 */
#define MAX20356_MPC_LDO1      5
/** Route to LDO2 */
#define MAX20356_MPC_LDO2      6
/** Route to LDO3 */
#define MAX20356_MPC_LDO3      7
/** Route to LDO4 */
#define MAX20356_MPC_LDO4      8
/** Route to LSW1 */
#define MAX20356_MPC_LSW1      9
/** Route to LSW2 */
#define MAX20356_MPC_LSW2      10
/** Route to LSW3 */
#define MAX20356_MPC_LSW3      11
/** @} */

/** @} */

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_MFD_MAX20356_H_ */
