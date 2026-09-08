/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 INTB trigger tests. The pmic node carries int-gpios wired to the
 * native_sim gpio-emul controller, so a test can drive the INTB line to fire the
 * parent's GPIO callback, and seed the Int0-5 registers through the I2C emulator
 * to steer source decode. pmic58 has no int-gpios and exercises the poll-only
 * path.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/kernel.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

/* Time for the trigger bottom half (workqueue or own thread) to run. */
#define TRIG_SETTLE K_MSEC(50)

struct trig_record {
	uint32_t count;
	enum max20356_event last_evt;
	void *last_user;
};

static struct trig_record record_a;
static struct trig_record record_b;

static void cb_a(const struct device *dev, enum max20356_event evt, void *user)
{
	ARG_UNUSED(dev);

	record_a.count++;
	record_a.last_evt = evt;
	record_a.last_user = user;
}

static void cb_b(const struct device *dev, enum max20356_event evt, void *user)
{
	ARG_UNUSED(dev);

	record_b.count++;
	record_b.last_evt = evt;
	record_b.last_user = user;
}

struct max20356_trig_fixture {
	const struct device *dev;
	const struct emul *emul;
	const struct device *poll_dev;
	struct gpio_dt_spec int_gpio;
};

static void *trig_setup(void)
{
	static struct max20356_trig_fixture fixture = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(pmic)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
		.poll_dev = DEVICE_DT_GET(DT_NODELABEL(pmic58)),
		.int_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(pmic), int_gpios),
	};

	zassert_true(device_is_ready(fixture.dev), "pmic not ready");
	zassert_true(device_is_ready(fixture.poll_dev), "pmic58 not ready");
	zassert_true(gpio_is_ready_dt(&fixture.int_gpio), "INTB gpio not ready");

	return &fixture;
}

static void trig_before(void *f)
{
	struct max20356_trig_fixture *fixture = f;

	mfd_max20356_emul_reset(fixture->emul);
	record_a = (struct trig_record){0};
	record_b = (struct trig_record){0};

	/* Park INTB inactive (physical high, line is active-low) so a later drive
	 * to low is a clean edge-to-active.
	 */
	(void)gpio_emul_input_set(fixture->int_gpio.port, fixture->int_gpio.pin, 1);
	k_sleep(TRIG_SETTLE);
	record_a = (struct trig_record){0};
	record_b = (struct trig_record){0};
}

/* Assert INTB: drive the emulated line low (active), let the bottom half run. */
static void fire_intb(struct max20356_trig_fixture *fixture)
{
	(void)gpio_emul_input_set(fixture->int_gpio.port, fixture->int_gpio.pin, 1);
	(void)gpio_emul_input_set(fixture->int_gpio.port, fixture->int_gpio.pin, 0);
	k_sleep(TRIG_SETTLE);
}

ZTEST_SUITE(max20356_trig, NULL, trig_setup, trig_before, NULL, NULL);

/* Registering the first callback for a group unmasks its IntMask bits. */
ZTEST_F(max20356_trig, test_add_callback_unmasks)
{
	uint8_t m0, m1, m4;

	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a, NULL));

	/* Charger group spans Int0 (ChgStat, CC1Tmo), Int1 (JEITA reg/sd),
	 * Int4 (ChgRestart, ChgVoltMode); IntMask mirrors the Int bit layout.
	 */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INTMASK0, &m0);
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INTMASK1, &m1);
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INTMASK4, &m4);

	zassert_equal(m0, MAX20356_INT0_CHGSTATINT_MSK | MAX20356_INT0_CC1TMOINT_MSK,
		      "IntMask0 = 0x%02x", m0);
	zassert_equal(m1, MAX20356_INT1_CHGJEITAREGINT_MSK | MAX20356_INT1_CHGJEITASDINT_MSK,
		      "IntMask1 = 0x%02x", m1);
	zassert_equal(m4, MAX20356_INT4_CHGRESTARTINT_MSK | MAX20356_INT4_CHGVOLTMODEINT_MSK,
		      "IntMask4 = 0x%02x", m4);
}

/* Removing the callback re-masks its IntMask bits. */
ZTEST_F(max20356_trig, test_remove_callback_masks)
{
	uint8_t m0;

	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a, NULL));
	zassert_ok(mfd_max20356_remove_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INTMASK0, &m0);
	zassert_equal(m0, 0x00, "IntMask0 not cleared: 0x%02x", m0);
}

