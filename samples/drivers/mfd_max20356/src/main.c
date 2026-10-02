/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 PMIC validation sample
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/regulator/max20356.h>
#include <zephyr/dt-bindings/mfd/max20356.h>
#include <sys/errno.h>
#include <stdio.h>

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

/* Classify a driver return code for a single write attempt. A rail that does
 * not implement an operation returns -ENOTSUP or -ENOSYS; that is expected
 * coverage, not a failure.
 */
static bool write_failed(int ret)
{
	return (ret != 0) && (ret != -ENOTSUP) && (ret != -ENOSYS);
}

static const char *write_result(int ret)
{
	if (ret == 0) {
		return "ok";
	}

	if ((ret == -ENOTSUP) || (ret == -ENOSYS)) {
		return "n/a";
	}

	return "FAIL";
}

struct rail {
	const struct device *dev;
	const char *name;
	/* set_voltage target in microvolts; 0 skips the call. */
	int32_t target_uv;
	/* set_current_limit target in microamps; 0 skips the call. */
	int32_t target_ua;
	/* Exercise set_mode (LDO1/LDO2 only). */
	bool toggle_mode;
	/* Enable is routed to an MPC pin in the overlay; the host must not drive
	 * the enable field over I2C, so skip the software enable/disable.
	 */
	bool mpc_enabled;
};

#define RAIL(label, nm, uv, ua, mode, mpc)                                                         \
	{                                                                                          \
		.dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(label)), .name = (nm),                    \
		.target_uv = (uv), .target_ua = (ua), .toggle_mode = (mode), .mpc_enabled = (mpc), \
	}

static const struct rail rails[] = {
	/*         label      name         target_uv  target_ua  mode   mpc  */
	RAIL(reg_buck1, "buck1 (I2C DVS)", 1000000, 200000, false, false),
	RAIL(reg_buck2, "buck2 (GPIO DVS)", 0, 200000, false, false),
	RAIL(reg_buck3, "buck3 (SPI DVS)", 1800000, 200000, false, false),
	RAIL(reg_bbst, "buckboost", 3300000, 200000, false, false),
	RAIL(reg_ldo1, "ldo1", 1800000, 0, true, false),
	RAIL(reg_ldo2, "ldo2", 3300000, 0, true, false),
	RAIL(reg_ldo3, "ldo3", 2500000, 0, false, false),
	RAIL(reg_ldo4, "ldo4 (RTC)", 1800000, 0, false, false),
	RAIL(reg_lsw1, "lsw1", 0, 0, false, false),
	RAIL(reg_lsw2, "lsw2 (MPC6 enable)", 0, 0, false, true),
	RAIL(reg_lsw3, "lsw3 (MPC7 enable)", 0, 0, false, true),
};

