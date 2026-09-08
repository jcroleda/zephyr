/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356/MAX20358 battery charger child driver. Exposes the Zephyr charger
 * API on top of the MFD parent's shared I2C register access. Charger status,
 * health and online are decoded live from the Status0/Status1 registers; the
 * fast-charge current and battery regulation voltage are cached because their
 * registers are cleared on a CHGIN edge.
 */

#define DT_DRV_COMPAT adi_max20356_charger

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/sys/linear_range.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(charger_max20356, CONFIG_CHARGER_LOG_LEVEL);

/* ChgStat[3:0] (Status0) charger-state encoding. */
#define MAX20356_CHGSTAT_OFF          0x0U
#define MAX20356_CHGSTAT_IDLE         0x1U
#define MAX20356_CHGSTAT_PRECHARGE    0x2U
#define MAX20356_CHGSTAT_CC1          0x3U
#define MAX20356_CHGSTAT_CC2          0x4U
#define MAX20356_CHGSTAT_CV           0x5U
#define MAX20356_CHGSTAT_MAINTAIN     0x6U
#define MAX20356_CHGSTAT_MAINTAIN_TMO 0x7U
#define MAX20356_CHGSTAT_FAULT_PCHG   0x8U
#define MAX20356_CHGSTAT_FAULT_SAFETY 0x9U
#define MAX20356_CHGSTAT_TEMP_SUSPEND 0xFU

/* ThmStat[2:0] (Status0) thermistor-zone encoding. */
#define MAX20356_THMSTAT_COLD    0x0U
#define MAX20356_THMSTAT_COOL    0x1U
#define MAX20356_THMSTAT_ROOM    0x2U
#define MAX20356_THMSTAT_WARM    0x3U
#define MAX20356_THMSTAT_HOT     0x4U
#define MAX20356_THMSTAT_NO_THM  0x5U

/* CC1IFChg/CC2IFChg fast-charge current, 4mA..500mA (ChgCur0/ChgCur1). */
#define MAX20356_ICHG_MIN_UA 4000U
#define MAX20356_ICHG_MAX_UA 500000U
static const struct linear_range ichg_ua_range[] = {
	LINEAR_RANGE_INIT(4000, 2000, 0x00U, 0x3FU),
	LINEAR_RANGE_INIT(140000, 10000, 0x40U, 0x64U),
};

/* ChgBatReg battery regulation voltage, 4.15V..4.70V (ChgCntl1). */
#define MAX20356_VBATREG_MIN_UV 4150000U
#define MAX20356_VBATREG_MAX_UV 4700000U
static const struct linear_range vbatreg_uv_range[] = {
	LINEAR_RANGE_INIT(4150000, 10000, 0x00U, 0x37U),
};

struct charger_max20356_config {
	const struct device *mfd_dev;
	uint32_t init_ichg_ua;
	uint32_t init_vbatreg_uv;
};

struct charger_max20356_data {
	const struct device *dev;
	uint32_t ichg_ua;
	uint32_t vbatreg_uv;
	bool charger_enabled;
	charger_status_notifier_t status_notifier;
	charger_online_notifier_t online_notifier;
#ifdef CONFIG_MFD_MAX20356_TRIGGER
	/* Which INTB event groups the driver currently holds a callback for.
	 * Registration is lazy: a group is claimed only while a consumer has the
	 * matching notifier installed, so a charger with no notifier in use leaves
	 * INTB free for another consumer (for example the watchdog, which requires
	 * exclusive INTB ownership).
	 */
	bool cb_chg_active;
	bool cb_thm_active;
	bool cb_usb_active;
#endif /* CONFIG_MFD_MAX20356_TRIGGER */
};

