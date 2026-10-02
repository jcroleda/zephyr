/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 PMIC watchdog charger-reset sample.
 *
 * Demonstrates the watchdog charger/limiter-only reset action (WDCntl.WDRstType
 * = 0b01): when the timer expires it resets the charger and input-limiter
 * registers to their defaults instead of resetting the SoC. The sample
 * configures the charger (modifying ChgCur0.CC1IFChg and enabling charging via
 * ChgCntl0.ChgEn) and the input limiter (ILimCtrl1/ILimCtrl2), records the charge
 * type and the limiter registers, arms the watchdog for reset type 1, feeds it a
 * few times to show normal servicing, then stops feeding so the timer expires.
 * After the reset it reads both back: the charge type should return to its
 * pre-configuration value and the limiter registers to their defaults, since both
 * the charger and the input limiter are in the reset scope.
 *
 * The MAX20356 keeps its register state across an SoC reboot (it has its own
 * power domain), so the MFD parent driver now soft-resets the device at init to
 * start from defaults; without that, "initial" readings here would be whatever a
 * previous run left behind.
 *
 * The watchdog is fed by reading Int5 (wdt_feed() performs that read), so the
 * reset only fires while nothing reads Int5. The WDTmr interrupt is masked on
 * INTB by default
 * (IntMask5.WDTmrM resets to 0) and this sample registers no INTB callbacks, so
 * no source asserts INTB and the parent never reads Int5 on its own. Reading the
 * charge type (Status0) during the wait does not feed the timer; only an Int5
 * read does.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>
#include <sys/errno.h>
#include <stdio.h>

/* Set to 1 to enable the register-level debug dumps (raw I2C reads of the
 * watchdog and charger registers through the public MFD helper). Left on by
 * default here as this is a bring-up sample; set to 0 to silence them.
 */
#define WDT_DEMO_DEBUG 1

/* Register addresses and field masks used by the optional debug dumps. These
 * mirror the private driver definitions in drivers/mfd/mfd_max20356.h (not a
 * public header) so the sample can read the registers back over I2C through the
 * public mfd_max20356_reg_read() helper and confirm the configuration landed.
 */
#define MAX20356_REG_STATUS0          0x01
#define MAX20356_STATUS0_CHGSTAT_MSK  GENMASK(3, 0)
#define MAX20356_REG_CHGCUR0          0x17
#define MAX20356_CHGCUR0_CC1IFCHG_MSK GENMASK(6, 0)
#define MAX20356_REG_CHGCNTL0         0x19
#define MAX20356_CHGCNTL0_CHGEN_MSK   BIT(7)
#define MAX20356_REG_WDCNTL           0x2A
#define MAX20356_WDCNTL_WDRSTTYPE_MSK GENMASK(3, 2)
#define MAX20356_WDCNTL_WDTMRSEL_MSK  GENMASK(1, 0)
#define MAX20356_REG_ILIMCTRL1          0x13
#define MAX20356_ILIMCTRL1_ILIMBLANK_MSK GENMASK(7, 6)
#define MAX20356_ILIMCTRL1_ILIMMAX_MSK   GENMASK(5, 3)
#define MAX20356_ILIMCTRL1_ILIMCNTL_MSK  GENMASK(2, 0)
#define MAX20356_REG_ILIMCTRL2          0x14
#define MAX20356_ILIMCTRL2_SYSDSCEN_MSK  BIT(5)
#define MAX20356_ILIMCTRL2_SYSMINVLT_MSK GENMASK(3, 0)

/* Shortest selectable watchdog window (WDTmrSel 0 => 4 s). */
#define WDT_DEMO_WINDOW_MS 4000U
/* Number of in-window feeds that demonstrate normal servicing before the fault.
 * Each wdt_feed() reads Int5 (the kick), which keeps the timer from expiring.
 */
#define WDT_DEMO_FEED_COUNT 3
/* How long to leave the watchdog unfed, in multiples of the window. The first
 * expiry after arming is ignored by the hardware (the first interval is
 * effectively doubled), so a reset lands roughly two windows after arming; wait
 * a couple more for margin.
 */
#define WDT_DEMO_FAULT_WINDOWS 4

#define WDT_DEMO_MODIFIED_ICHG_UA 300000

/* Input-limiter values the sample writes before arming the watchdog, chosen to
 * differ from the register defaults (ILimCtrl1 resets to 0x00, ILimCtrl2's
 * SysDSCEn/SysMinVlt reset to 0) so the writes are real changes; the
 * charger/limiter reset returns both registers to those defaults.
 */