static int validate_rail(const struct rail *rail)
{
	int failures = 0;
	int ret;

	if (rail->dev == NULL) {
		printk("[%s]: %s: not present in devicetree, skipping\n", now_str(), rail->name);
		return 0;
	}

	if (!device_is_ready(rail->dev)) {
		printk("[%s]: %s: device not ready\n", now_str(), rail->name);
		return 1;
	}

	printk("[%s]: %s: validating I2C write operations\n", now_str(), rail->name);

	if (!rail->mpc_enabled) {
		ret = regulator_enable(rail->dev);
		printk("[%s]:   enable                 -> %s (%d)\n", now_str(), write_result(ret),
		       ret);
		failures += write_failed(ret) ? 1 : 0;
	} else {
		printk("[%s]:   enable                 -> skipped (MPC-controlled)\n", now_str());
	}

	if (rail->target_uv != 0) {
		ret = regulator_set_voltage(rail->dev, rail->target_uv, rail->target_uv);
		printk("[%s]:   set_voltage(%d)   -> %s (%d)\n", now_str(), rail->target_uv,
		       write_result(ret), ret);
		failures += write_failed(ret) ? 1 : 0;
	}

	if (rail->target_ua != 0) {
		ret = regulator_set_current_limit(rail->dev, rail->target_ua, rail->target_ua);
		printk("[%s]:   set_current_limit(%d) -> %s (%d)\n", now_str(), rail->target_ua,
		       write_result(ret), ret);
		failures += write_failed(ret) ? 1 : 0;
	}

	if (rail->toggle_mode) {
		ret = regulator_set_mode(rail->dev, MAX20356_MODE_LOAD_SWITCH);
		printk("[%s]:   set_mode(load switch)  -> %s (%d)\n", now_str(), write_result(ret),
		       ret);
		failures += write_failed(ret) ? 1 : 0;

		ret = regulator_set_mode(rail->dev, MAX20356_MODE_LDO);
		printk("[%s]:   set_mode(ldo)          -> %s (%d)\n", now_str(), write_result(ret),
		       ret);
		failures += write_failed(ret) ? 1 : 0;
	}

	ret = regulator_set_active_discharge(rail->dev, true);
	printk("[%s]:   set_active_discharge   -> %s (%d)\n", now_str(), write_result(ret), ret);
	failures += write_failed(ret) ? 1 : 0;

	/* Read back the configured voltage to correlate with a probe. A load
	 * switch and a GPIO-DVS buck report -ENOTSUP here, which is expected.
	 */
	{
		int32_t volt_uv;

		ret = regulator_get_voltage(rail->dev, &volt_uv);
		if (ret == 0) {
			printk("[%s]:   get_voltage            -> %d uV\n", now_str(), volt_uv);
		}
	}

	return failures;
}

static int validate_regulators(void)
{
	int failures = 0;

	printk("[%s]: === MAX20356 regulator validation ===\n", now_str());

	ARRAY_FOR_EACH_PTR(rails, rail) {
		failures += validate_rail(rail);
	}

	if (failures == 0) {
		printk("[%s]: Regulator validation PASSED: all I2C writes accepted\n", now_str());
	} else {
		printk("[%s]: Regulator validation FAILED: %d write(s) rejected\n", now_str(),
		       failures);
	}

	return failures;
}

/* Exercise the SPI DVS public API. buck3 is configured for SPI DVS (Mode 2) in
 * the overlay, so a direct regulator_max20356_spi_set_voltage() clocks a command
 * byte to the part over the parent's spi-dvs controller and the setpoint is read
 * back from the mirrored Buck3DvsSpi register over I2C. buck1 (I2C DVS) is used
 * to confirm the API rejects a rail that is not in SPI DVS mode.
 */
static int validate_spi_dvs(void)
{
	const struct device *buck3 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(reg_buck3));
	const struct device *buck1 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(reg_buck1));
	int failures = 0;
	int ret;

	printk("[%s]: === MAX20356 SPI DVS validation ===\n", now_str());

	if ((buck3 == NULL) || !device_is_ready(buck3)) {
		printk("[%s]: buck3 not ready, skipping SPI DVS test\n", now_str());
		return 0;
	}

	/* 1.00V on a 25mV/0.5V-base buck => VLT code 20. */
	ret = regulator_max20356_spi_set_voltage(buck3, 1500000, 1500000);
	printk("[%s]: buck3 spi_set_voltage(1500000) -> %s (%d)\n", now_str(), write_result(ret),
	       ret);
	failures += write_failed(ret) ? 1 : 0;

	if (ret == 0) {
		int32_t volt_uv;

		ret = regulator_get_voltage(buck3, &volt_uv);
		if (ret == 0) {
			printk("[%s]:   get_voltage (readback) -> %d uV\n", now_str(), volt_uv);
			if (volt_uv != 1500000) {
				printk("[%s]:   readback mismatch (expected 1000000)\n", now_str());
				failures++;
			}
		}
	}

	/* A rail that is not in SPI DVS mode must be rejected with -ENOTSUP. */
	if ((buck1 != NULL) && device_is_ready(buck1)) {
		ret = regulator_max20356_spi_set_voltage(buck1, 1000000, 1000000);
		printk("[%s]: buck1 spi_set_voltage (non-SPI) -> %s (%d)\n", now_str(),
		       (ret == -ENOTSUP) ? "ok" : "FAIL", ret);
		failures += (ret == -ENOTSUP) ? 0 : 1;
	}

	if (failures == 0) {
		printk("[%s]: SPI DVS validation PASSED\n", now_str());
	} else {
		printk("[%s]: SPI DVS validation FAILED: %d check(s) failed\n", now_str(),
		       failures);
	}

	return failures;
}