static int charger_max20356_get_status(const struct device *dev, enum charger_status *status)
{
	const struct charger_max20356_config *cfg = dev->config;
	uint8_t val;
	int ret;

	ret = mfd_max20356_reg_read(cfg->mfd_dev, MAX20356_REG_STATUS0, &val);
	if (ret != 0) {
		return ret;
	}

	switch (FIELD_GET(MAX20356_STATUS0_CHGSTAT_MSK, val)) {
	case MAX20356_CHGSTAT_OFF:
	case MAX20356_CHGSTAT_IDLE:
	case MAX20356_CHGSTAT_FAULT_PCHG:
	case MAX20356_CHGSTAT_FAULT_SAFETY:
	case MAX20356_CHGSTAT_TEMP_SUSPEND:
		*status = CHARGER_STATUS_NOT_CHARGING;
		break;
	case MAX20356_CHGSTAT_PRECHARGE:
	case MAX20356_CHGSTAT_CC1:
	case MAX20356_CHGSTAT_CC2:
	case MAX20356_CHGSTAT_CV:
	case MAX20356_CHGSTAT_MAINTAIN:
		*status = CHARGER_STATUS_CHARGING;
		break;
	case MAX20356_CHGSTAT_MAINTAIN_TMO:
		*status = CHARGER_STATUS_FULL;
		break;
	default:
		*status = CHARGER_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static int charger_max20356_get_charge_type(const struct device *dev,
					    enum charger_charge_type *charge_type)
{
	const struct charger_max20356_config *cfg = dev->config;
	uint8_t val;
	int ret;

	ret = mfd_max20356_reg_read(cfg->mfd_dev, MAX20356_REG_STATUS0, &val);
	if (ret != 0) {
		return ret;
	}

	switch (FIELD_GET(MAX20356_STATUS0_CHGSTAT_MSK, val)) {
	case MAX20356_CHGSTAT_OFF:
	case MAX20356_CHGSTAT_IDLE:
	case MAX20356_CHGSTAT_MAINTAIN_TMO:
	case MAX20356_CHGSTAT_FAULT_PCHG:
	case MAX20356_CHGSTAT_FAULT_SAFETY:
	case MAX20356_CHGSTAT_TEMP_SUSPEND:
		*charge_type = CHARGER_CHARGE_TYPE_NONE;
		break;
	case MAX20356_CHGSTAT_PRECHARGE:
	case MAX20356_CHGSTAT_MAINTAIN:
		*charge_type = CHARGER_CHARGE_TYPE_TRICKLE;
		break;
	case MAX20356_CHGSTAT_CC1:
	case MAX20356_CHGSTAT_CC2:
	case MAX20356_CHGSTAT_CV:
		*charge_type = CHARGER_CHARGE_TYPE_FAST;
		break;
	default:
		*charge_type = CHARGER_CHARGE_TYPE_UNKNOWN;
		break;
	}

	return 0;
}

static int charger_max20356_get_online(const struct device *dev, enum charger_online *online)
{
	const struct charger_max20356_config *cfg = dev->config;
	uint8_t val;
	int ret;

	ret = mfd_max20356_reg_read(cfg->mfd_dev, MAX20356_REG_STATUS1, &val);
	if (ret != 0) {
		return ret;
	}

	if (((val & MAX20356_STATUS1_USBOK_MSK) != 0U) &&
	    ((val & MAX20356_STATUS1_USBOVP_MSK) == 0U)) {
		*online = CHARGER_ONLINE_FIXED;
	} else {
		*online = CHARGER_ONLINE_OFFLINE;
	}

	return 0;
}

static int charger_max20356_get_health(const struct device *dev, enum charger_health *health)
{
	const struct charger_max20356_config *cfg = dev->config;
	uint8_t status0, status1;
	int ret;

	ret = mfd_max20356_reg_read(cfg->mfd_dev, MAX20356_REG_STATUS0, &status0);
	if (ret != 0) {
		return ret;
	}

	ret = mfd_max20356_reg_read(cfg->mfd_dev, MAX20356_REG_STATUS1, &status1);
	if (ret != 0) {
		return ret;
	}

	/* Overvoltage / thermal-shutdown faults take precedence over the zone. */
	if ((status1 & MAX20356_STATUS1_USBOVP_MSK) != 0U) {
		*health = CHARGER_HEALTH_OVERVOLTAGE;
		return 0;
	}

	if (((status1 & MAX20356_STATUS1_THMSD_MSK) != 0U) ||
	    ((status1 & MAX20356_STATUS1_CHGJEITASD_MSK) != 0U)) {
		*health = CHARGER_HEALTH_OVERHEAT;
		return 0;
	}

	switch (FIELD_GET(MAX20356_STATUS0_THMSTAT_MSK, status0)) {
	case MAX20356_THMSTAT_COLD:
		*health = CHARGER_HEALTH_COLD;
		break;
	case MAX20356_THMSTAT_COOL:
		*health = CHARGER_HEALTH_COOL;
		break;
	case MAX20356_THMSTAT_WARM:
		*health = CHARGER_HEALTH_WARM;
		break;
	case MAX20356_THMSTAT_HOT:
		*health = CHARGER_HEALTH_HOT;
		break;
	default:
		*health = CHARGER_HEALTH_GOOD;
		break;
	}

	return 0;
}

static int charger_max20356_set_constant_charge_current(const struct device *dev,
							uint32_t current_ua)
{
	const struct charger_max20356_config *cfg = dev->config;
	struct charger_max20356_data *data = dev->data;
	uint16_t idx;
	int ret;

	if ((current_ua < MAX20356_ICHG_MIN_UA) || (current_ua > MAX20356_ICHG_MAX_UA)) {
		return -EINVAL;
	}

	ret = linear_range_group_get_index(ichg_ua_range, ARRAY_SIZE(ichg_ua_range), current_ua,
					   &idx);
	if (ret != 0) {
		return -EINVAL;
	}

	ret = mfd_max20356_reg_update(cfg->mfd_dev, MAX20356_REG_CHGCUR0,
				      MAX20356_CHGCUR0_CC1IFCHG_MSK,
				      FIELD_PREP(MAX20356_CHGCUR0_CC1IFCHG_MSK, idx));
	if (ret != 0) {
		return ret;
	}

	data->ichg_ua = current_ua;
	return 0;
}

static int charger_max20356_set_constant_charge_voltage(const struct device *dev,
							uint32_t voltage_uv)
{
	const struct charger_max20356_config *cfg = dev->config;
	struct charger_max20356_data *data = dev->data;
	uint16_t idx;
	int ret;

	if ((voltage_uv < MAX20356_VBATREG_MIN_UV) || (voltage_uv > MAX20356_VBATREG_MAX_UV)) {
		return -EINVAL;
	}

	ret = linear_range_group_get_index(vbatreg_uv_range, ARRAY_SIZE(vbatreg_uv_range), voltage_uv,
					   &idx);
	if (ret != 0) {
		return -EINVAL;
	}

	ret = mfd_max20356_reg_update(cfg->mfd_dev, MAX20356_REG_CHGCNTL1,
				      MAX20356_CHGCNTL1_CHGBATREG_MSK,
				      FIELD_PREP(MAX20356_CHGCNTL1_CHGBATREG_MSK, idx));
	if (ret != 0) {
		return ret;
	}

	data->vbatreg_uv = voltage_uv;
	return 0;
}

static int charger_max20356_charge_enable(const struct device *dev, const bool enable)
{
	const struct charger_max20356_config *cfg = dev->config;
	struct charger_max20356_data *data = dev->data;
	int ret;

	ret = mfd_max20356_reg_update(cfg->mfd_dev, MAX20356_REG_CHGCNTL0,
				      MAX20356_CHGCNTL0_CHGEN_MSK,
				      enable ? MAX20356_CHGCNTL0_CHGEN_MSK : 0U);
	if (ret != 0) {
		return ret;
	}

	data->charger_enabled = enable;
	return 0;
}

static int charger_max20356_get_prop(const struct device *dev, charger_prop_t prop,
				     union charger_propval *val)
{
	struct charger_max20356_data *data = dev->data;

	switch (prop) {
	case CHARGER_PROP_ONLINE:
		return charger_max20356_get_online(dev, &val->online);
	case CHARGER_PROP_STATUS:
		return charger_max20356_get_status(dev, &val->status);
	case CHARGER_PROP_CHARGE_TYPE:
		return charger_max20356_get_charge_type(dev, &val->charge_type);
	case CHARGER_PROP_HEALTH:
		return charger_max20356_get_health(dev, &val->health);
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		val->const_charge_current_ua = data->ichg_ua;
		return 0;
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		val->const_charge_voltage_uv = data->vbatreg_uv;
		return 0;
	default:
		return -ENOTSUP;
	}
}

#ifdef CONFIG_MFD_MAX20356_TRIGGER
/*
 * INTB event handler: charger/USB/thermal sources feed the status and online
 * notifiers so consumers see interrupt-driven state changes (REQ-IRQ-006).
 */
static void charger_max20356_evt(const struct device *mfd_dev, enum max20356_event evt, void *user)
{
	const struct device *dev = user;
	struct charger_max20356_data *data = dev->data;
	int ret;

	ARG_UNUSED(mfd_dev);

	if ((evt == MAX20356_EVT_CHARGER) || (evt == MAX20356_EVT_THERMAL)) {
		enum charger_status status;

		ret = charger_max20356_get_status(dev, &status);
		if ((ret == 0) && (data->status_notifier != NULL)) {
			data->status_notifier(status);
		}
	}

	if (evt == MAX20356_EVT_USB) {
		enum charger_online online;

		ret = charger_max20356_get_online(dev, &online);
		if ((ret == 0) && (data->online_notifier != NULL)) {
			data->online_notifier(online);
		}
	}
}

/* Add or drop the INTB callback for one event group so that *active tracks
 * @p want. Adding can fail (for example -EBUSY while the watchdog owns INTB);
 * dropping cannot, so *active only advances to the reached state.
 */
static int charger_max20356_set_group(const struct device *dev, enum max20356_event evt,
				      bool *active, bool want)
{
	const struct charger_max20356_config *cfg = dev->config;
	int ret;

	if (want == *active) {
		return 0;
	}

	if (want) {
		ret = mfd_max20356_add_callback(cfg->mfd_dev, evt, charger_max20356_evt,
						(void *)dev);
		if (ret != 0) {
			return ret;
		}
	} else {
		(void)mfd_max20356_remove_callback(cfg->mfd_dev, evt, charger_max20356_evt);
	}

	*active = want;

	return 0;
}

/* Reconcile INTB callback registration with the installed notifiers: the status
 * notifier needs the CHARGER and THERMAL groups, the online notifier needs USB.
 * No group is held while its notifier is NULL, so INTB stays free for exclusive
 * consumers when notifications are unused.
 */
static int charger_max20356_sync_callbacks(const struct device *dev)
{
	struct charger_max20356_data *data = dev->data;
	bool want_status = data->status_notifier != NULL;
	int ret;

	ret = charger_max20356_set_group(dev, MAX20356_EVT_CHARGER, &data->cb_chg_active,
					 want_status);
	if (ret != 0) {
		return ret;
	}

	ret = charger_max20356_set_group(dev, MAX20356_EVT_THERMAL, &data->cb_thm_active,
					 want_status);
	if (ret != 0) {
		return ret;
	}

	return charger_max20356_set_group(dev, MAX20356_EVT_USB, &data->cb_usb_active,
					 data->online_notifier != NULL);
}
#else
static int charger_max20356_sync_callbacks(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}
#endif /* CONFIG_MFD_MAX20356_TRIGGER */

static int charger_max20356_set_prop(const struct device *dev, charger_prop_t prop,
				     const union charger_propval *val)
{
	struct charger_max20356_data *data = dev->data;
	int ret;

	switch (prop) {
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		return charger_max20356_set_constant_charge_current(dev,
								    val->const_charge_current_ua);
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		return charger_max20356_set_constant_charge_voltage(dev,
								    val->const_charge_voltage_uv);
	case CHARGER_PROP_STATUS_NOTIFICATION: {
		charger_status_notifier_t prev = data->status_notifier;

		data->status_notifier = val->status_notification;
		ret = charger_max20356_sync_callbacks(dev);
		if (ret != 0) {
			data->status_notifier = prev;
			(void)charger_max20356_sync_callbacks(dev);
		}
		return ret;
	}
	case CHARGER_PROP_ONLINE_NOTIFICATION: {
		charger_online_notifier_t prev = data->online_notifier;

		data->online_notifier = val->online_notification;
		ret = charger_max20356_sync_callbacks(dev);
		if (ret != 0) {
			data->online_notifier = prev;
			(void)charger_max20356_sync_callbacks(dev);
		}
		return ret;
	}
	default:
		return -ENOTSUP;
	}
}

static int charger_max20356_init(const struct device *dev)
{
	const struct charger_max20356_config *cfg = dev->config;
	struct charger_max20356_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->mfd_dev)) {
		LOG_ERR("MFD parent device not ready");
		return -ENODEV;
	}

	data->dev = dev;

	if (cfg->init_ichg_ua != 0U) {
		ret = charger_max20356_set_constant_charge_current(dev, cfg->init_ichg_ua);
		if (ret != 0) {
			LOG_ERR("Failed to set fast-charge current: %d", ret);
			return ret;
		}
	}

	if (cfg->init_vbatreg_uv != 0U) {
		ret = charger_max20356_set_constant_charge_voltage(dev, cfg->init_vbatreg_uv);
		if (ret != 0) {
			LOG_ERR("Failed to set battery regulation voltage: %d", ret);
			return ret;
		}
	}

	return 0;
}

