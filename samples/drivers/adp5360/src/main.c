/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/adp5360.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/regulator.h>
#include <sys/errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/__assert.h>

#define DEVICE_NAME_MFD "mfd"
#define DEVICE_NAME_CHARGER "charger"
#define DEVICE_NAME_FUEL_GAUGE "fuel gauge"
#define DEVICE_NAME_BUCK_REGULATOR "buck regulator"
#define DEVICE_NAME_BUCK_BOOST_REGULATOR "buck boost regulator"

static char *interrupt_source_name[] =	{"SOC Low",
					 "SOC Accumulated",
					 "ADPICHG",
					 "Battery Protection",
					 "Thermistor Threshold",
					 "Battery Threshold",
					 "Charger Mode Change",
					 "VBus Threshold",
					 "MR Triggered",
					 "Watchdog Timeout",
					 "Buck Change",
					 "Buckboost Change"};

static char *pgood_source_name[] =	{"Charge Complete",
					 "VBUS OK",
					 "Battery OK",
					 "VOUT1 OK",
					 "VOUT2 OK"};

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

static void trigger_handler(const struct device *dev, void *user_data)
{
	const char *trig_name = (char *)user_data;

	printk("[%s]: Alert detected in INT pin! Source %s\n", now_str(), trig_name);
}

static void reset_handler(const struct device *dev, void *user_data)
{
	printk("[%s]: Reset signal detected in RESET pin!\n", now_str());
}

static void pgood_handler(const struct device *dev, void *user_data)
{
	const char *trig_name = (char *)user_data;

	printk("[%s]: PGOOD signal detected in PGOOD pin!\n Source %s\n", now_str(), trig_name);
}

static int check_device_ready(const struct device *dev, const char *dev_name)
{
	if (dev == NULL) {
		printk("Error: no %s device found.\n", dev_name);
		return -1;
	}

	if (!device_is_ready(dev)) {
		printk("Error: Device \"%s\" is not ready; "
		       "check the driver initialization logs for errors.\n",
		       dev->name);
		return -1;
	}

	return 0;
}

