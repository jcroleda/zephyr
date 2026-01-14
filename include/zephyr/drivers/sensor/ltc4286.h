/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extended public API for ADI's LTC4286 temperature sensor
 *
 * This exposes attributes for the LTC4286 which can be used for
 * fetching the device's input voltage and alert flag telemetry data.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_LTC4286_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_LTC4286_H_

#include <zephyr/types.h>
#include <zephyr/drivers/sensor.h>

/* Status Command Codes */
#define LTC4286_STATUS_WORD_VOUT         BIT(15)
#define LTC4286_STATUS_WORD_IOUT         BIT(14)
#define LTC4286_STATUS_WORD_INPUT        BIT(13)
#define LTC4286_STATUS_WORD_MFRSPEC      BIT(12)
#define LTC4286_STATUS_WORD_FET_UV_THRES BIT(11)
#define LTC4286_STATUS_WORD_OTHER        BIT(9)
#define LTC4286_STATUS_WORD_UNKNOWN      BIT(8)
#define LTC4286_STATUS_WORD_BUSY         BIT(7)
#define LTC4286_STATUS_WORD_OFF          BIT(6)
#define LTC4286_STATUS_WORD_IOUT_OTHRES  BIT(4)
#define LTC4286_STATUS_WORD_VIN_UTHRES   BIT(3)
#define LTC4286_STATUS_WORD_TEMP         BIT(2)
#define LTC4286_STATUS_WORD_COMLINE      BIT(1)
#define LTC4286_STATUS_WORD_NA           BIT(0)

/*
 * Custom sensor channels for LTC4286
 */
enum ltc4286_sensor_chan {
	SENSOR_CHAN_LTC4286_VIN = SENSOR_CHAN_PRIV_START,
	SENSOR_CHAN_LTC4286_STATUS,
	SENSOR_CHAN_LTC4286_POWER_GOOD,
	SENSOR_CHAN_LTC4286_COMPARATOR,
	SENSOR_CHAN_LTC4286_ALERT,
};

#endif /* ZEPHYR_DRIVERS_SENSOR_LTC4286_LTC4286_H_ */
