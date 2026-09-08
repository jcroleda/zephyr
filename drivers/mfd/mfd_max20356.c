/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/mfd/max20356.h>

#include "mfd_max20356.h"

#define MAX20356_PWRCMD_OFF        0xB2U
#define MAX20356_PWRCMD_HARD_RESET 0xC3U
#define MAX20356_PWRCMD_SOFT_RESET 0xD4U
#define MAX20356_PWRCMD_SEAL       0xE5U

#define MAX20356_MONCFG_MONCTR_MAX      0x0FU
#define MAX20356_MONCFG_MONRATIOCFG_MAX 0x03U

/* LockUnlock passwords (LockUnlock1-3). */
#define MAX20356_LOCK_PASSWD_UNLOCK 0x55U
#define MAX20356_LOCK_PASSWD_LOCK   0xAAU

/* Maps a lock domain to its LockMsk register, bit mask, and LockUnlock register. */
struct max20356_lock_desc {
	uint8_t lockmsk_reg;
	uint8_t lockmsk_bit;
	uint8_t unlock_reg;
};

static const struct max20356_lock_desc max20356_lock_table[MAX20356_LOCK_MAX] = {
	[MAX20356_LOCK_BUCK1] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_BK1LCK_MSK,
				 MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_BUCK2] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_BK2LCK_MSK,
				 MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_BUCK3] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_BK3LCK_MSK,
				 MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_BBST] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_BBLCK_MSK,
				MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_LDO1] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_LD1LCK_MSK,
				MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_LDO2] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_LD2LCK_MSK,
				MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_LDO3] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_LD3LCK_MSK,
				MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_LDO4] = {MAX20356_REG_LOCKMSK1, MAX20356_LOCKMSK1_LD4LCK_MSK,
				MAX20356_REG_LOCKUNLOCK1},
	[MAX20356_LOCK_CHG] = {MAX20356_REG_LOCKMSK3, MAX20356_LOCKMSK3_CHGLCK_MSK,
			       MAX20356_REG_LOCKUNLOCK3},
	[MAX20356_LOCK_LIM] = {MAX20356_REG_LOCKMSK3, MAX20356_LOCKMSK3_LIMLCK_MSK,
			       MAX20356_REG_LOCKUNLOCK3},
	[MAX20356_LOCK_WD] = {MAX20356_REG_LOCKMSK3, MAX20356_LOCKMSK3_WDLCK_MSK,
			      MAX20356_REG_LOCKUNLOCK3},
};

static inline const struct i2c_dt_spec *mfd_max20356_get_i2c(const struct device *dev)
{
	const struct mfd_max20356_config *config = dev->config;

	return &config->i2c;
}

int mfd_max20356_reg_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte_dt(mfd_max20356_get_i2c(dev), reg, val);
}

int mfd_max20356_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte_dt(mfd_max20356_get_i2c(dev), reg, val);
}

int mfd_max20356_reg_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	return i2c_reg_update_byte_dt(mfd_max20356_get_i2c(dev), reg, mask, val);
}

int mfd_max20356_reg_update_locked(const struct device *dev, enum max20356_lock_domain domain,
				   uint8_t reg, uint8_t mask, uint8_t val)
{
	struct mfd_max20356_data *data = dev->data;
	const struct max20356_lock_desc *desc;
	int ret;

	if (domain >= MAX20356_LOCK_MAX) {
		return -EINVAL;
	}

	desc = &max20356_lock_table[domain];

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Unmask only this domain (its LockMsk bit = 0, all others = 1) so the
	 * password affects a single function, then unlock, write and re-lock.
	 */
	ret = mfd_max20356_reg_write(dev, desc->lockmsk_reg, (uint8_t)~desc->lockmsk_bit);
	if (ret != 0) {
		goto unlock;
	}

	ret = mfd_max20356_reg_write(dev, desc->unlock_reg, MAX20356_LOCK_PASSWD_UNLOCK);
	if (ret != 0) {
		goto unlock;
	}

	ret = mfd_max20356_reg_update(dev, reg, mask, val);
	if (ret != 0) {
		goto unlock;
	}

	ret = mfd_max20356_reg_write(dev, desc->unlock_reg, MAX20356_LOCK_PASSWD_LOCK);

unlock:
	k_mutex_unlock(&data->lock);
	return ret;
}