static DEVICE_API(charger, charger_max20356_driver_api) = {
	.get_property = charger_max20356_get_prop,
	.set_property = charger_max20356_set_prop,
	.charge_enable = charger_max20356_charge_enable,
};

#define CHARGER_MAX20356_DEFINE(inst)                                                              \
	static struct charger_max20356_data charger_max20356_data_##inst = {                       \
		.ichg_ua = DT_INST_PROP_OR(inst, constant_charge_current_microamp, 0),             \
		.vbatreg_uv = DT_INST_PROP_OR(inst, constant_charge_voltage_microvolt, 0),         \
	};                                                                                         \
                                                                                                   \
	static const struct charger_max20356_config charger_max20356_config_##inst = {             \
		.mfd_dev = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                    \
		.init_ichg_ua = DT_INST_PROP_OR(inst, constant_charge_current_microamp, 0),        \
		.init_vbatreg_uv = DT_INST_PROP_OR(inst, constant_charge_voltage_microvolt, 0),    \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &charger_max20356_init, NULL, &charger_max20356_data_##inst,   \
			      &charger_max20356_config_##inst, POST_KERNEL,                        \
			      CONFIG_CHARGER_MAX20356_INIT_PRIORITY,                               \
			      &charger_max20356_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CHARGER_MAX20356_DEFINE)
