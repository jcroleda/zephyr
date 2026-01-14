/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/ltc4286.h>
#include <zephyr/sys/__assert.h>

#define DELAY_WITH_TRIGGER    K_SECONDS(5)
#define DELAY_WITHOUT_TRIGGER K_SECONDS(1)

#define MVAL_PER_VAL 1000

#define TRIG_TEMP_OVERTEMP 15
#define TRIG_VOUT_OVERVOLT 10


K_SEM_DEFINE(sem, 0, 1);

static const char *now_str(void)
{
	static char buf[16]; /* ...HH:MM:SS.MMM */
	uint32_t now = k_uptime_get_32();
	unsigned int ms = now % MSEC_PER_SEC;
	unsigned int s;
	unsigned int min;
	unsigned int h;

	now /= MSEC_PER_SEC;
	s = now % 60U;
	now /= 60U;
	min = now % 60U;
	now /= 60U;
	h = now;

	snprintf(buf, sizeof(buf), "%u:%02u:%02u.%03u", h, min, s, ms);
	return buf;
}

static void trigger_handler(const struct device *dev, const struct sensor_trigger *trigger)
{
	printf("[%s]: Triggered | Type: %d\tChannel:%d\n", now_str(), trigger->type, trigger->chan);

	k_sem_give(&sem);
}

static int sensor_set_attribute(const struct device *dev, enum sensor_channel chan,
				enum sensor_attribute attr, int value)
{
	struct sensor_value sensor_val;
	int ret;

	sensor_val.val1 = value / MVAL_PER_VAL;
	sensor_val.val2 = value % MVAL_PER_VAL;

	ret = sensor_attr_set(dev, chan, attr, &sensor_val);
	if (ret) {
		printf("sensor_attr_set failed ret %d\n", ret);
	}

	return ret;
}
static int init_triggers(const struct device *dev)
{
	struct sensor_trigger trig;
	int32_t val;
	int ret;

	trig.type = SENSOR_TRIG_THRESHOLD;
	trig.chan = SENSOR_CHAN_VOLTAGE;

	val = TRIG_TEMP_OVERTEMP;

	ret = sensor_set_attribute(dev, SENSOR_CHAN_AMBIENT_TEMP, SENSOR_ATTR_UPPER_THRESH, val);
	if (ret) {
		return ret;
	}

	val = TRIG_VOUT_OVERVOLT;

	ret = sensor_set_attribute(dev, SENSOR_CHAN_VOLTAGE, SENSOR_ATTR_UPPER_THRESH, val);
	if (ret) {
		return ret;
	}

	if (ret == 0) {
		ret = sensor_trigger_set(dev, &trig, trigger_handler);
	}
	if (ret != 0) {
		printf("Could not set trigger\n");
		return -EINVAL;
	}

	return 0;
}

static void process(const struct device *dev)
{
	struct sensor_value temp_val, vout_val, vin_val, curr_val, pwr_val, alert_flags;
	int ret;

	/* todo: setup triggers */
	if (IS_ENABLED(CONFIG_LTC4286_TRIGGER)) {
		ret = init_triggers(dev);
		if (ret) {
			printf("Could not set trigger: %d\n", ret);
			return;
		}
	}

	while (1) {
		ret = sensor_sample_fetch(dev);
		if (ret) {
			printf("sensor_sample_fetch failed ret %d\n", ret);
			return;
		}

		ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_val);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: temperature - %.6f Cel\n", now_str(),
		       sensor_value_to_double(&temp_val));

		ret = sensor_channel_get(dev, SENSOR_CHAN_VOLTAGE, &vout_val);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: voltage out - %.6fV\n", now_str(), sensor_value_to_double(&vout_val));

		ret = sensor_channel_get(dev, SENSOR_CHAN_LTC4286_VIN, &vin_val);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: voltage in - %.6fV\n", now_str(), sensor_value_to_double(&vin_val));

		ret = sensor_channel_get(dev, SENSOR_CHAN_CURRENT, &curr_val);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: current out - %.6fA\n", now_str(), sensor_value_to_double(&curr_val));

		ret = sensor_channel_get(dev, SENSOR_CHAN_POWER, &pwr_val);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: power in - %.6fW\n", now_str(), sensor_value_to_double(&pwr_val));

		ret = sensor_channel_get(dev, SENSOR_CHAN_LTC4286_STATUS, &alert_flags);
		if (ret) {
			goto sensor_error;
		}

		printf("[%s]: alert flags - 0x%04X\n", now_str(), alert_flags.val1);

		k_sleep(DELAY_WITHOUT_TRIGGER);
	}
sensor_error:
	printf("sensor_channel_get failed ret %d\n", ret);
}

int main(void)
{
	const struct device *const dev = DEVICE_DT_GET_ONE(adi_ltc4286);

	if (!device_is_ready(dev)) {
		printk("sensor: device not ready.\n");
		return 0;
	}

	printf("device is %p, name is %s\n", dev, dev->name);

	process(dev);
	return 0;
}
