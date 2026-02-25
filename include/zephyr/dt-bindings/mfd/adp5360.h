/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_MFD_ADP5360_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_MFD_ADP5360_H_

/**
 * @defgroup MFD_adp5360 ADP5360 Devicetree helpers.
 * @ingroup mfd_interfaces
 * @{
 */

/**
 * @name ADP5360 Interrupt Sources
 * @{
 */
/** State of Charge Low */
#define ADP5360_INT_VBUS_VOLTAGE_THRESHOLD 0x0U
/** State of Charge Accumulated */
#define ADP5360_INT_CHARGER_MODE_CHANGE    0x1U
/** ADPICHG */
#define ADP5360_INT_BAT_VOLTAGE_THRESHOLD  0x2U
/** Battery Protection */
#define ADP5360_INT_TEMP_THRESHOLD         0x3U
/** Thermistor Temperature Threshold */
#define ADP5360_INT_BAT_PROTECTION         0x4U
/** Battery Voltage */
#define ADP5360_INT_ADPICHG                0x5U
/** Charger Mode Changed */
#define ADP5360_INT_SOC_ACM                0x6U
/** VBUS Voltage Threshold */
#define ADP5360_INT_SOC_LOW                0x7U
/** Manual Reset Triggered */
#define ADP5360_INT_BUCKBSTPGOOD           0x8U
/** Watchdog Timeout */
#define ADP5360_INT_BUCKPGOOD              0x9U
/** Buck PGOOD status */
#define ADP5360_INT_WATCHDOG_TIMEOUT       0xAU
/** Buckboost PGOOD status */
#define ADP5360_INT_MANUAL_RESET           0xBU
/** @} */

/** @} */

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_MFD_ADP5360_H_*/