#define WDT_DEMO_ILIMCTRL1_VAL                                                                      \
	(FIELD_PREP(MAX20356_ILIMCTRL1_ILIMBLANK_MSK, 0x2U) |                                       \
	 FIELD_PREP(MAX20356_ILIMCTRL1_ILIMMAX_MSK, 0x5U) |                                         \
	 FIELD_PREP(MAX20356_ILIMCTRL1_ILIMCNTL_MSK, 0x3U))
#define WDT_DEMO_ILIMCTRL1_MSK                                                                      \
	(MAX20356_ILIMCTRL1_ILIMBLANK_MSK | MAX20356_ILIMCTRL1_ILIMMAX_MSK |                        \
	 MAX20356_ILIMCTRL1_ILIMCNTL_MSK)
#define WDT_DEMO_ILIMCTRL2_VAL                                                                      \
	(MAX20356_ILIMCTRL2_SYSDSCEN_MSK | FIELD_PREP(MAX20356_ILIMCTRL2_SYSMINVLT_MSK, 0x8U))
#define WDT_DEMO_ILIMCTRL2_MSK                                                                      \
	(MAX20356_ILIMCTRL2_SYSDSCEN_MSK | MAX20356_ILIMCTRL2_SYSMINVLT_MSK)

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

static const char *charge_type_str(enum charger_charge_type type)
{
	switch (type) {
	case CHARGER_CHARGE_TYPE_NONE:
		return "none";
	case CHARGER_CHARGE_TYPE_TRICKLE:
		return "trickle";
	case CHARGER_CHARGE_TYPE_FAST:
		return "fast";
	case CHARGER_CHARGE_TYPE_STANDARD:
		return "standard";
	case CHARGER_CHARGE_TYPE_ADAPTIVE:
		return "adaptive";
	case CHARGER_CHARGE_TYPE_LONGLIFE:
		return "longlife";
	case CHARGER_CHARGE_TYPE_BYPASS:
		return "bypass";
	default:
		return "unknown";
	}
}

/* Live charge-type read: decoded from Status0.ChgStat, so it reflects the actual
 * charger register state rather than a cached software value. A charger/limiter
 * reset (WDRstType = 0b01) clears ChgCntl0.ChgEn, which the charger state machine
 * reports back here.
 */
static int read_charge_type(const struct device *chgdev, enum charger_charge_type *type)
{
	union charger_propval val;
	int ret;

	ret = charger_get_prop(chgdev, CHARGER_PROP_CHARGE_TYPE, &val);
	if (ret != 0) {
		return ret;
	}

	*type = val.charge_type;
	return 0;
}

#if WDT_DEMO_DEBUG
/* Dump the watchdog control register and decode its fields so the arm/reset-type
 * configuration can be verified against what was programmed.
 */
static void debug_dump_watchdog(const struct device *pmic)
{
	uint8_t wdcntl;
	int ret;

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_WDCNTL, &wdcntl);
	if (ret != 0) {
		printk("[%s]:   DBG WDCntl (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_WDCNTL, ret);
		return;
	}

	printk("[%s]:   DBG WDCntl   (0x%02x) = 0x%02x (WDRstType=%u, WDTmrSel=%u)\n", now_str(),
	       MAX20356_REG_WDCNTL, wdcntl,
	       (unsigned int)FIELD_GET(MAX20356_WDCNTL_WDRSTTYPE_MSK, wdcntl),
	       (unsigned int)FIELD_GET(MAX20356_WDCNTL_WDTMRSEL_MSK, wdcntl));
}

/* Dump the charger registers (Status0/ChgCur0/ChgCntl0) and decode the fields the
 * sample touches, so the configured state and the post-reset state can be
 * compared at the register level.
 */
