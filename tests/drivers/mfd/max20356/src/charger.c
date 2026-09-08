/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 charger API tests. Status/charge-type/online/health are decoded from
 * the read-only Status0/Status1 registers, seeded through the I2C emulator back
 * door; current/voltage set-points round-trip through the ChgCur0/ChgCntl1
 * registers; the INTB notifier path is exercised by firing the parent trigger.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

#define TRIG_SETTLE K_MSEC(50)

struct max20356_chg_fixture {
	const struct device *dev;
	const struct device *mfd;
	const struct emul *emul;
	struct gpio_dt_spec int_gpio;
};

static void *chg_setup(void)
{
	static struct max20356_chg_fixture fixture = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(charger)),
		.mfd = DEVICE_DT_GET(DT_NODELABEL(pmic)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
		.int_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(pmic), int_gpios),
	};

	zassert_true(device_is_ready(fixture.dev), "charger not ready");

	return &fixture;
}

static void chg_before(void *f)
{
	struct max20356_chg_fixture *fixture = f;
	union charger_propval clr = {0};

	/* Notifier state is static driver data that survives across tests; clear
	 * both so each test starts with no INTB callback held (registration is
	 * lazy: a notifier claims its group, a NULL notifier releases it).
	 */
	(void)charger_set_prop(fixture->dev, CHARGER_PROP_STATUS_NOTIFICATION, &clr);
	(void)charger_set_prop(fixture->dev, CHARGER_PROP_ONLINE_NOTIFICATION, &clr);

	mfd_max20356_emul_reset(fixture->emul);
}

ZTEST_SUITE(max20356_chg, NULL, chg_setup, chg_before, NULL, NULL);

/* Seed Status0.ChgStat and read back the decoded charger status. */
static enum charger_status status_for(struct max20356_chg_fixture *fixture, uint8_t chgstat)
{
	union charger_propval val = {0};

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS0,
				  FIELD_PREP(MAX20356_STATUS0_CHGSTAT_MSK, chgstat));
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_STATUS, &val));
	return val.status;
}

ZTEST_F(max20356_chg, test_status_decode)
{
	zassert_equal(status_for(fixture, 0x0), CHARGER_STATUS_NOT_CHARGING); /* off */
	zassert_equal(status_for(fixture, 0x1), CHARGER_STATUS_NOT_CHARGING); /* idle */
	zassert_equal(status_for(fixture, 0x2), CHARGER_STATUS_CHARGING);     /* prechg */
	zassert_equal(status_for(fixture, 0x3), CHARGER_STATUS_CHARGING);     /* CC1 */
	zassert_equal(status_for(fixture, 0x5), CHARGER_STATUS_CHARGING);     /* CV */
	zassert_equal(status_for(fixture, 0x7), CHARGER_STATUS_FULL);         /* tmo done */
	zassert_equal(status_for(fixture, 0x8), CHARGER_STATUS_NOT_CHARGING); /* fault */
	zassert_equal(status_for(fixture, 0xF), CHARGER_STATUS_NOT_CHARGING); /* temp sus */
	zassert_equal(status_for(fixture, 0xA), CHARGER_STATUS_UNKNOWN);      /* reserved */
}

static enum charger_charge_type charge_type_for(struct max20356_chg_fixture *fixture,
						uint8_t chgstat)
{
	union charger_propval val = {0};

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS0,
				  FIELD_PREP(MAX20356_STATUS0_CHGSTAT_MSK, chgstat));
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_CHARGE_TYPE, &val));
	return val.charge_type;
}

ZTEST_F(max20356_chg, test_charge_type_decode)
{
	zassert_equal(charge_type_for(fixture, 0x0), CHARGER_CHARGE_TYPE_NONE);
	zassert_equal(charge_type_for(fixture, 0x2), CHARGER_CHARGE_TYPE_TRICKLE); /* prechg */
	zassert_equal(charge_type_for(fixture, 0x6), CHARGER_CHARGE_TYPE_TRICKLE); /* maintain */
	zassert_equal(charge_type_for(fixture, 0x3), CHARGER_CHARGE_TYPE_FAST);    /* CC1 */
	zassert_equal(charge_type_for(fixture, 0x5), CHARGER_CHARGE_TYPE_FAST);    /* CV */
	zassert_equal(charge_type_for(fixture, 0xA), CHARGER_CHARGE_TYPE_UNKNOWN); /* reserved */
}

