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
#include <zephyr/dt-bindings/mfd/max20356.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"
#include "spi_dvs_emul.h"

/* Snapshot of the DT-configured init registers, captured once right after the
 * regulator rails' POST_KERNEL init and before any test suite resets the
 * emulator. test_init_config() asserts against these (see boot_snapshot_init).
 */
static struct {
	uint8_t buck1ena;
	uint8_t buck1cfg0;
	uint8_t buck2dvscfg0;
	uint8_t buck3cfg0;
	uint8_t bbstcfg;
	uint8_t bbstcfg1;
	uint8_t ldo3ena;
	uint8_t ldo3cfg;
	uint8_t lsw1cfg;
	uint8_t buck1ctr;
	uint8_t buck3ctr;
	uint8_t buck3ena;
	uint8_t lsw1ctr;
	uint8_t lsw1ena;
} boot_snapshot;

static int boot_snapshot_init(void)
{
	const struct emul *emul = EMUL_DT_GET(DT_NODELABEL(pmic));

	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK1ENA, &boot_snapshot.buck1ena);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK1CFG0, &boot_snapshot.buck1cfg0);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK2DVSCFG0, &boot_snapshot.buck2dvscfg0);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK3CFG, &boot_snapshot.buck3cfg0);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BBSTCFG, &boot_snapshot.bbstcfg);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BBSTCFG1, &boot_snapshot.bbstcfg1);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_LDO3ENA, &boot_snapshot.ldo3ena);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_LDO3CFG, &boot_snapshot.ldo3cfg);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_LSW1CFG, &boot_snapshot.lsw1cfg);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK1CTR, &boot_snapshot.buck1ctr);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK3CTR, &boot_snapshot.buck3ctr);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BUCK3ENA, &boot_snapshot.buck3ena);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_LSW1CTR, &boot_snapshot.lsw1ctr);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_LSW1ENA, &boot_snapshot.lsw1ena);

	return 0;
}
/* APPLICATION level runs after POST_KERNEL device init, before ztest starts. */
SYS_INIT(boot_snapshot_init, APPLICATION, 0);

struct max20356_reg_fixture {
	const struct device *buck1;
	const struct device *buck2;
	const struct device *buck3;
	const struct device *bbst;
	const struct device *ldo1;
	const struct device *ldo3;
	const struct device *ldo4;
	const struct device *lsw1;
	const struct emul *emul;
};