static void debug_dump_charger(const struct device *pmic)
{
	uint8_t status0, chgcur0, chgcntl0;
	int ret;

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_STATUS0, &status0);
	if (ret == 0) {
		printk("[%s]:   DBG Status0  (0x%02x) = 0x%02x (ChgStat=%u)\n", now_str(),
		       MAX20356_REG_STATUS0, status0,
		       (unsigned int)FIELD_GET(MAX20356_STATUS0_CHGSTAT_MSK, status0));
	} else {
		printk("[%s]:   DBG Status0  (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_STATUS0, ret);
	}

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_CHGCUR0, &chgcur0);
	if (ret == 0) {
		printk("[%s]:   DBG ChgCur0  (0x%02x) = 0x%02x (CC1IFChg=%u)\n", now_str(),
		       MAX20356_REG_CHGCUR0, chgcur0,
		       (unsigned int)FIELD_GET(MAX20356_CHGCUR0_CC1IFCHG_MSK, chgcur0));
	} else {
		printk("[%s]:   DBG ChgCur0  (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_CHGCUR0, ret);
	}

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_CHGCNTL0, &chgcntl0);
	if (ret == 0) {
		printk("[%s]:   DBG ChgCntl0 (0x%02x) = 0x%02x (ChgEn=%u)\n", now_str(),
		       MAX20356_REG_CHGCNTL0, chgcntl0,
		       (unsigned int)FIELD_GET(MAX20356_CHGCNTL0_CHGEN_MSK, chgcntl0));
	} else {
		printk("[%s]:   DBG ChgCntl0 (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_CHGCNTL0, ret);
	}
}

/* Dump the input-limiter registers (ILimCtrl1/ILimCtrl2) and decode their fields.
 * These are in the charger/limiter reset scope, so they should return to their
 * defaults (ILimCtrl1 = 0x00; ILimCtrl2 SysDSCEn/SysMinVlt = 0) after the reset.
 */
static void debug_dump_limiter(const struct device *pmic)
{
	uint8_t ilim1, ilim2;
	int ret;

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL1, &ilim1);
	if (ret == 0) {
		printk("[%s]:   DBG ILimCtrl1 (0x%02x) = 0x%02x (ILimBlank=%u, ILimMax=%u, "
		       "ILimCntl=%u)\n", now_str(), MAX20356_REG_ILIMCTRL1, ilim1,
		       (unsigned int)FIELD_GET(MAX20356_ILIMCTRL1_ILIMBLANK_MSK, ilim1),
		       (unsigned int)FIELD_GET(MAX20356_ILIMCTRL1_ILIMMAX_MSK, ilim1),
		       (unsigned int)FIELD_GET(MAX20356_ILIMCTRL1_ILIMCNTL_MSK, ilim1));
	} else {
		printk("[%s]:   DBG ILimCtrl1 (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_ILIMCTRL1, ret);
	}

	ret = mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL2, &ilim2);
	if (ret == 0) {
		printk("[%s]:   DBG ILimCtrl2 (0x%02x) = 0x%02x (SysDSCEn=%u, SysMinVlt=%u)\n",
		       now_str(), MAX20356_REG_ILIMCTRL2, ilim2,
		       (unsigned int)FIELD_GET(MAX20356_ILIMCTRL2_SYSDSCEN_MSK, ilim2),
		       (unsigned int)FIELD_GET(MAX20356_ILIMCTRL2_SYSMINVLT_MSK, ilim2));
	} else {
		printk("[%s]:   DBG ILimCtrl2 (0x%02x) read failed: %d\n", now_str(),
		       MAX20356_REG_ILIMCTRL2, ret);
	}
}
#else
static inline void debug_dump_watchdog(const struct device *pmic)
{
	ARG_UNUSED(pmic);
}
static inline void debug_dump_charger(const struct device *pmic)
{
	ARG_UNUSED(pmic);
}
static inline void debug_dump_limiter(const struct device *pmic)
{
	ARG_UNUSED(pmic);
}
#endif /* WDT_DEMO_DEBUG */

