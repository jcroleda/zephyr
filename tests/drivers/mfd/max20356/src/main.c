/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 MFD parent + I2C register-model emulator tests.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/mfd/max20356.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

#define PWRCMD_OFF        0xB2U
#define PWRCMD_HARD_RESET 0xC3U
#define PWRCMD_SOFT_RESET 0xD4U
#define PWRCMD_SEAL       0xE5U

struct max20356_fixture {
	const struct device *dev;
	const struct emul *emul;
};

static void *max20356_setup(void)
{
	static struct max20356_fixture fixture = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(pmic)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
	};

	zassert_not_null(fixture.dev);
	zassert_not_null(fixture.emul);
	zassert_true(device_is_ready(fixture.dev), "parent device not ready");

	return &fixture;
}

static void max20356_before(void *f)
{
	struct max20356_fixture *fixture = f;

	mfd_max20356_emul_reset(fixture->emul);
}

ZTEST_SUITE(max20356, NULL, max20356_setup, max20356_before, NULL, NULL);

/* RevID (0x00) is seeded by the emulator reset and read back over I2C. */
ZTEST_F(max20356, test_revid_readback)
{
	uint8_t val = 0;

	zassert_ok(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_REVID, &val));
	zassert_equal(val, 0x01, "unexpected RevID 0x%02x", val);
}

/* A read/write register round-trips through the emulator. */
ZTEST_F(max20356, test_rw_register_roundtrip)
{
	uint8_t val = 0;

	zassert_ok(mfd_max20356_reg_write(fixture->dev, MAX20356_REG_MONCFG, 0xAB));
	zassert_ok(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_MONCFG, &val));
	zassert_equal(val, 0xAB, "MONCfg readback 0x%02x", val);
}

/* Writing a read-only register over I2C is ignored: constant readback. */
ZTEST_F(max20356, test_ro_register_constant)
{
	uint8_t val = 0xFF;

	zassert_ok(mfd_max20356_reg_write(fixture->dev, MAX20356_REG_STATUS0, 0xFF));
	zassert_ok(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_STATUS0, &val));
	zassert_equal(val, 0x00, "RO Status0 should stay 0, got 0x%02x", val);
}

/* The back door seeds a read-only register for status simulation. */
ZTEST_F(max20356, test_ro_register_backdoor_seed)
{
	uint8_t val = 0;

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_STATUS1, 0x5A);
	zassert_ok(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_STATUS1, &val));
	zassert_equal(val, 0x5A, "seeded Status1 readback 0x%02x", val);
}

/* reg_update modifies only the masked bits. */
ZTEST_F(max20356, test_reg_update_masked)
{
	uint8_t val = 0;

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_MONCFG, 0x0F);
	zassert_ok(mfd_max20356_reg_update(fixture->dev, MAX20356_REG_MONCFG, 0xF0, 0xA0));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_MONCFG, &val);
	zassert_equal(val, 0xAF, "masked update gave 0x%02x", val);
}

/* Each power command writes its documented PwrCmd opcode. */
ZTEST_F(max20356, test_power_command)
{
	const struct {
		enum max20356_power_cmd cmd;
		uint8_t opcode;
	} cases[] = {
		{MAX20356_PWR_OFF, PWRCMD_OFF},
		{MAX20356_PWR_HARD_RESET, PWRCMD_HARD_RESET},
		{MAX20356_PWR_SOFT_RESET, PWRCMD_SOFT_RESET},
		{MAX20356_PWR_SEAL, PWRCMD_SEAL},
	};

	ARRAY_FOR_EACH(cases, i) {
		uint8_t val = 0;

		zassert_ok(mfd_max20356_power_command(fixture->dev, cases[i].cmd));
		mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_PWRCMD, &val);
		zassert_equal(val, cases[i].opcode, "cmd %d -> PwrCmd 0x%02x", cases[i].cmd, val);
	}
}

ZTEST_F(max20356, test_power_command_invalid)
{
	zassert_equal(mfd_max20356_power_command(fixture->dev, 0xFF), -EINVAL);
}

/* Monitor-mux selection packs channel and ratio into MONCfg. */
ZTEST_F(max20356, test_mon_select)
{
	uint8_t val = 0;

	zassert_ok(mfd_max20356_mon_select(fixture->dev, 0x0A, 0x02));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_MONCFG, &val);
	zassert_equal(FIELD_GET(MAX20356_MONCFG_MONCTR_MSK, val), 0x0A);
	zassert_equal(FIELD_GET(MAX20356_MONCFG_MONRATIOCFG_MSK, val), 0x02);
}

ZTEST_F(max20356, test_mon_select_invalid)
{
	zassert_equal(mfd_max20356_mon_select(fixture->dev, 0x10, 0), -EINVAL);
	zassert_equal(mfd_max20356_mon_select(fixture->dev, 0, 0x04), -EINVAL);
}

/* Watchdog reset-type helper updates only WDRstType. */
ZTEST_F(max20356, test_wdt_set_rsttype)
{
	uint8_t val = 0;

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_WDCNTL, 0x03);
	zassert_ok(mfd_max20356_wdt_set_rsttype(fixture->dev, MAX20356_WDT_HARD_RESET));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_WDCNTL, &val);
	zassert_equal(FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, val), MAX20356_WDT_HARD_RESET);
	zassert_equal(val & ~MAX20356_WDCNTL_WDRSTTYPE_MSK, 0x03,
		      "unrelated WDCntl bits changed: 0x%02x", val);
}

/* Variant comes from the devicetree compatible. */
ZTEST_F(max20356, test_variant)
{
	zassert_equal(mfd_max20356_get_variant(fixture->dev), MAX20356_VARIANT_MAX20356);
}

/* Without CONFIG_MFD_MAX20356_TRIGGER the callback API reports -ENOTSUP. */
ZTEST_F(max20356, test_callbacks_not_supported)
{
	zassert_equal(mfd_max20356_add_callback(fixture->dev, MAX20356_EVT_CHARGER, NULL, NULL),
		      -ENOTSUP);
	zassert_equal(mfd_max20356_remove_callback(fixture->dev, MAX20356_EVT_CHARGER, NULL),
		      -ENOTSUP);
}

/* A bus error propagates as a negative errno through every access helper. */
ZTEST_F(max20356, test_bus_error_propagates)
{
	uint8_t val = 0;

	mfd_max20356_emul_set_fail(fixture->emul, true);

	zassert_true(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_REVID, &val) < 0);
	zassert_true(mfd_max20356_reg_write(fixture->dev, MAX20356_REG_MONCFG, 0x11) < 0);
	zassert_true(mfd_max20356_reg_update(fixture->dev, MAX20356_REG_MONCFG, 0x0F, 0x01) < 0);
	zassert_true(mfd_max20356_power_command(fixture->dev, MAX20356_PWR_OFF) < 0);

	mfd_max20356_emul_set_fail(fixture->emul, false);
	zassert_ok(mfd_max20356_reg_read(fixture->dev, MAX20356_REG_REVID, &val));
}

/* The MAX20358 instance reports its variant from the compatible string. */
ZTEST(max20356, test_variant_max20358)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(pmic58));

	zassert_true(device_is_ready(dev), "max20358 device not ready");
	zassert_equal(mfd_max20356_get_variant(dev), MAX20356_VARIANT_MAX20358);
}