static void *reg_setup(void)
{
	static struct max20356_reg_fixture fixture = {
		.buck1 = DEVICE_DT_GET(DT_NODELABEL(reg_buck1)),
		.buck2 = DEVICE_DT_GET(DT_NODELABEL(reg_buck2)),
		.buck3 = DEVICE_DT_GET(DT_NODELABEL(reg_buck3)),
		.bbst = DEVICE_DT_GET(DT_NODELABEL(reg_bbst)),
		.ldo1 = DEVICE_DT_GET(DT_NODELABEL(reg_ldo1)),
		.ldo3 = DEVICE_DT_GET(DT_NODELABEL(reg_ldo3)),
		.ldo4 = DEVICE_DT_GET(DT_NODELABEL(reg_ldo4)),
		.lsw1 = DEVICE_DT_GET(DT_NODELABEL(reg_lsw1)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
	};

	zassert_true(device_is_ready(fixture.buck1), "buck1 not ready");
	zassert_true(device_is_ready(fixture.buck2), "buck2 not ready");
	zassert_true(device_is_ready(fixture.buck3), "buck3 not ready");
	zassert_true(device_is_ready(fixture.bbst), "buckboost not ready");
	zassert_true(device_is_ready(fixture.ldo1), "ldo1 not ready");
	zassert_true(device_is_ready(fixture.ldo3), "ldo3 not ready");
	zassert_true(device_is_ready(fixture.ldo4), "ldo4 not ready");
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

/* Buck-boost set/get voltage round-trips (2.5V base, 50mV step). */
ZTEST_F(max20356_reg, test_buckboost_voltage)
{
	int32_t volt;
	uint8_t reg;

	zassert_ok(regulator_set_voltage(fixture->bbst, 3300000, 3300000));

	/* idx = (3.3V - 2.5V) / 50mV = 16. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BBSTVSET, &reg);
	zassert_equal(FIELD_GET(MAX20356_BBSTVSET_BBSTVSET_MSK, reg), 16, "BBstVSet = 0x%02x", reg);

	zassert_ok(regulator_get_voltage(fixture->bbst, &volt));
	zassert_equal(volt, 3300000, "buck-boost voltage = %d", volt);
}

/* Buck-boost current limit maps to BBstIPSet1 (0-375mA, 25mA steps). */
ZTEST_F(max20356_reg, test_buckboost_current_limit)
{
	int32_t curr;

	zassert_ok(regulator_set_current_limit(fixture->bbst, 250000, 250000));
	zassert_ok(regulator_get_current_limit(fixture->bbst, &curr));
	zassert_equal(curr, 250000, "buck-boost current limit = %d", curr);
}

/* LDO3 set/get voltage round-trips (0.9V base, 25mV step, 7-bit field). */
ZTEST_F(max20356_reg, test_ldo3_voltage)
{
	int32_t volt;
	uint8_t reg;

	zassert_ok(regulator_set_voltage(fixture->ldo3, 1800000, 1800000));

	/* idx = (1.8V - 0.9V) / 25mV = 36. */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LDO3VSET, &reg);
	zassert_equal(FIELD_GET(MAX20356_LDO3VSET_LDO3VSET_MSK, reg), 36, "LDO3VSet = 0x%02x", reg);

	zassert_ok(regulator_get_voltage(fixture->ldo3, &volt));
	zassert_equal(volt, 1800000, "ldo3 voltage = %d", volt);
}

/* LDO4 (RTC) uses the discrete base/increment table. */
ZTEST_F(max20356_reg, test_ldo4_voltage)
{
	int32_t volt;
	uint8_t reg;

	/* 1.825V => base 1.8V (LDO4VSet=1) + 25mV increment (LDO4VInc=1). */
	zassert_ok(regulator_set_voltage(fixture->ldo4, 1825000, 1825000));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LDO4CFG, &reg);
	zassert_true((reg & MAX20356_LDO4CFG_LDO4VSET_MSK) != 0U, "LDO4VSet not 1.8V base");
	zassert_equal(FIELD_GET(MAX20356_LDO4CFG_LDO4VINC_MSK, reg), 1, "LDO4VInc = %u",
		      FIELD_GET(MAX20356_LDO4CFG_LDO4VINC_MSK, reg));

	zassert_ok(regulator_get_voltage(fixture->ldo4, &volt));
	zassert_equal(volt, 1825000, "ldo4 voltage = %d", volt);
}

/* LDO4 rejects a voltage that is not in its discrete table. */
ZTEST_F(max20356_reg, test_ldo4_voltage_invalid)
{
	zassert_equal(regulator_set_voltage(fixture->ldo4, 1400000, 1450000), -EINVAL);
}

/* LDO4 exposes its discrete voltage list. */
ZTEST_F(max20356_reg, test_ldo4_count_list_voltage)
{
	int32_t volt;

	zassert_equal(regulator_count_voltages(fixture->ldo4), 6);
	zassert_ok(regulator_list_voltage(fixture->ldo4, 0, &volt));
	zassert_equal(volt, 1200000, "index 0 => %d", volt);
	zassert_ok(regulator_list_voltage(fixture->ldo4, 3, &volt));
	zassert_equal(volt, 1800000, "index 3 => %d", volt);
	zassert_equal(regulator_list_voltage(fixture->ldo4, 6, &volt), -EINVAL);
}

/* Buck count/list voltages reflect the 64-code VSet range. */
ZTEST_F(max20356_reg, test_buck_count_list_voltage)
{
	int32_t volt;

	zassert_equal(regulator_count_voltages(fixture->buck1), 64);
	zassert_ok(regulator_list_voltage(fixture->buck1, 0, &volt));
	zassert_equal(volt, 500000, "index 0 => %d", volt);
}

/* LDO1 supports LDO vs load-switch mode selection. */
ZTEST_F(max20356_reg, test_ldo1_mode)
{
	regulator_mode_t mode;

	zassert_ok(regulator_set_mode(fixture->ldo1, MAX20356_MODE_LOAD_SWITCH));
	zassert_ok(regulator_get_mode(fixture->ldo1, &mode));
	zassert_equal(mode, MAX20356_MODE_LOAD_SWITCH);

	zassert_ok(regulator_set_mode(fixture->ldo1, MAX20356_MODE_LDO));
	zassert_ok(regulator_get_mode(fixture->ldo1, &mode));
	zassert_equal(mode, MAX20356_MODE_LDO);

	/* An out-of-range mode is rejected. */
	zassert_equal(regulator_set_mode(fixture->ldo1, 5), -ENOTSUP);
}

/* Bucks have no mode field. */
ZTEST_F(max20356_reg, test_buck_no_mode)
{
	regulator_mode_t mode;

	zassert_equal(regulator_set_mode(fixture->buck1, MAX20356_MODE_LDO), -ENOTSUP);
	zassert_equal(regulator_get_mode(fixture->buck1, &mode), -ENOTSUP);
}

/* Active discharge round-trips on a buck (BuckCfg.ActDsc). */
ZTEST_F(max20356_reg, test_active_discharge)
{
	bool ad;

	zassert_ok(regulator_set_active_discharge(fixture->buck1, true));
	zassert_ok(regulator_get_active_discharge(fixture->buck1, &ad));
	zassert_true(ad, "active discharge not set");

	zassert_ok(regulator_set_active_discharge(fixture->buck1, false));
	zassert_ok(regulator_get_active_discharge(fixture->buck1, &ad));
	zassert_false(ad, "active discharge not cleared");
}

/* Out-of-window voltage requests are rejected. */
ZTEST_F(max20356_reg, test_set_voltage_out_of_range)
{
	zassert_equal(regulator_set_voltage(fixture->buck1, 100000, 200000), -EINVAL);
	zassert_equal(regulator_set_current_limit(fixture->buck1, 500000, 600000), -EINVAL);
}

/* A bus error propagates out of the voltage accessors. */
ZTEST_F(max20356_reg, test_bus_error_propagates)
{
	int32_t volt;

	mfd_max20356_emul_set_fail(fixture->emul, true);

	zassert_true(regulator_set_voltage(fixture->buck1, 1000000, 1000000) < 0);
	zassert_true(regulator_get_voltage(fixture->buck1, &volt) < 0);
	zassert_true(regulator_enable(fixture->buck1) < 0);

	mfd_max20356_emul_set_fail(fixture->emul, false);
}

/* The DT-configured init block (registers 0x30..0x71) is applied at boot: only
 * present properties are written, unset fields keep their reset value, and the
 * masked writes coexist with the enable/DVS fields in the same registers. Uses
 * the boot snapshot because the per-suite emulator resets wipe these writes.
 */
ZTEST(max20356_reg, test_init_config)
{
	/* buck1: sequencing slot set to 3, enable field left untouched. */
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1SEQ_MSK, boot_snapshot.buck1ena), 3,
		      "buck1 Seq = %u", FIELD_GET(MAX20356_BUCK1ENA_BUCK1SEQ_MSK,
						  boot_snapshot.buck1ena));
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1EN_MSK, boot_snapshot.buck1ena), 0,
		      "buck1 En disturbed by Seq write");

	/* buck1 Cfg0: LowEMI and FPWM requested; FPWM lives in Cfg1, LowEMI in
	 * Cfg0. Only the two requested bits should be set; an unconfigured Cfg0
	 * bit (Fast) must stay clear.
	 */
	zassert_true((boot_snapshot.buck1cfg0 & MAX20356_BUCK1CFG0_BUCK1LOWEMI_MSK) != 0U,
		     "buck1 LowEMI not set");
	zassert_true((boot_snapshot.buck1cfg0 & MAX20356_BUCK1CFG0_BUCK1FAST_MSK) == 0U,
		     "buck1 Fast set but not requested");

	/* buck2 DvsCfg0: DvsIpMax[5] from the init block AND the GPIO DvsCfg[4:0]
	 * code from dvs_init both survive (masked writes to disjoint bits).
	 */
	zassert_true((boot_snapshot.buck2dvscfg0 & MAX20356_BUCK2DVSCFG0_BUCK2DVSIPMAX_MSK) != 0U,
		     "buck2 DvsIpMax not set");
	zassert_true(FIELD_GET(MAX20356_BUCK2DVSCFG0_BUCK2DVSCFG_MSK, boot_snapshot.buck2dvscfg0) !=
			     0U,
		     "buck2 GPIO DvsCfg code clobbered by DvsIpMax write");

	/* buck3: no adi,buck-* Cfg0 property in the overlay, so the register is
	 * untouched (write-only-if-present).
	 */
	zassert_equal(boot_snapshot.buck3cfg0, 0, "buck3 Cfg0 written but nothing requested");

	/* buck-boost: Cfg LowEMI bool, Cfg1 Fast bool, and the 2-bit fHIGH enum. */
	zassert_true((boot_snapshot.bbstcfg & MAX20356_BBSTCFG_BBSTLOWEMI_MSK) != 0U,
		     "bbst LowEMI not set");
	zassert_true((boot_snapshot.bbstcfg1 & MAX20356_BBSTCFG1_BBSTFAST_MSK) != 0U,
		     "bbst Fast not set");
	zassert_equal(FIELD_GET(MAX20356_BBSTCFG1_BBFHIGHSH_MSK, boot_snapshot.bbstcfg1), 2,
		      "bbst fHIGH threshold = %u",
		      FIELD_GET(MAX20356_BBSTCFG1_BBFHIGHSH_MSK, boot_snapshot.bbstcfg1));

	/* LDO3: sequencing slot 5 and two Cfg bools. */
	zassert_equal(FIELD_GET(MAX20356_LDO3ENA_LDO3SEQ_MSK, boot_snapshot.ldo3ena), 5,
		      "ldo3 Seq = %u",
		      FIELD_GET(MAX20356_LDO3ENA_LDO3SEQ_MSK, boot_snapshot.ldo3ena));
	zassert_true((boot_snapshot.ldo3cfg & MAX20356_LDO3CFG_LDO3_NOCLP_MSK) != 0U,
		     "ldo3 NOCLP not set");
	zassert_true((boot_snapshot.ldo3cfg & MAX20356_LDO3CFG_LDO3PSVDSC_MSK) != 0U,
		     "ldo3 PsvDsc not set");

	/* LSW1: non-lockable rail, LowIq set through the plain update path. */
	zassert_true((boot_snapshot.lsw1cfg & MAX20356_LSW1CFG_LSW1LOWIQ_MSK) != 0U,
		     "lsw1 LowIq not set");
}