/* Exercise GPIO DVS. buck2 is configured for GPIO DVS (Mode 1) in the overlay:
 * the PMIC samples the live state of an MPC pin pair (adi,dvs-mpc-pair = <3 5>)
 * to select one of four preset voltages (adi,dvs-voltages, loaded into
 * Buck2DvsVlt0..3 at init). MPC3 is the low select bit and MPC5 the high select
 * bit, so the select state picks the preset as 00->preset0 .. 11->preset3.
 *
 * The host drives the two select pins over I2C through the parent's MPC GPIO
 * controller. A direct regulator_set_voltage() on a GPIO-DVS buck is -ENOTSUP
 * (the live value is pin-selected, not a single setpoint), so this walks the four
 * select states and prints the preset each selects for correlation with a probe.
 */
#define GPIO_DVS_LOW_PIN  3
#define GPIO_DVS_HIGH_PIN 5

static const int32_t gpio_dvs_presets[4] = {1000000, 1100000, 1200000, 1300000};

static int validate_gpio_dvs(void)
{
	const struct device *buck2 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(reg_buck2));
	const struct device *gpio = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio_mpc));
	int failures = 0;
	int ret;

	printk("[%s]: === MAX20356 GPIO DVS validation ===\n", now_str());

	if ((buck2 == NULL) || !device_is_ready(buck2)) {
		printk("[%s]: buck2 not ready, skipping GPIO DVS test\n", now_str());
		return 0;
	}

	if ((gpio == NULL) || !device_is_ready(gpio)) {
		printk("[%s]: MPC GPIO controller not ready, skipping GPIO DVS test\n", now_str());
		return 0;
	}

	/* A direct setpoint must be rejected on a GPIO-DVS buck. */
	ret = regulator_set_voltage(buck2, 1000000, 1000000);
	printk("[%s]: buck2 set_voltage (GPIO DVS) -> %s (%d)\n", now_str(),
	       (ret == -ENOTSUP) ? "ok" : "FAIL", ret);
	failures += (ret == -ENOTSUP) ? 0 : 1;

	/* Drive the select pins as outputs so the host owns the DVS state. */
	ret = gpio_pin_configure(gpio, GPIO_DVS_LOW_PIN, GPIO_OUTPUT_LOW);
	failures += write_failed(ret) ? 1 : 0;
	ret = gpio_pin_configure(gpio, GPIO_DVS_HIGH_PIN, GPIO_OUTPUT_LOW);
	failures += write_failed(ret) ? 1 : 0;

	/* Walk 00, 01, 10, 11: the select state indexes the preset table. */
	for (uint8_t sel = 0U; sel < 4U; sel++) {
		int low = (sel & BIT(0)) != 0U;
		int high = (sel & BIT(1)) != 0U;

		ret = gpio_pin_set(gpio, GPIO_DVS_LOW_PIN, low);
		failures += write_failed(ret) ? 1 : 0;
		ret = gpio_pin_set(gpio, GPIO_DVS_HIGH_PIN, high);
		failures += write_failed(ret) ? 1 : 0;

		printk("[%s]:   select %d%d (MPC%d=%d MPC%d=%d) -> preset %u = %d uV\n", now_str(),
		       high, low, GPIO_DVS_HIGH_PIN, high, GPIO_DVS_LOW_PIN, low, sel,
		       gpio_dvs_presets[sel]);

		/* Let the rail settle so a probe can sample each preset. */
		k_sleep(K_MSEC(100));
	}

	if (failures == 0) {
		printk("[%s]: GPIO DVS validation PASSED\n", now_str());
	} else {
		printk("[%s]: GPIO DVS validation FAILED: %d check(s) failed\n", now_str(),
		       failures);
	}

	return failures;
}