ZTEST_F(max20356_chg, test_online_decode)
{
	union charger_propval val = {0};

	/* UsbOk set, UsbOVP clear => online. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, MAX20356_STATUS1_USBOK_MSK);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_ONLINE, &val));
	zassert_equal(val.online, CHARGER_ONLINE_FIXED);

	/* UsbOVP overrides UsbOk => offline. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1,
				  MAX20356_STATUS1_USBOK_MSK | MAX20356_STATUS1_USBOVP_MSK);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_ONLINE, &val));
	zassert_equal(val.online, CHARGER_ONLINE_OFFLINE);

	/* Neither set => offline. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, 0x00);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_ONLINE, &val));
	zassert_equal(val.online, CHARGER_ONLINE_OFFLINE);
}

ZTEST_F(max20356_chg, test_health_fault_priority)
{
	union charger_propval val = {0};

	/* USB overvoltage wins over everything. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, MAX20356_STATUS1_USBOVP_MSK);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_HEALTH, &val));
	zassert_equal(val.health, CHARGER_HEALTH_OVERVOLTAGE);

	/* Thermal shutdown => overheat. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, MAX20356_STATUS1_THMSD_MSK);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_HEALTH, &val));
	zassert_equal(val.health, CHARGER_HEALTH_OVERHEAT);

	/* JEITA shutdown also => overheat. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1,
				  MAX20356_STATUS1_CHGJEITASD_MSK);
	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_HEALTH, &val));
	zassert_equal(val.health, CHARGER_HEALTH_OVERHEAT);
}

ZTEST_F(max20356_chg, test_health_thermal_zones)
{
	union charger_propval val = {0};
	const struct {
		uint8_t thmstat;
		enum charger_health health;
	} cases[] = {
		{0x0, CHARGER_HEALTH_COLD},
		{0x1, CHARGER_HEALTH_COOL},
		{0x3, CHARGER_HEALTH_WARM},
		{0x4, CHARGER_HEALTH_HOT},
		{0x2, CHARGER_HEALTH_GOOD}, /* room */
		{0x5, CHARGER_HEALTH_GOOD}, /* no thermistor */
	};

	ARRAY_FOR_EACH(cases, i) {
		mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS0,
					  FIELD_PREP(MAX20356_STATUS0_THMSTAT_MSK,
						     cases[i].thmstat));
		mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, 0x00);
		zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_HEALTH, &val));
		zassert_equal(val.health, cases[i].health, "thmstat 0x%x -> %d", cases[i].thmstat,
			      val.health);
	}
}

ZTEST_F(max20356_chg, test_set_get_charge_current)
{
	union charger_propval set = {.const_charge_current_ua = 100000};
	union charger_propval get = {0};
	uint8_t reg;

	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &set));

	/* 100mA: first segment is 4mA @2mA (to 130mA), so idx = (100000-4000)/2000 = 48. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_CHGCUR0, &reg);
	zassert_equal(FIELD_GET(MAX20356_CHGCUR0_CC1IFCHG_MSK, reg), 48, "CC1IFChg = 0x%02x", reg);

	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &get));
	zassert_equal(get.const_charge_current_ua, 100000);
}

ZTEST_F(max20356_chg, test_set_charge_current_high_segment)
{
	union charger_propval set = {.const_charge_current_ua = 300000};
	uint8_t reg;

	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &set));

	/* 300mA: second segment 140mA @10mA from 0x40, idx = 0x40 + (300000-140000)/10000 = 80. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_CHGCUR0, &reg);
	zassert_equal(FIELD_GET(MAX20356_CHGCUR0_CC1IFCHG_MSK, reg), 80, "CC1IFChg = 0x%02x", reg);
}

ZTEST_F(max20356_chg, test_set_charge_current_out_of_range)
{
	union charger_propval lo = {.const_charge_current_ua = 1000};    /* < 4mA */
	union charger_propval hi = {.const_charge_current_ua = 600000};  /* > 500mA */

	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &lo),
		      -EINVAL);
	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &hi),
		      -EINVAL);
}

ZTEST_F(max20356_chg, test_set_get_charge_voltage)
{
	union charger_propval set = {.const_charge_voltage_uv = 4200000};
	union charger_propval get = {0};
	uint8_t reg;

	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &set));

	/* 4.20V: 4.15V base @10mV => idx = (4200000-4150000)/10000 = 5. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_CHGCNTL1, &reg);
	zassert_equal(FIELD_GET(MAX20356_CHGCNTL1_CHGBATREG_MSK, reg), 5, "ChgBatReg = 0x%02x", reg);

	zassert_ok(charger_get_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &get));
	zassert_equal(get.const_charge_voltage_uv, 4200000);
}

ZTEST_F(max20356_chg, test_set_charge_voltage_out_of_range)
{
	union charger_propval lo = {.const_charge_voltage_uv = 4000000}; /* < 4.15V */
	union charger_propval hi = {.const_charge_voltage_uv = 4800000}; /* > 4.70V */

	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &lo),
		      -EINVAL);
	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &hi),
		      -EINVAL);
}