static int read_charger_properties(const struct device *chgdev)
{
	union charger_propval charger_val;
	int ret;

	printk("[%s]: Found device \"%s\", getting charger data\n", now_str(), chgdev->name);

	ret = charger_get_prop(chgdev, CHARGER_PROP_INPUT_REGULATION_CURRENT_UA, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Regulation current on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}
	printk("[%s]: Current Limit: %d uA\n", now_str(),
	       charger_val.input_current_regulation_current_ua);

	ret = charger_get_prop(chgdev, CHARGER_PROP_PRECHARGE_CURRENT_UA, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Precharge current on device! ret %d\n", now_str(), ret);
		return -1;
	}
	printk("[%s]: Current (Weak Charge): %d uA\n", now_str(), charger_val.precharge_current_ua);

	ret = charger_get_prop(chgdev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Constant charge current on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}
	printk("[%s]: Current (Fast Charge): %d uA\n", now_str(),
	       charger_val.const_charge_current_ua);

	ret = charger_get_prop(chgdev, CHARGER_PROP_CHARGE_TERM_CURRENT_UA, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Termination current on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_INPUT_REGULATION_VOLTAGE_UV, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Adaptive Voltage on device! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: Adaptive Voltage: %d uV\n", now_str(),
	       charger_val.input_voltage_regulation_voltage_uv);

	ret = charger_get_prop(chgdev, CHARGER_PROP_CHARGE_TERM_CURRENT_UA, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Termination Current on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	printk("[%s]: Current (Termination): %d uA\n", now_str(),
	       charger_val.charge_term_current_ua);

	return 0;
}

static int setup_triggers(const struct device *mfddev)
{
	int ret;

	if (!IS_ENABLED(CONFIG_MFD_ADP5360_TRIGGER)) {
		return 0;
	}

	ret = mfd_adp5360_interrupt_trigger_set(mfddev,
						ADP5360_INTERRUPT_CHARGER_MODE_CHANGE,
						trigger_handler,
						(void *)interrupt_source_name[6]);
	if (ret) {
		printk("[%s]: Error setting trigger! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: INT trigger set for %s.\n", now_str(), interrupt_source_name[6]);

	ret = mfd_adp5360_reset_trigger_set(mfddev, reset_handler, NULL);
	if (ret) {
		printk("[%s]: Error setting reset trigger! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: reset trigger set.\n", now_str());

	ret = mfd_adp5360_pgood_trigger_set(mfddev, ADP5360_PGOOD1,
					    ADP5360_PGOOD_STATUS_VOUT1_OK,
					    pgood_handler,
					    (void *)pgood_source_name[3]);
	if (ret) {
		printk("[%s]: Error setting pgood trigger! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: PGOOD1 trigger set for %s.\n", now_str(), pgood_source_name[3]);

	ret = mfd_adp5360_pgood_trigger_set(mfddev, ADP5360_PGOOD1,
					    ADP5360_PGOOD_STATUS_CHARGE_COMPLETE,
					    pgood_handler,
					    (void *)pgood_source_name[0]);
	if (ret) {
		printk("[%s]: Error setting pgood trigger! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: PGOOD1 trigger set for %s.\n", now_str(), pgood_source_name[0]);

	ret = mfd_adp5360_pgood_trigger_set(mfddev, ADP5360_PGOOD2,
					    ADP5360_PGOOD_STATUS_VOUT2_OK,
					    pgood_handler,
					    (void *)pgood_source_name[4]);
	if (ret) {
		printk("[%s]: Error setting pgood trigger! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: PGOOD2 trigger set for %s.\n", now_str(), pgood_source_name[4]);

	return 0;
}

static int enable_charger(const struct device *chgdev)
{
	int ret;

	ret = charger_charge_enable(chgdev, true);
	if (ret) {
		printk("[%s]: Failed to start charger! ret %d\n", now_str(), ret);
		return -1;
	}

	return 0;
}

static int read_fuel_gauge_properties(const struct device *flgdev)
{
	union fuel_gauge_prop_val fuel_gauge_val;
	int ret;

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_DESIGN_CAPACITY, &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge Battery Capacity on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	printk("[%s]: Fuel gauge Battery Capacity: %d\n", now_str(), fuel_gauge_val.design_cap);

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_STATE_OF_CHARGE_ALARM, &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge SoC Alarm on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	printk("[%s]: Fuel gauge SoC Alarm: %d\n", now_str(), fuel_gauge_val.state_of_charge_alarm);

	return 0;
}

static int read_regulator_voltages(const struct device *bckdev,
				   const struct device *bstdev)
{
	uint32_t volt_out_uv;
	int ret;

	ret = regulator_get_voltage(bckdev, &volt_out_uv);
	if (ret) {
		printk("[%s]: Error getting buck voltage on device! ret %d\n", now_str(), ret);
		return -1;
	}

	printk("[%s]: Buck Output Voltage: %d\n", now_str(), volt_out_uv);

	ret = regulator_get_voltage(bstdev, &volt_out_uv);
	if (ret) {
		printk("[%s]: Error getting buck boost voltage on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	printk("[%s]: Buck Boost Output Voltage: %d\n", now_str(), volt_out_uv);

	return 0;
}

static int monitor_charger_status(const struct device *chgdev)
{
	union charger_propval charger_val;
	int ret;

	ret = charger_get_prop(chgdev, CHARGER_PROP_ONLINE, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Online status on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	if (charger_val.online == CHARGER_ONLINE_FIXED) {
		printk("[%s]: Online: Charger is active.\n", now_str());
	} else {
		printk("[%s]: Online: Charger is offline.\n", now_str());
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_PRESENT, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Battery Present status on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	if (charger_val.present) {
		printk("[%s]: Present: Battery is Present.\n", now_str());
	} else {
		printk("[%s]: Present: Battery is Not Present.\n", now_str());
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_STATUS, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Charging Status on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	switch (charger_val.status) {
	case CHARGER_STATUS_NOT_CHARGING:
		printk("[%s]: Status: Charger is Not Charging.\n", now_str());
		break;
	case CHARGER_STATUS_CHARGING:
		printk("[%s]: Status: Charger is Charging.\n", now_str());
		break;
	case CHARGER_STATUS_FULL:
		printk("[%s]: Status: Charging is Complete.\n", now_str());
		break;
	case CHARGER_STATUS_DISCHARGING:
		printk("[%s]: Status: Charger is not charging and in an internal status "
		       "state.\n",
		       now_str());
		break;
	default:
		printk("[%s]: Status: Charger Status is unknown.\n", now_str());
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_CHARGE_TYPE, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Charging Mode on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	switch (charger_val.charge_type) {
	case CHARGER_CHARGE_TYPE_NONE:
		printk("[%s]: Mode: Charger is Not Charging.\n", now_str());
		break;
	case CHARGER_CHARGE_TYPE_TRICKLE:
		printk("[%s]: Mode: Charger is in Trickle Mode.\n", now_str());
		break;
	case CHARGER_CHARGE_TYPE_FAST:
		printk("[%s]: Mode: Charging is in Fast Mode (CC).\n", now_str());
		break;
	case CHARGER_CHARGE_TYPE_LONGLIFE:
		printk("[%s]: Mode: Charging is in Fast Mode (CV).\n", now_str());
		break;
	case CHARGER_CHARGE_TYPE_UNKNOWN:
		printk("[%s]: Mode: Charger is not charging and in an internal status "
		       "state.\n",
		       now_str());
		break;
	default:
		printk("[%s]: Mode: Charger Type is unknown.\n", now_str());
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_HEALTH, &charger_val);
	if (ret) {
		printk("[%s]: Error getting Battery Health Status on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	switch (charger_val.health) {
	case CHARGER_HEALTH_COLD:
		printk("[%s]: Health: Battery is Cold.\n", now_str());
		break;
	case CHARGER_HEALTH_COOL:
		printk("[%s]: Health: Battery is Cool.\n", now_str());
		break;
	case CHARGER_HEALTH_WARM:
		printk("[%s]: Health: Battery is Warm.\n", now_str());
		break;
	case CHARGER_HEALTH_HOT:
		printk("[%s]: Health: Battery is Hot.\n", now_str());
		break;
	case CHARGER_HEALTH_WATCHDOG_TIMER_EXPIRE:
		printk("[%s]: Health: Charger Watchdog has expired!\n", now_str());
		break;
	case CHARGER_HEALTH_OVERVOLTAGE:
		printk("[%s]: Health: Charger Overvoltage has occurred!\n", now_str());
		break;
	case CHARGER_HEALTH_OVERHEAT:
		printk("[%s]: Health: Charger is Overheating!\n", now_str());
		break;
	case CHARGER_HEALTH_SAFETY_TIMER_EXPIRE:
		printk("[%s]: Health: Charger safety timer has expired!\n", now_str());
		break;
	case CHARGER_HEALTH_NO_BATTERY:
		printk("[%s]: Health: No battery connected!\n", now_str());
		break;
	case CHARGER_HEALTH_GOOD:
		printk("[%s]: Health: Charger health is good.\n", now_str());
		break;
	default:
		printk("[%s]: Health: Charger health is unknown.\n", now_str());
	}

	return 0;
}

static int monitor_fuel_gauge_status(const struct device *flgdev)
{
	union fuel_gauge_prop_val fuel_gauge_val;
	int ret;

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_VOLTAGE, &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge Voltage on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	printk("[%s]: Fuel gauge Voltage: %d\n", now_str(), fuel_gauge_val.voltage);

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_CYCLE_COUNT, &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge Cycle Count on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	printk("[%s]: Fuel gauge Cycle Count: %d\n", now_str(), fuel_gauge_val.cycle_count);

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_STATUS, &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge Status on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	printk("[%s]: Fuel gauge Status 0x%X\n", now_str(), fuel_gauge_val.fg_status);

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_ABSOLUTE_STATE_OF_CHARGE,
				  &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge SoC on device! ret %d\n", now_str(),
		       ret);
		return -1;
	}

	printk("[%s]: Fuel gauge SoC: %d\n", now_str(),
	       fuel_gauge_val.absolute_state_of_charge);

	ret = fuel_gauge_get_prop(flgdev, FUEL_GAUGE_THERM_VOLTAGE_UV,
				  &fuel_gauge_val);
	if (ret) {
		printk("[%s]: Error getting Fuel Gauge Thermistor Voltage on device! ret %d\n",
		       now_str(), ret);
		return -1;
	}

	printk("[%s]: Fuel gauge Thermistor Voltage (uV): %d\n", now_str(),
	       fuel_gauge_val.therm_voltage_uv);

	return 0;
}

static int monitor_loop(const struct device *chgdev,
			const struct device *flgdev)
{
	int ret;

	while (1) {
		ret = monitor_charger_status(chgdev);
		if (ret) {
			return ret;
		}

		printk("\n\n");

		ret = monitor_fuel_gauge_status(flgdev);
		if (ret) {
			return ret;
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}

int main(void)
{
	const struct device *mfddev = DEVICE_DT_GET_ONE(adi_adp5360);
	const struct device *chgdev = DEVICE_DT_GET_OR_NULL(DT_INST(0, adi_adp5360_charger));
	const struct device *flgdev = DEVICE_DT_GET_OR_NULL(DT_INST(0, adi_adp5360_fuel_gauge));
	const struct device *bckdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(buck));
	const struct device *bstdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(buckboost));
	int ret;

	ret = check_device_ready(mfddev, DEVICE_NAME_MFD);
	if (ret) {
		return ret;
	}

	ret = check_device_ready(chgdev, DEVICE_NAME_CHARGER);
	if (ret) {
		return ret;
	}

	ret = check_device_ready(flgdev, DEVICE_NAME_FUEL_GAUGE);
	if (ret) {
		return ret;
	}

	ret = check_device_ready(bckdev, DEVICE_NAME_BUCK_REGULATOR);
	if (ret) {
		return ret;
	}

	ret = check_device_ready(bstdev, DEVICE_NAME_BUCK_BOOST_REGULATOR);
	if (ret) {
		return ret;
	}

	ret = read_charger_properties(chgdev);
	if (ret) {
		return ret;
	}

	ret = setup_triggers(mfddev);
	if (ret) {
		return ret;
	}

	ret = enable_charger(chgdev);
	if (ret) {
		return ret;
	}

	ret = read_fuel_gauge_properties(flgdev);
	if (ret) {
		return ret;
	}

	ret = read_regulator_voltages(bckdev, bstdev);
	if (ret) {
		return ret;
	}

	ret = monitor_loop(chgdev, flgdev);
	if (ret) {
		return ret;
	}

	return 0;
}
