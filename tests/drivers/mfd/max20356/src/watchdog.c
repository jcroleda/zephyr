/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 watchdog child tests. The wdog node exposes the standard Zephyr wdt
 * API on top of the MFD parent. install_timeout maps window.max to WDTmrSel and
 * flags to WDRstType; setup arms WDCntl; feed reads the clear-on-read Int5.WDTmr
 * (modeled by the emulator); disable clears WDRstType. WDCntl is guarded by
 * LockMsk3.WDLck, so a locked-domain test proves the child's unlock sequence.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

/* A no-op INTB consumer used to exercise the watchdog's exclusive-ownership
 * (isolation) checks; it is never expected to fire in these tests.
 */
static void misc_cb(const struct device *dev, enum max20356_event evt, void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

struct max20356_wdt_fixture {
	const struct device *dev;
	const struct device *mfd;
	const struct emul *emul;
};

static void *wdt_suite_setup(void)
{
	/* The watchdog lives under pmic58, a parent with no charger/regulator
	 * children, so it can own the shared Int5 register exclusively while armed.
	 */
	static struct max20356_wdt_fixture fixture = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(wdog)),
		.mfd = DEVICE_DT_GET(DT_NODELABEL(pmic58)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic58)),
	};

	zassert_true(device_is_ready(fixture.dev), "wdog not ready");
	zassert_true(device_is_ready(fixture.mfd), "pmic58 not ready");

	return &fixture;
}

static void wdt_before(void *f)
{
	struct max20356_wdt_fixture *fixture = f;

	/* The driver instance and the parent's INTB claim are static and survive
	 * between tests. Drop any lingering event consumer, then force the watchdog
	 * back to the uninstalled/disabled state: setup() engages any timeout a
	 * prior test left installed (no-op otherwise), disable() clears both flags
	 * and releases the claim, and a final release covers the case where setup
	 * never armed. Return values are irrelevant here. Reset the emulator
	 * afterwards so the register file is clean for the test.
	 */
	(void)mfd_max20356_remove_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb);
	(void)wdt_setup(fixture->dev, 0);
	(void)wdt_disable(fixture->dev);
	(void)mfd_max20356_wdt_claim(fixture->mfd, false);

	mfd_max20356_emul_reset(fixture->emul);
}

static int install(const struct device *dev, uint32_t max_ms, uint32_t flags)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = max_ms},
		.callback = NULL,
		.flags = flags,
	};

	return wdt_install_timeout(dev, &cfg);
}

ZTEST_SUITE(max20356_wdt, NULL, wdt_suite_setup, wdt_before, NULL, NULL);

/* window.max maps to the smallest WDTmrSel interval that is >= the request. */
ZTEST_F(max20356_wdt, test_install_interval_mapping)
{
	static const struct {
		uint32_t max_ms;
		uint8_t tmrsel;
	} cases[] = {
		{4000U, 0U}, {8000U, 1U}, {16000U, 2U}, {32000U, 3U},
		{1U, 0U},    {5000U, 1U}, {9000U, 2U},  {17000U, 3U},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint8_t wdcntl;

		zassert_ok(install(fixture->dev, cases[i].max_ms, WDT_FLAG_RESET_SOC),
			   "install %u ms failed", cases[i].max_ms);
		zassert_ok(wdt_setup(fixture->dev, 0));

		mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
		zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDTMRSEL_MSK, wdcntl), cases[i].tmrsel,
			      "%u ms -> WDTmrSel 0x%02x", cases[i].max_ms, wdcntl);

		/* Uninstall so the next iteration can install again. */
		zassert_ok(wdt_disable(fixture->dev));
	}
}

/* A request above the longest interval is out of range. */
ZTEST_F(max20356_wdt, test_install_too_long)
{
	zassert_equal(install(fixture->dev, 33000U, WDT_FLAG_RESET_SOC), -EINVAL);
}

/* Windowed mode is not supported. */
ZTEST_F(max20356_wdt, test_install_window_min_rejected)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 1000U, .max = 8000U},
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_equal(wdt_install_timeout(fixture->dev, &cfg), -EINVAL);
}

/* No pre-expiry warning interrupt exists, so a callback is not supported. */
ZTEST_F(max20356_wdt, test_install_callback_rejected)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = 8000U},
		.callback = (wdt_callback_t)0x1,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_equal(wdt_install_timeout(fixture->dev, &cfg), -ENOTSUP);
}

/* WDT_FLAG_RESET_NONE has no hardware mapping (0b00 means the watchdog is off). */
ZTEST_F(max20356_wdt, test_install_reset_none_rejected)
{
	zassert_equal(install(fixture->dev, 8000U, WDT_FLAG_RESET_NONE), -ENOTSUP);
}

/* The reset flags map to the matching WDRstType after setup. */
ZTEST_F(max20356_wdt, test_reset_type_mapping)
{
	uint8_t wdcntl;

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl), MAX20356_WDT_HARD_RESET,
		      "RESET_SOC -> WDRstType 0x%02x", wdcntl);

	zassert_ok(wdt_disable(fixture->dev));

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_CPU_CORE));
	zassert_ok(wdt_setup(fixture->dev, 0));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl), MAX20356_WDT_SOFT_RESET,
		      "RESET_CPU_CORE -> WDRstType 0x%02x", wdcntl);
}

/* A second install while one is valid is rejected. */
ZTEST_F(max20356_wdt, test_double_install)
{
	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_equal(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC), -ENOMEM);
}

/* setup without an installed timeout fails. */
ZTEST_F(max20356_wdt, test_setup_without_install)
{
	zassert_equal(wdt_setup(fixture->dev, 0), -EINVAL);
}