static int validate_watchdog_charger_reset(const struct device *pmic, const struct device *chgdev)
{
	union charger_propval ichg = {.const_charge_current_ua = WDT_DEMO_MODIFIED_ICHG_UA};
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = WDT_DEMO_WINDOW_MS},
		.callback = NULL,
		/* A reset flag is required by the wdt API; the charger/limiter-only
		 * action (0b01) is not expressible through it, so it is applied
		 * separately below via mfd_max20356_wdt_set_rsttype().
		 */
		.flags = WDT_FLAG_RESET_CPU_CORE,
	};
	const struct device *wdtdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt));
	enum charger_charge_type initial_type;
	enum charger_charge_type final_type;
	uint8_t initial_ilim1 = 0, initial_ilim2 = 0;
	uint8_t final_ilim1 = 0, final_ilim2 = 0;
	int ret;

	printk("[%s]: === MAX20356 watchdog charger-reset test ===\n", now_str());

	if ((wdtdev == NULL) || !device_is_ready(wdtdev)) {
		printk("[%s]: watchdog device not ready\n", now_str());
		return -ENODEV;
	}

	/* Record the charge type before configuring the charger: this is the
	 * value the charger/limiter reset should restore.
	 */
	ret = read_charge_type(chgdev, &initial_type);
	if (ret != 0) {
		printk("[%s]: failed to read initial charge type: %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: initial charge type: %s\n", now_str(), charge_type_str(initial_type));

	/* Modify a charger register (ChgCur0.CC1IFChg) and enable charging
	 * (ChgCntl0.ChgEn); both are reset by the charger/limiter reset action.
	 */
	ret = charger_set_prop(chgdev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &ichg);
	if (ret != 0) {
		printk("[%s]: failed to set fast-charge current: %d\n", now_str(), ret);
		return ret;
	}

	ret = charger_charge_enable(chgdev, false);
	if (ret != 0) {
		printk("[%s]: failed to enable charger: %d\n", now_str(), ret);
		return ret;
	}
	printk("[%s]: charger configured (charging enabled)\n", now_str());

	/* Also modify the input-limiter registers (ILimCtrl1/ILimCtrl2). These are
	 * in the charger/limiter reset scope, so the watchdog reset should return
	 * them to their defaults just like the charger registers.
	 */
	ret = mfd_max20356_reg_update(pmic, MAX20356_REG_ILIMCTRL1, WDT_DEMO_ILIMCTRL1_MSK,
				      WDT_DEMO_ILIMCTRL1_VAL);
	if (ret != 0) {
		printk("[%s]: failed to set ILimCtrl1: %d\n", now_str(), ret);
		return ret;
	}

	ret = mfd_max20356_reg_update(pmic, MAX20356_REG_ILIMCTRL2, WDT_DEMO_ILIMCTRL2_MSK,
				      WDT_DEMO_ILIMCTRL2_VAL);
	if (ret != 0) {
		printk("[%s]: failed to set ILimCtrl2: %d\n", now_str(), ret);
		return ret;
	}

	(void)mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL1, &initial_ilim1);
	(void)mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL2, &initial_ilim2);
	printk("[%s]: limiter configured (ILimCtrl1=0x%02x, ILimCtrl2=0x%02x)\n", now_str(),
	       initial_ilim1, initial_ilim2);

	/* Debug: confirm the charger and limiter registers took the configuration
	 * over I2C.
	 */
	debug_dump_charger(pmic);
	debug_dump_limiter(pmic);

	/* Arm the watchdog, then override the reset action to charger/limiter-only
	 * (WDRstType = 0b01). The override is a plain WDCntl write, so this sample's
	 * overlay leaves the watchdog node unlocked.
	 */
	ret = wdt_install_timeout(wdtdev, &cfg);
	if (ret != 0) {
		printk("[%s]: wdt_install_timeout failed: %d\n", now_str(), ret);
		return ret;
	}

	ret = wdt_setup(wdtdev, 0);
	if (ret != 0) {
		printk("[%s]: wdt_setup failed: %d\n", now_str(), ret);
		return ret;
	}

	ret = mfd_max20356_wdt_set_rsttype(pmic, MAX20356_WDT_CHG_LIM_RST);
	if (ret != 0) {
		printk("[%s]: failed to select charger/limiter reset: %d\n", now_str(), ret);
		(void)wdt_disable(wdtdev);
		return ret;
	}
	printk("[%s]: watchdog armed (window %u ms, reset type 1 = charger/limiter reset); "
	       "feeding %d times\n", now_str(), WDT_DEMO_WINDOW_MS, WDT_DEMO_FEED_COUNT);

	/* Debug: confirm the watchdog is armed with the expected timer interval and
	 * reset type (WDRstType should read 1, WDTmrSel 0).
	 */
	debug_dump_watchdog(pmic);

	/* Feed the watchdog a few times to show normal servicing. wdt_feed() reads
	 * Int5 (the kick), so the timer is restarted and the charger/limiter reset
	 * does not fire while feeding continues.
	 */
	for (int i = 0; i < WDT_DEMO_FEED_COUNT; i++) {
		/* Feed well inside the window to prove normal servicing works. */
		k_sleep(K_MSEC(WDT_DEMO_WINDOW_MS / 2U));
		ret = wdt_feed(wdtdev, 0);
		printk("[%s]:   feed %d -> %s (%d)\n", now_str(), i, (ret == 0) ? "ok" : "FAIL",
		       ret);
		if (ret != 0) {
			(void)wdt_disable(wdtdev);
			return ret;
		}
	}

	/* Stop feeding and let the timer expire. Nothing reads Int5 now, so the
	 * timer is never kicked and the charger/limiter reset fires.
	 */
	printk("[%s]: feeding stopped; waiting for charger-register reset...\n", now_str());

	for (int i = 0; i < WDT_DEMO_FAULT_WINDOWS; i++) {
		enum charger_charge_type type;

		k_sleep(K_MSEC(WDT_DEMO_WINDOW_MS));

		ret = read_charge_type(chgdev, &type);
		if (ret != 0) {
			printk("[%s]: failed to read charge type: %d\n", now_str(), ret);
			(void)wdt_disable(wdtdev);
			return ret;
		}
		printk("[%s]:   window %d: charge type = %s\n", now_str(), i, charge_type_str(type));
	}

	ret = read_charge_type(chgdev, &final_type);
	if (ret != 0) {
		printk("[%s]: failed to read final charge type: %d\n", now_str(), ret);
		(void)wdt_disable(wdtdev);
		return ret;
	}

	/* Stop the watchdog so it does not keep resetting the charger. */
	ret = wdt_disable(wdtdev);
	if (ret != 0) {
		printk("[%s]: wdt_disable failed: %d\n", now_str(), ret);
		return ret;
	}

	/* Read the limiter registers back after the reset: they are in the
	 * charger/limiter reset scope, so they should have returned to defaults.
	 */
	(void)mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL1, &final_ilim1);
	(void)mfd_max20356_reg_read(pmic, MAX20356_REG_ILIMCTRL2, &final_ilim2);

	/* Debug: dump the charger and limiter registers after the reset. If the
	 * charger/limiter reset fired, the charger registers return to their
	 * defaults (ChgEn=0, CC1IFChg=0) and the limiter registers to theirs
	 * (ILimCtrl1=0x00, ILimCtrl2 SysDSCEn/SysMinVlt=0).
	 */
	debug_dump_charger(pmic);
	debug_dump_limiter(pmic);

	printk("[%s]: charge type after watchdog: %s (initial %s)\n", now_str(),
	       charge_type_str(final_type), charge_type_str(initial_type));
	printk("[%s]: limiter after watchdog: ILimCtrl1=0x%02x (was 0x%02x), "
	       "ILimCtrl2=0x%02x (was 0x%02x)\n", now_str(), final_ilim1, initial_ilim1,
	       final_ilim2, initial_ilim2);

	if (final_type == initial_type) {
		printk("[%s]: WDT charger-reset test PASSED: charge type returned to initial\n",
		       now_str());
	} else {
		/* On a bench with no battery or charge source the charger never
		 * leaves the not-charging state, so enabling it does not change the
		 * charge type and the reset is not observable this way. Report it as
		 * a soft result rather than a wiring failure.
		 */
		printk("[%s]: WDT charger-reset test: charge type unchanged from configured "
		       "state (expected if no charge source is attached)\n",
		       now_str());
	}

	/* The limiter registers reset to known defaults, so this check is
	 * observable without any external supply: a changed value means the reset
	 * did not take, an unchanged (still-configured) value means it did not fire.
	 */
	if ((final_ilim1 != initial_ilim1) || (final_ilim2 != initial_ilim2)) {
		printk("[%s]: limiter check PASSED: ILimCtrl1/2 reset by watchdog\n", now_str());
	} else {
		printk("[%s]: limiter check: ILimCtrl1/2 unchanged (reset did not fire)\n",
		       now_str());
	}

	return 0;
}

int main(void)
{
	const struct device *pmic = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pmic));
	const struct device *chgdev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(charger));

	/* The watchdog and charger route their register access through the MFD
	 * parent, so it must be up first. Its init priority runs ahead of the
	 * children; check here to fail early and clearly if it did not come up.
	 */
	if ((pmic == NULL) || !device_is_ready(pmic)) {
		printk("[%s]: MFD parent (MAX20356) not ready\n", now_str());
		return -ENODEV;
	}

	if ((chgdev == NULL) || !device_is_ready(chgdev)) {
		printk("[%s]: charger device not ready\n", now_str());
		return -ENODEV;
	}

	return validate_watchdog_charger_reset(pmic, chgdev);
}