/* An INTB assertion with a pending charger source dispatches the charger cb. */
ZTEST_F(max20356_trig, test_dispatch_charger)
{
	int dummy = 0;

	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a, &dummy));

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT0, MAX20356_INT0_CHGSTATINT_MSK);
	fire_intb(fixture);

	zassert_equal(record_a.count, 1, "charger cb fired %u times", record_a.count);
	zassert_equal(record_a.last_evt, MAX20356_EVT_CHARGER);
	zassert_equal(record_a.last_user, &dummy, "user data not passed through");
}

/* A single INTB assertion fans out to every group with a pending source. */
ZTEST_F(max20356_trig, test_dispatch_multiple_groups)
{
	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a, NULL));
	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_THERMAL, cb_b, NULL));

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT0, MAX20356_INT0_CHGSTATINT_MSK);
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT1, MAX20356_INT1_THMSDINT_MSK);
	fire_intb(fixture);

	zassert_equal(record_a.count, 1, "charger cb fired %u times", record_a.count);
	zassert_equal(record_a.last_evt, MAX20356_EVT_CHARGER);
	zassert_equal(record_b.count, 1, "thermal cb fired %u times", record_b.count);
	zassert_equal(record_b.last_evt, MAX20356_EVT_THERMAL);
}

/* A pending source with no registered callback dispatches nothing. */
ZTEST_F(max20356_trig, test_unsubscribed_source_ignored)
{
	zassert_ok(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a, NULL));

	/* Thermal source pending, but only charger is subscribed. */
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT1, MAX20356_INT1_THMSDINT_MSK);
	fire_intb(fixture);

	zassert_equal(record_a.count, 0, "charger cb should not have fired");
}

/* Invalid arguments to the callback API are rejected. */
ZTEST_F(max20356_trig, test_add_callback_invalid)
{
	zassert_equal(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_MAX, cb_a, NULL),
		      -EINVAL);
	zassert_equal(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, NULL, NULL),
		      -EINVAL);
}

/* Removing a callback that was never registered fails. */
ZTEST_F(max20356_trig, test_remove_callback_notfound)
{
	zassert_equal(mfd_max20356_remove_callback(fixture->dev, MAX20356_EVT_CHARGER, cb_a),
		      -EINVAL);
	zassert_equal(mfd_max20356_remove_callback(fixture->dev, MAX20356_EVT_MAX, cb_a), -EINVAL);
}

/* While the watchdog owns INTB, the bottom half reads Int5 alone and leaves
 * Int0-4 untouched, so it cannot clear a source outside the watchdog's own
 * register. A seeded Int0 survives an INTB assertion; the seeded Int5 is read
 * (cleared) as the watchdog path expects.
 */
ZTEST_F(max20356_trig, test_wdt_active_reads_int5_only)
{
	uint8_t int0, int5;

	/* Callbacks are static driver state that earlier tests may have left
	 * registered; the watchdog cannot claim INTB while any are present.
	 */
	for (uint8_t evt = 0; evt < MAX20356_EVT_MAX; evt++) {
		(void)mfd_max20356_remove_callback(fixture->dev, (enum max20356_event)evt, cb_a);
		(void)mfd_max20356_remove_callback(fixture->dev, (enum max20356_event)evt, cb_b);
	}

	zassert_ok(mfd_max20356_wdt_claim(fixture->dev, true));

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT0, MAX20356_INT0_CHGSTATINT_MSK);
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT5, MAX20356_INT5_WDTMR_MSK);
	fire_intb(fixture);

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INT0, &int0);
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INT5, &int5);

	zassert_equal(int0, MAX20356_INT0_CHGSTATINT_MSK, "Int0 was read: 0x%02x", int0);
	zassert_equal(int5, 0x00, "Int5 not read/cleared: 0x%02x", int5);

	(void)mfd_max20356_wdt_claim(fixture->dev, false);
}

/* Without int-gpios the device is poll-only: the callback API still registers
 * and unmasks, but no INTB line drives dispatch.
 */
ZTEST_F(max20356_trig, test_poll_only_device)
{
	const struct emul *emul58 = EMUL_DT_GET(DT_NODELABEL(pmic58));
	uint8_t m0;

	mfd_max20356_emul_reset(emul58);

	zassert_ok(mfd_max20356_add_callback(fixture->poll_dev, MAX20356_EVT_CHARGER, cb_b, NULL));
	mfd_max20356_emul_get_reg(emul58, MAX20356_REG_INTMASK0, &m0);
	zassert_equal(m0, MAX20356_INT0_CHGSTATINT_MSK | MAX20356_INT0_CC1TMOINT_MSK,
		      "poll-only IntMask0 = 0x%02x", m0);

	zassert_ok(mfd_max20356_remove_callback(fixture->poll_dev, MAX20356_EVT_CHARGER, cb_b));
}