/* A second setup while enabled is rejected. */
ZTEST_F(max20356_wdt, test_double_setup)
{
	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));
	zassert_equal(wdt_setup(fixture->dev, 0), -EBUSY);
}

/* Pause options cannot be honored by hardware. */
ZTEST_F(max20356_wdt, test_setup_unsupported_options)
{
	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_equal(wdt_setup(fixture->dev, WDT_OPT_PAUSE_IN_SLEEP), -ENOTSUP);
}

/* feed on channel 0 reads (and clears) Int5.WDTmr; a bad channel is rejected. */
ZTEST_F(max20356_wdt, test_feed)
{
	uint8_t int5;

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_INT5, MAX20356_INT5_WDTMR_MSK);
	zassert_ok(wdt_feed(fixture->dev, 0));

	/* Int5 is clear-on-read: the feed consumed the WDTmr pulse. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_INT5, &int5);
	zassert_equal(int5, 0x00, "Int5 not cleared by feed: 0x%02x", int5);

	zassert_equal(wdt_feed(fixture->dev, 1), -EINVAL);
}

/* An armed watchdog owns INTB exclusively: registering an event callback while
 * it runs is refused, and it is allowed again once the watchdog is disabled.
 */
ZTEST_F(max20356_wdt, test_armed_blocks_callback)
{
	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));

	zassert_equal(mfd_max20356_add_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb, NULL),
		      -EBUSY, "callback registered while watchdog armed");

	zassert_ok(wdt_disable(fixture->dev));

	/* Released: registration works again. */
	zassert_ok(mfd_max20356_add_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb, NULL));
	zassert_ok(mfd_max20356_remove_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb));
}

/* If an INTB consumer is already registered, the watchdog cannot arm: it would
 * share the Int5 register with that consumer's dispatch.
 */
ZTEST_F(max20356_wdt, test_setup_refused_when_callback_registered)
{
	zassert_ok(mfd_max20356_add_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb, NULL));

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_equal(wdt_setup(fixture->dev, 0), -EBUSY, "watchdog armed despite live callback");

	zassert_ok(mfd_max20356_remove_callback(fixture->mfd, MAX20356_EVT_MISC, misc_cb));

	/* With the consumer gone, the same timeout can now be armed. */
	zassert_ok(wdt_setup(fixture->dev, 0));
}

/* disable returns WDRstType to off and uninstalls the timeout. */
ZTEST_F(max20356_wdt, test_disable)
{
	uint8_t wdcntl;

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));
	zassert_ok(wdt_disable(fixture->dev));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl), MAX20356_WDT_OFF,
		      "WDRstType not cleared: 0x%02x", wdcntl);

	/* Timeout was uninstalled: a new setup needs a fresh install. */
	zassert_equal(wdt_setup(fixture->dev, 0), -EINVAL);
}

/* disable before setup reports that the instance is not enabled. */
ZTEST_F(max20356_wdt, test_disable_not_enabled)
{
	zassert_equal(wdt_disable(fixture->dev), -EFAULT);
}

/* setup and disable land even when WDCntl is locked, proving the child performs
 * the unlock/write/re-lock sequence around every watchdog write.
 */
ZTEST_F(max20356_wdt, test_locked_domain)
{
	uint8_t wdcntl, unlock;

	mfd_max20356_emul_set_locked3(fixture->emul, MAX20356_LOCKMSK3_WDLCK_MSK, true);

	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));
	zassert_ok(wdt_setup(fixture->dev, 0));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl), MAX20356_WDT_HARD_RESET,
		      "locked setup did not arm: 0x%02x", wdcntl);

	/* The sequence ends by re-locking (last password write is 0xAA). */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK3, &unlock);
	zassert_equal(unlock, 0xAA, "WD domain not re-locked: 0x%02x", unlock);
}

/* A plain (non-helper) write to a locked WDCntl is dropped by the hardware. */
ZTEST_F(max20356_wdt, test_locked_plain_write_dropped)
{
	uint8_t wdcntl;

	mfd_max20356_emul_set_locked3(fixture->emul, MAX20356_LOCKMSK3_WDLCK_MSK, true);

	zassert_ok(mfd_max20356_reg_update(fixture->mfd, MAX20356_REG_WDCNTL,
					   MAX20356_WDCNTL_WDRSTTYPE_MSK,
					   FIELD_PREP(MAX20356_WDCNTL_WDRSTTYPE_MSK,
						      MAX20356_WDT_HARD_RESET)));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(wdcntl, 0x00, "locked WDCntl accepted a plain write: 0x%02x", wdcntl);
}

/* The parent fallback writes the charger/limiter-only reset action (0b01). */
ZTEST_F(max20356_wdt, test_parent_rsttype_fallback)
{
	uint8_t wdcntl;

	zassert_ok(mfd_max20356_wdt_set_rsttype(fixture->mfd, MAX20356_WDT_CHG_LIM_RST));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &wdcntl);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl), MAX20356_WDT_CHG_LIM_RST,
		      "WDRstType fallback: 0x%02x", wdcntl);
}

/* A bus error on the WDCntl path propagates as a negative errno. */
ZTEST_F(max20356_wdt, test_bus_error_propagates)
{
	zassert_ok(install(fixture->dev, 8000U, WDT_FLAG_RESET_SOC));

	mfd_max20356_emul_set_fail(fixture->emul, true);
	zassert_true(wdt_setup(fixture->dev, 0) < 0);
	zassert_true(wdt_feed(fixture->dev, 0) < 0);
	mfd_max20356_emul_set_fail(fixture->emul, false);
}