enum max20356_variant mfd_max20356_get_variant(const struct device *dev)
{
	const struct mfd_max20356_config *config = dev->config;

	return config->variant;
}

int mfd_max20356_power_command(const struct device *dev, enum max20356_power_cmd cmd)
{
	uint8_t val;

	switch (cmd) {
	case MAX20356_PWR_OFF:
		val = MAX20356_PWRCMD_OFF;
		break;
	case MAX20356_PWR_HARD_RESET:
		val = MAX20356_PWRCMD_HARD_RESET;
		break;
	case MAX20356_PWR_SOFT_RESET:
		val = MAX20356_PWRCMD_SOFT_RESET;
		break;
	case MAX20356_PWR_SEAL:
		val = MAX20356_PWRCMD_SEAL;
		break;
	default:
		return -EINVAL;
	}

	return mfd_max20356_reg_write(dev, MAX20356_REG_PWRCMD, val);
}

int mfd_max20356_mon_select(const struct device *dev, uint8_t channel, uint8_t ratio)
{
	uint8_t mask = MAX20356_MONCFG_MONCTR_MSK | MAX20356_MONCFG_MONRATIOCFG_MSK;
	uint8_t val;

	if (channel > MAX20356_MONCFG_MONCTR_MAX || ratio > MAX20356_MONCFG_MONRATIOCFG_MAX) {
		return -EINVAL;
	}

	val = FIELD_PREP(MAX20356_MONCFG_MONCTR_MSK, channel) |
	      FIELD_PREP(MAX20356_MONCFG_MONRATIOCFG_MSK, ratio);

	return mfd_max20356_reg_update(dev, MAX20356_REG_MONCFG, mask, val);
}

int mfd_max20356_wdt_set_rsttype(const struct device *dev, enum max20356_wdt_rsttype rsttype)
{
	return mfd_max20356_reg_update(dev, MAX20356_REG_WDCNTL, MAX20356_WDCNTL_WDRSTTYPE_MSK,
				       FIELD_PREP(MAX20356_WDCNTL_WDRSTTYPE_MSK, rsttype));
}

static int mfd_max20356_init(const struct device *dev)
{
	const struct mfd_max20356_config *config = dev->config;
	struct mfd_max20356_data *data = dev->data;
	uint8_t revid;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

	/* The MAX20356/20358 do not have a device ID */
	ret = mfd_max20356_reg_read(dev, MAX20356_REG_REVID, &revid);
	if (ret < 0) {
		return ret;
	}



#ifdef CONFIG_MFD_MAX20356_TRIGGER
	ret = mfd_max20356_trigger_init(dev);
	if (ret < 0) {
		return ret;
	}
#endif /* CONFIG_MFD_MAX20356_TRIGGER */

	return 0;
}

#define MFD_MAX20356_DEFINE(inst, variant_id)                                                      \
	static struct mfd_max20356_data mfd_max20356_data_##variant_id##_##inst;                   \
                                                                                                   \
	static const struct mfd_max20356_config mfd_max20356_config_##variant_id##_##inst = {      \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.variant = (variant_id),                                                           \
		IF_ENABLED(CONFIG_MFD_MAX20356_TRIGGER,                                            \
			   (.int_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, int_gpios, {0}),))          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mfd_max20356_init, NULL,                                       \
			      &mfd_max20356_data_##variant_id##_##inst,                            \
			      &mfd_max20356_config_##variant_id##_##inst, POST_KERNEL,             \
			      CONFIG_MFD_INIT_PRIORITY, NULL);

#define DT_DRV_COMPAT adi_max20356
#define MFD_MAX20356_DEFINE_356(inst) MFD_MAX20356_DEFINE(inst, MAX20356_VARIANT_MAX20356)
DT_INST_FOREACH_STATUS_OKAY(MFD_MAX20356_DEFINE_356)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT adi_max20358
#define MFD_MAX20356_DEFINE_358(inst) MFD_MAX20356_DEFINE(inst, MAX20356_VARIANT_MAX20358)
DT_INST_FOREACH_STATUS_OKAY(MFD_MAX20356_DEFINE_358)
#undef DT_DRV_COMPAT