/* adi,mpc-enable-map routes MPC6 -> LSW1 and MPC7 -> BUCK3: each routed rail
 * gets exactly its pin's <rail>Ctr bit and its enable field set to controlled-by-
 * MPC (En = 10). An unrouted rail (buck1) keeps a zero Ctr and is not forced into
 * MPC mode. Uses the boot snapshot (these are init-time writes).
 */
ZTEST(max20356_reg, test_mpc_enable_map)
{
	/* LSW1 (non-lockable path): only MPC6 routed, enable = controlled-by-MPC. */
	zassert_equal(boot_snapshot.lsw1ctr, BIT(6), "lsw1 Ctr = 0x%02x", boot_snapshot.lsw1ctr);
	zassert_equal(FIELD_GET(MAX20356_LSW1ENA_LSW1EN_MSK, boot_snapshot.lsw1ena), 0x2,
		      "lsw1 En not controlled-by-MPC (= %u)",
		      FIELD_GET(MAX20356_LSW1ENA_LSW1EN_MSK, boot_snapshot.lsw1ena));

	/* BUCK3 (lockable path): only MPC7 routed, enable = controlled-by-MPC. */
	zassert_equal(boot_snapshot.buck3ctr, BIT(7), "buck3 Ctr = 0x%02x", boot_snapshot.buck3ctr);
	zassert_equal(FIELD_GET(MAX20356_BUCK3ENA_BUCK3EN_MSK, boot_snapshot.buck3ena), 0x2,
		      "buck3 En not controlled-by-MPC (= %u)",
		      FIELD_GET(MAX20356_BUCK3ENA_BUCK3EN_MSK, boot_snapshot.buck3ena));

	/* buck1 is unrouted: no Ctr bit, enable field left alone (still disabled). */
	zassert_equal(boot_snapshot.buck1ctr, 0, "buck1 Ctr written but unrouted");
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1EN_MSK, boot_snapshot.buck1ena), 0,
		      "buck1 En forced but unrouted");
}

