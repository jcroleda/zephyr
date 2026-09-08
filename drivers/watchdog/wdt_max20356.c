/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356/MAX20358 watchdog child driver. Exposes the standard Zephyr watchdog
 * API on top of the MFD parent's shared I2C register access. The timer interval
 * (WDCntl.WDTmrSel) and reset action (WDCntl.WDRstType) are programmed at setup;
 * the timer is fed by reading Int5.WDTmr through the parent.
 *
 * The feed reads the shared Int5 register (clear-on-read; it also carries
 * I2cTmoInt), so the watchdog must run in isolation: setup() claims INTB
 * exclusively through mfd_max20356_wdt_claim(), which fails if any INTB event
 * consumer (charger, regulator, ...) is registered, and no consumer may register
 * while the watchdog is armed. disable() releases the claim.
 */

#define DT_DRV_COMPAT adi_max20356_watchdog

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wdt_max20356, CONFIG_WDT_LOG_LEVEL);

/* WDTmrSel intervals in milliseconds, indexed by field value (0..3). Per the
 * datasheet the first expiration after arming is ignored, so the effective first
 * interval is twice the selected value; the driver keeps no deadline state, so a
 * consumer that tracks feed deadlines must account for this doubling itself.
 */
static const uint32_t wdt_max20356_intervals_ms[] = {4000U, 8000U, 16000U, 32000U};

struct wdt_max20356_config {
	const struct device *mfd;
};

struct wdt_max20356_data {
	uint8_t tmrsel;
	uint8_t rsttype;
	bool timeout_valid;
	bool enabled;
};

static int wdt_max20356_install_timeout(const struct device *dev,
					const struct wdt_timeout_cfg *timeout)
{
	struct wdt_max20356_data *data = dev->data;
	uint8_t tmrsel;

	if (data->timeout_valid) {
		return -ENOMEM;
	}

	/* Hardware has no windowed mode and no pre-expiry warning interrupt. */
	if (timeout->window.min != 0U) {
		return -EINVAL;
	}

	if (timeout->callback != NULL) {
		return -ENOTSUP;
	}

	/* Map window.max to the smallest interval that is >= the request. */
	for (tmrsel = 0U; tmrsel < ARRAY_SIZE(wdt_max20356_intervals_ms); tmrsel++) {
		if (timeout->window.max <= wdt_max20356_intervals_ms[tmrsel]) {
			break;
		}
	}

	if (tmrsel >= ARRAY_SIZE(wdt_max20356_intervals_ms)) {
		return -EINVAL;
	}

	switch (timeout->flags & WDT_FLAG_RESET_MASK) {
	case WDT_FLAG_RESET_SOC:
		data->rsttype = MAX20356_WDT_HARD_RESET;
		break;
	case WDT_FLAG_RESET_CPU_CORE:
		data->rsttype = MAX20356_WDT_SOFT_RESET;
		break;
	default:
		/* WDRstType 0b00 means the watchdog is off, so there is no
		 * "expire without reset" mode to map WDT_FLAG_RESET_NONE onto.
		 */
		return -ENOTSUP;
	}

	data->tmrsel = tmrsel;
	data->timeout_valid = true;

	return 0;
}

static int wdt_max20356_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_max20356_config *config = dev->config;
	struct wdt_max20356_data *data = dev->data;
	int ret;

	if (!data->timeout_valid) {
		return -EINVAL;
	}

	if (data->enabled) {
		return -EBUSY;
	}

	/* The watchdog runs only in the On power mode and cannot be paused, so the
	 * pause options cannot be honored.
	 */
	if ((options & (WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG)) != 0U) {
		return -ENOTSUP;
	}

	/* The feed reads the shared Int5 register, so the watchdog must own INTB
	 * exclusively: claim it before arming, refusing if an INTB consumer is
	 * already registered.
	 */
	ret = mfd_max20356_wdt_claim(config->mfd, true);
	if (ret != 0) {
		return ret;
	}

	/* Per datasheet, set WDRstType = 0 before changing WDTmrSel, then arm by
	 * writing the configured WDRstType. Both writes go through the lock helper
	 * because WDCntl is guarded by LockMsk3.WDLck.
	 */
	ret = mfd_max20356_reg_update_locked(config->mfd, MAX20356_LOCK_WD, MAX20356_REG_WDCNTL,
					     MAX20356_WDCNTL_WDRSTTYPE_MSK |
						     MAX20356_WDCNTL_WDTMRSEL_MSK,
					     FIELD_PREP(MAX20356_WDCNTL_WDTMRSEL_MSK, data->tmrsel));
	if (ret != 0) {
		(void)mfd_max20356_wdt_claim(config->mfd, false);
		return ret;
	}

	ret = mfd_max20356_reg_update_locked(config->mfd, MAX20356_LOCK_WD, MAX20356_REG_WDCNTL,
					     MAX20356_WDCNTL_WDRSTTYPE_MSK,
					     FIELD_PREP(MAX20356_WDCNTL_WDRSTTYPE_MSK, data->rsttype));
	if (ret != 0) {
		(void)mfd_max20356_wdt_claim(config->mfd, false);
		return ret;
	}

	data->enabled = true;

	return 0;
}

static int wdt_max20356_disable(const struct device *dev)
{
	const struct wdt_max20356_config *config = dev->config;
	struct wdt_max20356_data *data = dev->data;
	int ret;

	if (!data->enabled) {
		return -EFAULT;
	}

	ret = mfd_max20356_reg_update_locked(config->mfd, MAX20356_LOCK_WD, MAX20356_REG_WDCNTL,
					     MAX20356_WDCNTL_WDRSTTYPE_MSK,
					     FIELD_PREP(MAX20356_WDCNTL_WDRSTTYPE_MSK,
							MAX20356_WDT_OFF));
	if (ret != 0) {
		return ret;
	}

	/* Release exclusive INTB ownership so event consumers can register again. */
	(void)mfd_max20356_wdt_claim(config->mfd, false);

	/* wdt_disable() uninstalls all timeouts: a fresh install is required
	 * before the next setup.
	 */
	data->enabled = false;
	data->timeout_valid = false;

	return 0;
}

static int wdt_max20356_feed(const struct device *dev, int channel_id)
{
	const struct wdt_max20356_config *config = dev->config;

	if (channel_id != 0) {
		return -EINVAL;
	}

	return mfd_max20356_wdt_feed(config->mfd);
}

static DEVICE_API(wdt, wdt_max20356_driver_api) = {
	.setup = wdt_max20356_setup,
	.disable = wdt_max20356_disable,
	.install_timeout = wdt_max20356_install_timeout,
	.feed = wdt_max20356_feed,
};

static int wdt_max20356_init(const struct device *dev)
{
	const struct wdt_max20356_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		return -ENODEV;
	}

	return 0;
}

#define WDT_MAX20356_DEFINE(inst)                                                                  \
	static struct wdt_max20356_data wdt_max20356_data_##inst;                                  \
                                                                                                   \
	static const struct wdt_max20356_config wdt_max20356_config_##inst = {                     \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &wdt_max20356_init, NULL, &wdt_max20356_data_##inst,           \
			      &wdt_max20356_config_##inst, POST_KERNEL,                            \
			      CONFIG_WDT_MAX20356_INIT_PRIORITY, &wdt_max20356_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_MAX20356_DEFINE)