ZTEST_F(max20356_chg, test_charge_enable)
{
	uint8_t reg;

	zassert_ok(charger_charge_enable(fixture->dev, true));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_CHGCNTL0, &reg);
	zassert_true((reg & MAX20356_CHGCNTL0_CHGEN_MSK) != 0U, "ChgEn not set: 0x%02x", reg);

	zassert_ok(charger_charge_enable(fixture->dev, false));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_CHGCNTL0, &reg);
	zassert_true((reg & MAX20356_CHGCNTL0_CHGEN_MSK) == 0U, "ChgEn not cleared: 0x%02x", reg);
}

ZTEST_F(max20356_chg, test_unsupported_prop)
{
	union charger_propval val = {0};

	zassert_equal(charger_get_prop(fixture->dev, CHARGER_PROP_PRESENT, &val), -ENOTSUP);
	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_PRESENT, &val), -ENOTSUP);
}

ZTEST_F(max20356_chg, test_bus_error_propagates)
{
	union charger_propval val = {0};

	mfd_max20356_emul_set_fail(fixture->emul, true);

	zassert_true(charger_get_prop(fixture->dev, CHARGER_PROP_STATUS, &val) < 0);
	zassert_true(charger_get_prop(fixture->dev, CHARGER_PROP_ONLINE, &val) < 0);
	zassert_true(charger_get_prop(fixture->dev, CHARGER_PROP_HEALTH, &val) < 0);
	zassert_true(charger_get_prop(fixture->dev, CHARGER_PROP_CHARGE_TYPE, &val) < 0);
	zassert_true(charger_charge_enable(fixture->dev, true) < 0);

	mfd_max20356_emul_set_fail(fixture->emul, false);
}

/* Charger status/online notifiers fire when an INTB event is dispatched. */
static volatile enum charger_status notified_status;
static volatile uint32_t status_notify_count;

static void on_status(enum charger_status s)
{
	notified_status = s;
	status_notify_count++;
}

ZTEST_F(max20356_chg, test_status_notifier_on_intb)
{
	union charger_propval nv = {.status_notification = on_status};

	status_notify_count = 0;
	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_STATUS_NOTIFICATION, &nv));

	/* Seed a charging state and a pending charger interrupt, then fire INTB. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS0,
				  FIELD_PREP(MAX20356_STATUS0_CHGSTAT_MSK, 0x3));
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT0, MAX20356_INT0_CHGSTATINT_MSK);

	(void)gpio_emul_input_set(fixture->int_gpio.port, fixture->int_gpio.pin, 1);
	(void)gpio_emul_input_set(fixture->int_gpio.port, fixture->int_gpio.pin, 0);
	k_sleep(TRIG_SETTLE);

	zassert_true(status_notify_count > 0, "status notifier did not fire");
	zassert_equal(notified_status, CHARGER_STATUS_CHARGING);
}

/* With no notifier installed the charger holds no INTB callback, so an exclusive
 * consumer can claim INTB. Installing a notifier then blocks a fresh claim, and
 * clearing it frees INTB again. mfd_max20356_wdt_claim() is the observable proxy
 * for "is any INTB event callback registered".
 */
ZTEST_F(max20356_chg, test_lazy_registration_frees_intb)
{
	union charger_propval nv = {.status_notification = on_status};
	union charger_propval clr = {0};

	/* before-hook cleared both notifiers: INTB is free. */
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, true), "claim should succeed when idle");
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, false));

	/* Installing a notifier claims the CHARGER/THERMAL groups. */
	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_STATUS_NOTIFICATION, &nv));
	zassert_equal(mfd_max20356_wdt_claim(fixture->mfd, true), -EBUSY,
		      "claim should be refused while a notifier is installed");

	/* Clearing it releases the groups and frees INTB again. */
	zassert_ok(charger_set_prop(fixture->dev, CHARGER_PROP_STATUS_NOTIFICATION, &clr));
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, true), "claim should succeed once freed");
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, false));
}

/* A notifier cannot be installed while an exclusive consumer owns INTB: the lazy
 * add_callback is refused and the notifier state is rolled back so it does not
 * silently point at a callback the parent never registered.
 */
ZTEST_F(max20356_chg, test_notifier_refused_while_intb_claimed)
{
	union charger_propval nv = {.online_notification = NULL};

	nv.status_notification = on_status;

	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, true));

	zassert_equal(charger_set_prop(fixture->dev, CHARGER_PROP_STATUS_NOTIFICATION, &nv), -EBUSY,
		      "notifier install should fail while INTB is claimed");

	mfd_max20356_wdt_claim(fixture->mfd, false);

	/* Rollback: the notifier is not left installed, so no INTB group is held
	 * and a subsequent claim still succeeds.
	 */
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, true), "notifier state was not rolled back");
	zassert_ok(mfd_max20356_wdt_claim(fixture->mfd, false));
}
