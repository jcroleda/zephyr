/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 MFD parent + I2C register-model emulator tests.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/mfd/max20356.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

#define PWRCMD_OFF        0xB2U
#define PWRCMD_HARD_RESET 0xC3U
#define PWRCMD_SOFT_RESET 0xD4U
#define PWRCMD_SEAL       0xE5U

/* Device-level init writes (PwrCfg, MiscFunctions) happen at MFD init, before
 * ztest starts and before the per-test emulator reset. Snapshot them at boot so
 * test_init_config() can assert against them. See test_init_config.
 */
static struct {
	uint8_t pwrcfg;
	uint8_t miscfunctions;
	uint8_t ilimctrl1;
	uint8_t ilimctrl2;
	uint8_t dropctrl;
	uint8_t bootcfg;
	uint8_t moncfg;
} boot_snapshot;

static int boot_snapshot_init(void)
{
	const struct emul *emul = EMUL_DT_GET(DT_NODELABEL(pmic));

	mfd_max20356_emul_get_reg(emul, MAX20356_REG_PWRCFG, &boot_snapshot.pwrcfg);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_MISCFUNCTIONS, &boot_snapshot.miscfunctions);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_ILIMCTRL1, &boot_snapshot.ilimctrl1);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_ILIMCTRL2, &boot_snapshot.ilimctrl2);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_DROPCTRL, &boot_snapshot.dropctrl);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_BOOTCFG, &boot_snapshot.bootcfg);
	mfd_max20356_emul_get_reg(emul, MAX20356_REG_MONCFG, &boot_snapshot.moncfg);

	return 0;
}

/* APPLICATION level runs after POST_KERNEL device init, before ztest starts. */
SYS_INIT(boot_snapshot_init, APPLICATION, 0);

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

/* The device-level init block writes only the fields whose properties are
 * present in the overlay. The pmic node sets adi,intb-unmasked-in-shutdown,
 * adi,rtc-ldo-off and adi,factory-mode-disabled; adi,stay-on and
 * adi,active-discharge-constant are absent and must stay at 0.
 */
ZTEST(max20356, test_init_config)
{
	zassert_true((boot_snapshot.pwrcfg & MAX20356_PWRCFG_INTBOOTMSK_MSK) != 0U,
		     "INTBootMsk not set: PwrCfg 0x%02x", boot_snapshot.pwrcfg);
	zassert_true((boot_snapshot.pwrcfg & MAX20356_PWRCFG_STAYON_MSK) == 0U,
		     "StayOn set but not requested: PwrCfg 0x%02x", boot_snapshot.pwrcfg);

	zassert_true((boot_snapshot.miscfunctions & MAX20356_MISCFUNCTIONS_RTCLDOOFF_MSK) != 0U,
		     "RTC-LDO-off not set: MiscFunctions 0x%02x", boot_snapshot.miscfunctions);
	zassert_true((boot_snapshot.miscfunctions & MAX20356_MISCFUNCTIONS_FACTORYMODEDIS_MSK) != 0U,
		     "factory-mode-disabled not set: MiscFunctions 0x%02x",
		     boot_snapshot.miscfunctions);
	zassert_true((boot_snapshot.miscfunctions & MAX20356_MISCFUNCTIONS_DISCHARGECONST_MSK) == 0U,
		     "active-discharge-constant set but not requested: MiscFunctions 0x%02x",
		     boot_snapshot.miscfunctions);
}

/* The input-limiter / SYS-regulation init block writes only the fields whose
 * properties are present. The overlay sets adi,input-current-limit-milliamp =
 * 300mA (ILimCntl index 3), adi,sys-min-voltage-microvolt = 3.6V (SysMinVlt
 * index 3), adi,sys-uvlo-threshold-microvolt = 3.0V (SysUVLOThSel index 2). The
 * blanking, max-limit and discharge fields are absent and stay at 0.
 */