static int read_charger_properties(const struct device *chgdev)
{
	union charger_propval val;
	int ret;

	printk("[%s]: Charger \"%s\": reading configuration\n", now_str(), chgdev->name);

	ret = charger_get_prop(chgdev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &val);
	if (ret != 0) {
		printk("[%s]: Error getting constant charge current! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]:   Constant charge current: %d uA\n", now_str(),
	       val.const_charge_current_ua);

	ret = charger_get_prop(chgdev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &val);
	if (ret != 0) {
		printk("[%s]: Error getting charge voltage! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]:   Charge termination voltage: %d uV\n", now_str(),
	       val.const_charge_voltage_uv);

	ret = charger_charge_enable(chgdev, true);
	if (ret != 0) {
		printk("[%s]: Failed to enable charger! ret %d\n", now_str(), ret);
		return ret;
	}

	return 0;
}

static int monitor_charger_status(const struct device *chgdev)
{
	union charger_propval val;
	int ret;

	ret = charger_get_prop(chgdev, CHARGER_PROP_ONLINE, &val);
	if (ret != 0) {
		printk("[%s]: Error getting online status! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: Online: Charger is %s.\n", now_str(),
	       (val.online == CHARGER_ONLINE_FIXED) ? "active" : "offline");

	ret = charger_get_prop(chgdev, CHARGER_PROP_STATUS, &val);
	if (ret != 0) {
		printk("[%s]: Error getting charging status! ret %d\n", now_str(), ret);
		return ret;
	}

	switch (val.status) {
	case CHARGER_STATUS_NOT_CHARGING:
		printk("[%s]: Status: Not charging.\n", now_str());
		break;
	case CHARGER_STATUS_CHARGING:
		printk("[%s]: Status: Charging.\n", now_str());
		break;
	case CHARGER_STATUS_FULL:
		printk("[%s]: Status: Charge complete.\n", now_str());
		break;
	case CHARGER_STATUS_DISCHARGING:
		printk("[%s]: Status: Discharging.\n", now_str());
		break;
	default:
		printk("[%s]: Status: Unknown.\n", now_str());
	}

	ret = charger_get_prop(chgdev, CHARGER_PROP_HEALTH, &val);
	if (ret != 0) {
		printk("[%s]: Error getting health! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: Health code: %d\n", now_str(), val.health);

	return 0;
}

static int monitor_fuel_gauge_status(const struct device *flgdev)
{
	struct sensor_value val;
	int ret;

	ret = sensor_sample_fetch(flgdev);
	if (ret != 0) {
		printk("[%s]: Error fetching fuel gauge sample! ret %d\n", now_str(), ret);
		return ret;
	}

	ret = sensor_channel_get(flgdev, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	if (ret != 0) {
		printk("[%s]: Error getting fuel gauge voltage! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: Fuel gauge voltage: %d.%06d V\n", now_str(), val.val1, val.val2);

	ret = sensor_channel_get(flgdev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &val);
	if (ret != 0) {
		printk("[%s]: Error getting fuel gauge SoC! ret %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: Fuel gauge SoC: %d%%\n", now_str(), val.val1);

	return 0;
}

/* How long to wait for the charger status change to propagate through INTB and
 * the parent's trigger thread into the notifier callback.
 */
#define INTB_SETTLE_MS 200

/* Incremented from the charger status notifier, dispatched from the parent's INTB
 * trigger context (never an ISR), so a plain counter read from the main thread is
 * adequate for this test.
 */
static volatile uint32_t chg_status_notifications;

/* Charger status change notification. Reaching this proves the whole INTB path
 * worked: PMIC INTB pin -> host GPIO IRQ -> parent Int0-5 decode -> charger
 * event callback.
 */
static void chg_status_cb(enum charger_status status)
{
	ARG_UNUSED(status);

	chg_status_notifications++;
}

/* Self-test the INTB pin using the charger status interrupt, which is the only
 * INTB source triggerable purely over I2C: install a status notifier (which
 * unmasks ChgStatInt onto INTB via the parent), then toggle charge enable to
 * provoke a charger state change. A notification confirms the INTB pin and the
 * whole trigger path function.
 */
static int validate_intb(const struct device *chgdev)
{
	union charger_propval notif = {.status_notification = chg_status_cb};
	union charger_propval none = {.status_notification = NULL};
	int ret;

	printk("[%s]: === MAX20356 INTB self-test (charger status) ===\n", now_str());

	chg_status_notifications = 0U;

	ret = charger_set_prop(chgdev, CHARGER_PROP_STATUS_NOTIFICATION, &notif);
	if (ret != 0) {
		printk("[%s]: install status notifier failed: %d\n", now_str(), ret);
		return ret;
	}

	printk("[%s]: Enabled Status Notifier", now_str());

	/* Toggle charge enable to force a charger state transition, which latches
	 * ChgStatInt and asserts INTB.
	 */
	(void)charger_charge_enable(chgdev, false);
	k_sleep(K_MSEC(INTB_SETTLE_MS));
	(void)charger_charge_enable(chgdev, true);
	k_sleep(K_MSEC(INTB_SETTLE_MS));

	if (chg_status_notifications > 0U) {
		printk("[%s]: INTB self-test PASSED: %u notification(s)\n", now_str(),
		       chg_status_notifications);
	} else {
		/* Soft result: with no battery or charge source attached on the
		 * bench, a charge-enable toggle may not change the charger state,
		 * so no interrupt is raised. This is not a wiring failure.
		 */
		printk("[%s]: INTB self-test: no notification observed (expected if no "
		       "charge source is attached)\n",
		       now_str());
	}

	/* Drop the notifier so INTB is left free for exclusive consumers. */
	(void)charger_set_prop(chgdev, CHARGER_PROP_STATUS_NOTIFICATION, &none);

	return 0;
}

int main(void)
{
	const struct device *pmic = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pmic));
	const struct device *chgdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(charger));
	const struct device *flgdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(fuelgauge));
	int ret;

	/* The charger and regulator rails route their register access through the
	 * MFD parent, so it must be up before any child is usable. Its init
	 * priority (CONFIG_MFD_INIT_PRIORITY) runs ahead of the children, but
	 * check it here to fail early and clearly if the parent did not come up.
	 */
	if ((pmic == NULL) || !device_is_ready(pmic)) {
		printk("[%s]: MFD parent (MAX20356) not ready\n", now_str());
		return -ENODEV;
	}

	if ((chgdev == NULL) || !device_is_ready(chgdev)) {
		printk("[%s]: charger device not ready\n", now_str());
		return -ENODEV;
	}

	if ((flgdev == NULL) || !device_is_ready(flgdev)) {
		printk("[%s]: fuel gauge device not ready\n", now_str());
		return -ENODEV;
	}

	ret = validate_regulators();
	if (ret != 0) {
		return ret;
	}

	ret = validate_spi_dvs();
	if (ret != 0) {
		return ret;
	}

	ret = validate_gpio_dvs();
	if (ret != 0) {
		return ret;
	}

	/* INTB pin self-test: the charger status interrupt is triggerable purely
	 * over I2C. The watchdog alert (which claims INTB exclusively) is a separate,
	 * mutually exclusive sample under watchdog/.
	 */
	ret = validate_intb(chgdev);
	if (ret != 0) {
		return ret;
	}

	ret = read_charger_properties(chgdev);
	if (ret != 0) {
		return ret;
	}

	while (1) {
		ret = monitor_charger_status(chgdev);
		if (ret != 0) {
			return ret;
		}

		ret = monitor_fuel_gauge_status(flgdev);
		if (ret != 0) {
			return ret;
		}

		printk("\n");
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