/* adi,lock-enable = <1> (buck1 default): a write to a rail whose lock domain is
 * engaged still lands, because the driver runs the unlock/write/re-lock password
 * sequence around it. The domain is re-locked afterwards.
 */
ZTEST_F(max20356_reg, test_lock_enable_uses_password_path)
{
	uint8_t vset, unlock;

	mfd_max20356_emul_set_locked(fixture->emul, MAX20356_LOCKMSK1_BK1LCK_MSK, true);

	zassert_ok(regulator_set_voltage(fixture->buck1, 1000000, 1000000));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BUCK1VSET, &vset);
	zassert_equal(FIELD_GET(MAX20356_BUCK1VSET_BUCK1VSET_MSK, vset), 20,
		      "buck1 VSet not written through the password path: 0x%02x", vset);

	/* The sequence ends by re-locking (last LockUnlock1 write is 0xAA). */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK1, &unlock);
	zassert_equal(unlock, 0xAA, "buck1 domain not re-locked: 0x%02x", unlock);

	mfd_max20356_emul_set_locked(fixture->emul, MAX20356_LOCKMSK1_BK1LCK_MSK, false);
}

/* adi,lock-enable = <0> (ldo1): the driver writes directly, without the password
 * sequence. When the rail's lock domain is engaged the write is dropped by the
 * hardware; when the domain is clear the write lands. Either way the driver never
 * touches LockUnlock1.
 */
ZTEST_F(max20356_reg, test_lock_disable_uses_direct_path)
{
	uint8_t vset, unlock;

	/* Domain engaged: a direct write is dropped, and LockUnlock1 is untouched. */
	mfd_max20356_emul_set_locked(fixture->emul, MAX20356_LOCKMSK1_LD1LCK_MSK, true);
	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK1, 0x00);

	zassert_ok(regulator_set_voltage(fixture->ldo1, 1300000, 1300000));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LDO1VSET, &vset);
	zassert_equal(vset, 0x00, "locked ldo1 accepted a direct write: 0x%02x", vset);
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK1, &unlock);
	zassert_equal(unlock, 0x00, "direct path issued a password write: 0x%02x", unlock);

	/* Domain clear: the same direct write lands. */
	mfd_max20356_emul_set_locked(fixture->emul, MAX20356_LOCKMSK1_LD1LCK_MSK, false);

	zassert_ok(regulator_set_voltage(fixture->ldo1, 1300000, 1300000));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LDO1VSET, &vset);
	zassert_equal(FIELD_GET(MAX20356_LDO1VSET_LDO1VSET_MSK, vset), 4,
		      "unlocked ldo1 direct write did not land: 0x%02x", vset);
}