ZTEST(max20356, test_init_config_ilim)
{
	zassert_equal(FIELD_GET(MAX20356_ILIMCTRL1_ILIMCNTL_MSK, boot_snapshot.ilimctrl1), 3,
		      "ILimCntl: ILimCtrl1 0x%02x", boot_snapshot.ilimctrl1);
	zassert_equal(FIELD_GET(MAX20356_ILIMCTRL1_ILIMMAX_MSK, boot_snapshot.ilimctrl1), 0,
		      "ILimMax set but not requested: ILimCtrl1 0x%02x", boot_snapshot.ilimctrl1);
	zassert_equal(FIELD_GET(MAX20356_ILIMCTRL2_SYSMINVLT_MSK, boot_snapshot.ilimctrl2), 3,
		      "SysMinVlt: ILimCtrl2 0x%02x", boot_snapshot.ilimctrl2);
	zassert_equal(FIELD_GET(MAX20356_DROPCTRL_SYSUVLOTHSEL_MSK, boot_snapshot.dropctrl), 2,
		      "SysUVLOThSel: DropCtrl 0x%02x", boot_snapshot.dropctrl);
}

/* The boot-config init block writes only present fields. The overlay sets
 * adi,power-reset-config = 5 (PwrRstCfg) and adi,boot-delay = 2 (BootDly); the
 * two boolean bits are absent and stay at 0.
 */
ZTEST(max20356, test_init_config_bootcfg)
{
	zassert_equal(FIELD_GET(MAX20356_BOOTCFG_PWRRSTCFG_MSK, boot_snapshot.bootcfg), 5,
		      "PwrRstCfg: BootCfg 0x%02x", boot_snapshot.bootcfg);
	zassert_equal(FIELD_GET(MAX20356_BOOTCFG_BOOTDLY_MSK, boot_snapshot.bootcfg), 2,
		      "BootDly: BootCfg 0x%02x", boot_snapshot.bootcfg);
	zassert_true((boot_snapshot.bootcfg & MAX20356_BOOTCFG_CHGALWTRY_MSK) == 0U,
		     "ChgAlwTry set but not requested: BootCfg 0x%02x", boot_snapshot.bootcfg);
}

/* The overlay sets adi,ivmon-high-impedance, so MONCfg.MONHiZ is set at init. */
ZTEST(max20356, test_init_config_ivmon_hiz)
{
	zassert_true((boot_snapshot.moncfg & MAX20356_MONCFG_MONHIZ_MSK) != 0U,
		     "MONHiZ not set: MONCfg 0x%02x", boot_snapshot.moncfg);
}

/* PFN pin status reflects the PFN register bits. */
ZTEST_F(max20356, test_pfn_status)
{
	bool active;

	mfd_max20356_emul_set_reg(fixture->emul, MAX20356_REG_PFN, MAX20356_PFN_PFN1PIN_MSK);

	zassert_ok(mfd_max20356_pfn_status(fixture->dev, MAX20356_PFN1, &active));
	zassert_true(active, "PFN1 should read asserted");

	zassert_ok(mfd_max20356_pfn_status(fixture->dev, MAX20356_PFN2, &active));
	zassert_false(active, "PFN2 should read deasserted");
}

ZTEST_F(max20356, test_pfn_status_invalid)
{
	bool active;

	zassert_equal(mfd_max20356_pfn_status(fixture->dev, MAX20356_PFN1, NULL), -EINVAL);
	zassert_equal(mfd_max20356_pfn_status(fixture->dev, 0xFF, &active), -EINVAL);
}

/* MPC interrupt routing programs the source's ITRCfg select and enable bits. */
ZTEST_F(max20356, test_mpc_int_route)
{
	uint8_t val;

	zassert_ok(mfd_max20356_mpc_int_route(fixture->dev, MAX20356_MPC_INT_BUCK1_PGOOD,
					      BIT(4), true));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BK1ITRCFG, &val);
	zassert_equal(val, BIT(4) | MAX20356_BK1ITRCFG_BK1PGMPCINT_MSK,
		      "BK1ITRCfg = 0x%02x", val);

	zassert_ok(mfd_max20356_mpc_int_route(fixture->dev, MAX20356_MPC_INT_USBOK, BIT(0),
					      false));
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_USBOKITRCFG, &val);
	zassert_equal(val, BIT(0), "USBOKITRCfg = 0x%02x", val);
}

ZTEST_F(max20356, test_mpc_int_route_invalid)
{
	zassert_equal(mfd_max20356_mpc_int_route(fixture->dev, 0xFF, BIT(0), false), -EINVAL);
}
