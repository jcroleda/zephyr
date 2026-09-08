/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 regulator tests for the DVS modes, current-limit API and the locked
 * register access they rely on. buck1 uses I2C DVS (Mode 0); buck2 is configured
 * for GPIO DVS (Mode 1) in the overlay so its DvsCfg/DvsVlt/DvsCur programming is
 * checked after init.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"
#include "spi_dvs_emul.h"

struct max20356_reg_fixture {
	const struct device *buck1;
	const struct device *buck2;
	const struct device *buck3;
	const struct device *lsw1;
	const struct emul *emul;
};

static void *reg_setup(void)
{
	static struct max20356_reg_fixture fixture = {
		.buck1 = DEVICE_DT_GET(DT_NODELABEL(reg_buck1)),
		.buck2 = DEVICE_DT_GET(DT_NODELABEL(reg_buck2)),
		.buck3 = DEVICE_DT_GET(DT_NODELABEL(reg_buck3)),
		.lsw1 = DEVICE_DT_GET(DT_NODELABEL(reg_lsw1)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
	};

	zassert_true(device_is_ready(fixture.buck1), "buck1 not ready");
	zassert_true(device_is_ready(fixture.buck2), "buck2 not ready");
	zassert_true(device_is_ready(fixture.buck3), "buck3 not ready");
	zassert_true(device_is_ready(fixture.lsw1), "lsw1 not ready");

	spi_dvs_emul_bind(fixture.emul);

	return &fixture;
}

ZTEST_SUITE(max20356_reg, NULL, reg_setup, NULL, NULL, NULL);

/* GPIO-mode set_voltage is rejected: the live preset is chosen by MPC pins. */
ZTEST_F(max20356_reg, test_gpio_dvs_set_voltage_rejected)
{
	zassert_equal(regulator_set_voltage(fixture->buck2, 1000000, 1000000), -ENOTSUP);
}

/* SPI-mode (buck3) set_voltage clocks a {AD=2, VLT} command byte. */
ZTEST_F(max20356_reg, test_spi_dvs_set_voltage)
{
	uint8_t cmd;

	spi_dvs_emul_bind(fixture->emul);

	/* 1.00V on a 25mV/0.5V-base buck => VLT index 20 (0x14). */
	zassert_ok(regulator_set_voltage(fixture->buck3, 1000000, 1000000));
	zassert_true(spi_dvs_emul_got_cmd(), "no SPI DVS command clocked");

	cmd = spi_dvs_emul_last_cmd();
	zassert_equal(cmd >> 6, 0x2, "AD bits => buck3 (2), got %u", cmd >> 6);
	zassert_equal(cmd & 0x3F, 0x14, "VLT bits = 0x%02x", cmd & 0x3F);
}

/* SPI-mode get_voltage reads the mirrored Buck3DvsSpi register over I2C. */
ZTEST_F(max20356_reg, test_spi_dvs_get_voltage)
{
	int32_t volt;

	spi_dvs_emul_bind(fixture->emul);

	zassert_ok(regulator_set_voltage(fixture->buck3, 1100000, 1100000));
	zassert_ok(regulator_get_voltage(fixture->buck3, &volt));
	zassert_equal(volt, 1100000, "buck3 SPI voltage readback = %d", volt);
}

/* I2C-mode (buck1) set/get voltage round-trips through the locked VSET write. */
ZTEST_F(max20356_reg, test_i2c_set_get_voltage)
{
	int32_t volt;

	zassert_ok(regulator_set_voltage(fixture->buck1, 1000000, 1000000));
	zassert_ok(regulator_get_voltage(fixture->buck1, &volt));
	zassert_equal(volt, 1000000, "buck1 voltage = %d", volt);
}

/* Current-limit set/get round-trips on a buck (ISet, 25mA steps). */
ZTEST_F(max20356_reg, test_buck_current_limit)
{
	int32_t curr;

	zassert_ok(regulator_set_current_limit(fixture->buck1, 200000, 200000));
	zassert_ok(regulator_get_current_limit(fixture->buck1, &curr));
	zassert_equal(curr, 200000, "buck1 current limit = %d", curr);
}

/* Current-limit count/list reflect the 16-code ISet range. */
ZTEST_F(max20356_reg, test_buck_current_limit_list)
{
	int32_t curr;

	zassert_equal(regulator_count_current_limits(fixture->buck1), 16);
	zassert_ok(regulator_list_current_limit(fixture->buck1, 4, &curr));
	zassert_equal(curr, 100000, "index 4 => %d", curr);
}

/* Load switches expose neither voltage nor current-limit control. */
ZTEST_F(max20356_reg, test_lsw_no_voltage_no_current)
{
	int32_t val;

	zassert_equal(regulator_set_voltage(fixture->lsw1, 1000000, 1000000), -ENOTSUP);
	zassert_equal(regulator_get_voltage(fixture->lsw1, &val), -ENOTSUP);
	zassert_equal(regulator_count_current_limits(fixture->lsw1), 0);
}

/* enable/disable land on the buck via the locked En field. */
ZTEST_F(max20356_reg, test_enable_disable)
{
	uint8_t ena;

	zassert_ok(regulator_enable(fixture->buck1));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BUCK1ENA, &ena);
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1EN_MSK, ena), 0x1, "buck1 not enabled");

	zassert_ok(regulator_disable(fixture->buck1));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BUCK1ENA, &ena);
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1EN_MSK, ena), 0x0, "buck1 not disabled");
}
