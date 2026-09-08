/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356/MAX20358 regulator child driver. Each rail (buck1-3, buck-boost,
 * ldo1-4, lsw1-3) is a devicetree child node matched by name; the driver drives
 * the rail's enable, output-voltage, mode and active-discharge registers through
 * the MFD parent's shared I2C access. Voltages use linear-range tables where the
 * hardware scale is linear; LDO4 (RTC) has a small discrete table.
 */

#define DT_DRV_COMPAT adi_max20356_regulator

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/dt-bindings/mfd/max20356.h>
#include <zephyr/sys/linear_range.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(regulator_max20356, CONFIG_REGULATOR_LOG_LEVEL);

/* <reg>En[1:0] "Enabled" code (01). */
#define MAX20356_REG_EN_ENABLED 0x1U

/* Buck<n>DvsCfg[4:0] DVS-mode codes. */
#define MAX20356_DVS_CFG_I2C      0x00U
#define MAX20356_DVS_CFG_GPIO_MIN 0x01U
#define MAX20356_DVS_CFG_GPIO_MAX 0x1CU
#define MAX20356_DVS_CFG_SPI      0x1DU

/* adi,dvs-mode enum indices (must match the binding's enum order). */
#define MAX20356_DVS_MODE_I2C  0
#define MAX20356_DVS_MODE_GPIO 1
#define MAX20356_DVS_MODE_SPI  2

/* SPI DVS command byte: {AD[1:0], VLT[5:0]}. */
#define MAX20356_DVS_SPI_ADD_MSK GENMASK(7, 6)
#define MAX20356_DVS_SPI_VLT_MSK GENMASK(5, 0)

/* Buck output: 0.5V base, per-rail step (Buck<n>VSet[5:0], codes 0x00..0x3F). */
static const struct linear_range __maybe_unused buck_range_10mv =
	LINEAR_RANGE_INIT(500000, 10000U, 0x00U, 0x3FU);
static const struct linear_range __maybe_unused buck_range_25mv =
	LINEAR_RANGE_INIT(500000, 25000U, 0x00U, 0x3FU);
static const struct linear_range __maybe_unused buck_range_50mv =
	LINEAR_RANGE_INIT(500000, 50000U, 0x00U, 0x3FU);

/* Buck-boost: 2.5V-5.5V, 50mV steps (BBstVSet[5:0]); codes above 0x3C not used. */
static const struct linear_range __maybe_unused bbst_range =
	LINEAR_RANGE_INIT(2500000, 50000U, 0x00U, 0x3CU);

/* LDO1/LDO2: 0.9V-4.0V, 100mV steps (LDO<n>VSet[4:0]). */
static const struct linear_range __maybe_unused ldo1_2_range =
	LINEAR_RANGE_INIT(900000, 100000U, 0x00U, 0x1FU);

/* LDO3: 0.9V-4.075V, 25mV steps (LDO3VSet[6:0]). */
static const struct linear_range __maybe_unused ldo3_range =
	LINEAR_RANGE_INIT(900000, 25000U, 0x00U, 0x7FU);

/* LDO4 (RTC): LDO4VSet base (1.2V/1.8V) plus LDO4VInc (0/25/50mV). */
static const int32_t ldo4_voltages[] = {
	1200000, 1225000, 1250000, 1800000, 1825000, 1850000,
};

/* Buck ISet / buck-boost BBstIPSet1: 0-375mA, 25mA steps (4-bit field). */
static const struct linear_range __maybe_unused iset_ua_range =
	LINEAR_RANGE_INIT(0, 25000U, 0x00U, 0x0FU);

struct regulator_max20356_desc {
	uint8_t ena_reg;
	uint8_t ena_mask;
	uint8_t vset_reg;
	uint8_t vset_mask;
	uint8_t cfg_reg;
	uint8_t actdsc_mask;
	uint8_t mode_mask;
	uint8_t iset_reg;
	uint8_t iset_mask;
	/* DVS registers (bucks only; 0 elsewhere). */
	uint8_t dvscfg0_reg;
	uint8_t dvsvlt_reg[4];
	uint8_t dvsspi_reg;
	uint8_t spi_add;
	enum max20356_lock_domain lock;
	bool lockable;
	bool is_ldo4;
	bool is_buck;
};

struct regulator_max20356_config {
	struct regulator_common_config common;
	const struct device *mfd_dev;
	const struct regulator_max20356_desc *desc;
	const struct linear_range *uv_range;
	struct spi_dt_spec dvs_spi;
	uint32_t dvs_voltages[4];
	uint32_t dvs_valley_ua;
	uint8_t dvs_mode;
	uint8_t dvs_mpc_pair[2];
	uint8_t dvs_mpc_pair_len;
	bool ldo4_rtc;
};

struct regulator_max20356_data {
	struct regulator_common_data common;
};

struct regulator_max20356_common_config {
	const struct device *mfd_dev;
};

/* Route a register update through the password sequence for lockable rails
 * (bucks, buck-boost, LDOs) or a plain update for the load switches.
 */
static int regulator_max20356_reg_update(const struct device *dev, uint8_t reg, uint8_t mask,
					 uint8_t val)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->lockable) {
		return mfd_max20356_reg_update_locked(config->mfd_dev, config->desc->lock, reg, mask,
						      val);
	}

	return mfd_max20356_reg_update(config->mfd_dev, reg, mask, val);
}

static int regulator_max20356_enable(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;

	return regulator_max20356_reg_update(dev, config->desc->ena_reg, config->desc->ena_mask,
					     MAX20356_REG_EN_ENABLED);
}

static int regulator_max20356_disable(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;

	return regulator_max20356_reg_update(dev, config->desc->ena_reg, config->desc->ena_mask,
					     0U);
}

static int regulator_max20356_set_mode(const struct device *dev, regulator_mode_t mode)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->mode_mask == 0U) {
		return -ENOTSUP;
	}

	if (mode > MAX20356_MODE_LOAD_SWITCH) {
		return -ENOTSUP;
	}

	return regulator_max20356_reg_update(dev, config->desc->cfg_reg, config->desc->mode_mask,
					     (mode == MAX20356_MODE_LOAD_SWITCH)
						     ? config->desc->mode_mask
						     : 0U);
}

static int regulator_max20356_get_mode(const struct device *dev, regulator_mode_t *mode)
{
	const struct regulator_max20356_config *config = dev->config;
	uint8_t val;
	int ret;

	if (config->desc->mode_mask == 0U) {
		return -ENOTSUP;
	}

	ret = mfd_max20356_reg_read(config->mfd_dev, config->desc->cfg_reg, &val);
	if (ret != 0) {
		return ret;
	}

	*mode = ((val & config->desc->mode_mask) != 0U) ? MAX20356_MODE_LOAD_SWITCH
							: MAX20356_MODE_LDO;
	return 0;
}

static int regulator_max20356_set_active_discharge(const struct device *dev, bool active_discharge)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->actdsc_mask == 0U) {
		return -ENOTSUP;
	}

	return regulator_max20356_reg_update(dev, config->desc->cfg_reg, config->desc->actdsc_mask,
					     active_discharge ? config->desc->actdsc_mask : 0U);
}

static int regulator_max20356_get_active_discharge(const struct device *dev, bool *active_discharge)
{
	const struct regulator_max20356_config *config = dev->config;
	uint8_t val;
	int ret;

	if (config->desc->actdsc_mask == 0U) {
		return -ENOTSUP;
	}

	ret = mfd_max20356_reg_read(config->mfd_dev, config->desc->cfg_reg, &val);
	if (ret != 0) {
		return ret;
	}

	*active_discharge = (val & config->desc->actdsc_mask) != 0U;
	return 0;
}

static unsigned int regulator_max20356_count_voltages(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->is_ldo4) {
		return ARRAY_SIZE(ldo4_voltages);
	}

	if (config->uv_range == NULL) {
		return 0;
	}

	return linear_range_values_count(config->uv_range);
}

static int regulator_max20356_list_voltage(const struct device *dev, unsigned int idx,
					   int32_t *volt_uv)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->is_ldo4) {
		if (idx >= ARRAY_SIZE(ldo4_voltages)) {
			return -EINVAL;
		}

		*volt_uv = ldo4_voltages[idx];
		return 0;
	}

	if (config->uv_range == NULL) {
		return -EINVAL;
	}

	return linear_range_get_value(config->uv_range, idx, volt_uv);
}

static int regulator_max20356_ldo4_set_voltage(const struct device *dev, int32_t min_uv,
					       int32_t max_uv)
{
	uint8_t mask = MAX20356_LDO4CFG_LDO4VSET_MSK | MAX20356_LDO4CFG_LDO4VINC_MSK;

	ARRAY_FOR_EACH(ldo4_voltages, i) {
		if ((ldo4_voltages[i] >= min_uv) && (ldo4_voltages[i] <= max_uv)) {
			uint8_t vset = (i >= 3U) ? 1U : 0U;
			uint8_t vinc = i % 3U;
			uint8_t val = FIELD_PREP(MAX20356_LDO4CFG_LDO4VSET_MSK, vset) |
				      FIELD_PREP(MAX20356_LDO4CFG_LDO4VINC_MSK, vinc);

			return regulator_max20356_reg_update(dev, MAX20356_REG_LDO4CFG, mask, val);
		}
	}

	return -EINVAL;
}

static int regulator_max20356_ldo4_get_voltage(const struct device *dev, int32_t *volt_uv)
{
	const struct regulator_max20356_config *config = dev->config;
	uint8_t val, vinc;
	int ret;

	ret = mfd_max20356_reg_read(config->mfd_dev, MAX20356_REG_LDO4CFG, &val);
	if (ret != 0) {
		return ret;
	}

	vinc = FIELD_GET(MAX20356_LDO4CFG_LDO4VINC_MSK, val);
	if (vinc > 2U) {
		vinc = 2U;
	}

	*volt_uv = ((val & MAX20356_LDO4CFG_LDO4VSET_MSK) != 0U ? 1800000 : 1200000) +
		   ((int32_t)vinc * 25000);
	return 0;
}

/* SPI DVS: clock one {AD[1:0], VLT[5:0]} command byte to set the buck voltage. */
static int regulator_max20356_spi_set_voltage(const struct device *dev, int32_t min_uv,
					      int32_t max_uv)
{
	const struct regulator_max20356_config *config = dev->config;
	uint16_t idx;
	uint8_t cmd;
	int ret;

	if (!spi_is_ready_dt(&config->dvs_spi)) {
		return -ENODEV;
	}

	ret = linear_range_get_win_index(config->uv_range, min_uv, max_uv, &idx);
	if (ret == -EINVAL) {
		return ret;
	}

	cmd = FIELD_PREP(MAX20356_DVS_SPI_ADD_MSK, config->desc->spi_add) |
	      FIELD_PREP(MAX20356_DVS_SPI_VLT_MSK, idx);

	const struct spi_buf buf = {.buf = &cmd, .len = sizeof(cmd)};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1U};

	return spi_write_dt(&config->dvs_spi, &tx);
}

static int regulator_max20356_set_voltage(const struct device *dev, int32_t min_uv, int32_t max_uv)
{
	const struct regulator_max20356_config *config = dev->config;
	uint16_t idx;
	int ret;

	if (config->desc->is_ldo4) {
		return regulator_max20356_ldo4_set_voltage(dev, min_uv, max_uv);
	}

	if (config->uv_range == NULL) {
		return -ENOTSUP;
	}

	if (config->desc->is_buck) {
		switch (config->dvs_mode) {
		case MAX20356_DVS_MODE_SPI:
			return regulator_max20356_spi_set_voltage(dev, min_uv, max_uv);
		case MAX20356_DVS_MODE_GPIO:
			/* Live voltage is chosen by the external MPC pins from the
			 * presets programmed at init; there is no single setpoint.
			 */
			return -ENOTSUP;
		default:
			break;
		}
	}

	ret = linear_range_get_win_index(config->uv_range, min_uv, max_uv, &idx);
	if (ret == -EINVAL) {
		return ret;
	}

	return regulator_max20356_reg_update(dev, config->desc->vset_reg, config->desc->vset_mask,
					     FIELD_PREP(config->desc->vset_mask, idx));
}

static int regulator_max20356_get_voltage(const struct device *dev, int32_t *volt_uv)
{
	const struct regulator_max20356_config *config = dev->config;
	uint8_t reg, val;
	int ret;

	if (config->desc->is_ldo4) {
		return regulator_max20356_ldo4_get_voltage(dev, volt_uv);
	}

	if (config->uv_range == NULL) {
		return -ENOTSUP;
	}

	/* SPI-set voltage is mirrored in Buck<n>DvsSpi and read back over I2C. */
	reg = (config->desc->is_buck && (config->dvs_mode == MAX20356_DVS_MODE_SPI))
		      ? config->desc->dvsspi_reg
		      : config->desc->vset_reg;

	ret = mfd_max20356_reg_read(config->mfd_dev, reg, &val);
	if (ret != 0) {
		return ret;
	}

	return linear_range_get_value(config->uv_range,
				      FIELD_GET(config->desc->vset_mask, val), volt_uv);
}

static unsigned int regulator_max20356_count_current_limits(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->iset_mask == 0U) {
		return 0;
	}

	return linear_range_values_count(&iset_ua_range);
}

static int regulator_max20356_list_current_limit(const struct device *dev, unsigned int idx,
						 int32_t *current_ua)
{
	const struct regulator_max20356_config *config = dev->config;

	if (config->desc->iset_mask == 0U) {
		return -ENOTSUP;
	}

	return linear_range_get_value(&iset_ua_range, idx, current_ua);
}

static int regulator_max20356_set_current_limit(const struct device *dev, int32_t min_ua,
						int32_t max_ua)
{
	const struct regulator_max20356_config *config = dev->config;
	uint16_t idx;
	int ret;

	if (config->desc->iset_mask == 0U) {
		return -ENOTSUP;
	}

	ret = linear_range_get_win_index(&iset_ua_range, min_ua, max_ua, &idx);
	if (ret == -EINVAL) {
		return ret;
	}

	return regulator_max20356_reg_update(dev, config->desc->iset_reg, config->desc->iset_mask,
					     FIELD_PREP(config->desc->iset_mask, idx));
}

static int regulator_max20356_get_current_limit(const struct device *dev, int32_t *curr_ua)
{
	const struct regulator_max20356_config *config = dev->config;
	uint8_t val;
	int ret;

	if (config->desc->iset_mask == 0U) {
		return -ENOTSUP;
	}

	ret = mfd_max20356_reg_read(config->mfd_dev, config->desc->iset_reg, &val);
	if (ret != 0) {
		return ret;
	}

	return linear_range_get_value(&iset_ua_range, FIELD_GET(config->desc->iset_mask, val),
				      curr_ua);
}

/* MPC pin pair (a<b over MPC0..7) to the Buck<n>DvsCfg[4:0] GPIO code. */
static int regulator_max20356_dvs_gpio_code(uint8_t a, uint8_t b, uint8_t *code)
{
	uint8_t val = MAX20356_DVS_CFG_GPIO_MIN;

	if ((a >= b) || (b > 7U)) {
		return -EINVAL;
	}

	for (uint8_t i = 0U; i < 8U; i++) {
		for (uint8_t j = i + 1U; j < 8U; j++) {
			if ((i == a) && (j == b)) {
				*code = val;
				return 0;
			}
			val++;
		}
	}

	return -EINVAL;
}

static int regulator_max20356_dvs_init(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;
	const struct regulator_max20356_desc *desc = config->desc;
	uint8_t cfg;
	int ret;

	switch (config->dvs_mode) {
	case MAX20356_DVS_MODE_I2C:
		cfg = MAX20356_DVS_CFG_I2C;
		break;
	case MAX20356_DVS_MODE_SPI:
		cfg = MAX20356_DVS_CFG_SPI;
		break;
	case MAX20356_DVS_MODE_GPIO:
		if (config->dvs_mpc_pair_len != 2U) {
			LOG_ERR("gpio DVS needs adi,dvs-mpc-pair of two MPC pins");
			return -EINVAL;
		}

		ret = regulator_max20356_dvs_gpio_code(config->dvs_mpc_pair[0],
						       config->dvs_mpc_pair[1], &cfg);
		if (ret != 0) {
			LOG_ERR("invalid adi,dvs-mpc-pair");
			return ret;
		}

		/* Program the four preset voltages selected by the MPC pin states. */
		for (uint8_t i = 0U; i < 4U; i++) {
			uint16_t idx;

			ret = linear_range_get_index(config->uv_range, config->dvs_voltages[i],
						     &idx);
			if (ret != 0) {
				LOG_ERR("adi,dvs-voltages[%u] out of range", i);
				return -EINVAL;
			}

			ret = mfd_max20356_reg_update_locked(
				config->mfd_dev, desc->lock, desc->dvsvlt_reg[i],
				desc->vset_mask, FIELD_PREP(desc->vset_mask, idx));
			if (ret != 0) {
				return ret;
			}
		}
		break;
	default:
		return -EINVAL;
	}

	/* Program DvsCfg[4:0] and the valley current (DvsCur). */
	cfg = FIELD_PREP(MAX20356_BUCK1DVSCFG0_BUCK1DVSCFG_MSK, cfg);
	if (config->dvs_valley_ua == 1000000U) {
		cfg |= MAX20356_BUCK1DVSCFG0_BUCK1DVSCUR_MSK;
	}

	return mfd_max20356_reg_update_locked(config->mfd_dev, desc->lock, desc->dvscfg0_reg,
					      MAX20356_BUCK1DVSCFG0_BUCK1DVSCFG_MSK |
						      MAX20356_BUCK1DVSCFG0_BUCK1DVSCUR_MSK,
					      cfg);
}

static int regulator_max20356_init(const struct device *dev)
{
	const struct regulator_max20356_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->mfd_dev)) {
		LOG_ERR("MFD parent device not ready");
		return -ENODEV;
	}

	regulator_common_data_init(dev);

	if (config->desc->is_buck) {
		ret = regulator_max20356_dvs_init(dev);
		if (ret != 0) {
			return ret;
		}
	}

	if (config->desc->is_ldo4 && config->ldo4_rtc) {
		if (mfd_max20356_get_variant(config->mfd_dev) != MAX20356_VARIANT_MAX20358) {
			LOG_ERR("LDO4RTC is only supported on the MAX20358");
			return -ENOTSUP;
		}

		ret = mfd_max20356_reg_update_locked(config->mfd_dev, config->desc->lock,
						     MAX20356_REG_LDO4CFG,
						     MAX20356_LDO4CFG_LDO4RTC_MSK,
						     MAX20356_LDO4CFG_LDO4RTC_MSK);
		if (ret != 0) {
			return ret;
		}
	}

	return regulator_common_init(dev, false);
}

static int regulator_max20356_ship_mode(const struct device *dev)
{
	const struct regulator_max20356_common_config *config = dev->config;

	return mfd_max20356_power_command(config->mfd_dev, MAX20356_PWR_OFF);
}

static int regulator_max20356_common_init(const struct device *dev)
{
	const struct regulator_max20356_common_config *config = dev->config;

	if (!device_is_ready(config->mfd_dev)) {
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(regulator_parent, parent_api) = {
	.ship_mode = regulator_max20356_ship_mode,
};

static DEVICE_API(regulator, api) = {
	.enable = regulator_max20356_enable,
	.disable = regulator_max20356_disable,
	.set_mode = regulator_max20356_set_mode,
	.get_mode = regulator_max20356_get_mode,
	.set_voltage = regulator_max20356_set_voltage,
	.get_voltage = regulator_max20356_get_voltage,
	.count_voltages = regulator_max20356_count_voltages,
	.list_voltage = regulator_max20356_list_voltage,
	.set_active_discharge = regulator_max20356_set_active_discharge,
	.get_active_discharge = regulator_max20356_get_active_discharge,
	.count_current_limits = regulator_max20356_count_current_limits,
	.list_current_limit = regulator_max20356_list_current_limit,
	.set_current_limit = regulator_max20356_set_current_limit,
	.get_current_limit = regulator_max20356_get_current_limit,
};

#define MAX20356_BUCK_DESC(n, cfg_reg_id)                                                          \
	static const struct regulator_max20356_desc __maybe_unused buck##n##_desc = {              \
		.ena_reg = MAX20356_REG_BUCK##n##ENA,                                              \
		.ena_mask = MAX20356_BUCK##n##ENA_BUCK##n##EN_MSK,                                 \
		.vset_reg = MAX20356_REG_BUCK##n##VSET,                                            \
		.vset_mask = MAX20356_BUCK##n##VSET_BUCK##n##VSET_MSK,                             \
		.cfg_reg = MAX20356_REG_##cfg_reg_id,                                              \
		.actdsc_mask = MAX20356_##cfg_reg_id##_BUCK##n##ACTDSC_MSK,                        \
		.iset_reg = MAX20356_REG_BUCK##n##ISET,                                            \
		.iset_mask = MAX20356_BUCK##n##ISET_BUCK##n##ISET_MSK,                             \
		.dvscfg0_reg = MAX20356_REG_BUCK##n##DVSCFG0,                                      \
		.dvsvlt_reg = {MAX20356_REG_BUCK##n##DVSCFG1, MAX20356_REG_BUCK##n##DVSCFG2,       \
			       MAX20356_REG_BUCK##n##DVSCFG3, MAX20356_REG_BUCK##n##DVSCFG4},      \
		.dvsspi_reg = MAX20356_REG_BUCK##n##DVSSPI,                                        \
		.spi_add = (n) - 1U,                                                               \
		.lock = MAX20356_LOCK_BUCK##n,                                                     \
		.lockable = true,                                                                  \
		.is_buck = true,                                                                   \
	}

MAX20356_BUCK_DESC(1, BUCK1CFG0);
MAX20356_BUCK_DESC(2, BUCK2CFG);
MAX20356_BUCK_DESC(3, BUCK3CFG);

static const struct regulator_max20356_desc __maybe_unused bbst_desc = {
	.ena_reg = MAX20356_REG_BBSTENA,
	.ena_mask = MAX20356_BBSTENA_BBSTEN_MSK,
	.vset_reg = MAX20356_REG_BBSTVSET,
	.vset_mask = MAX20356_BBSTVSET_BBSTVSET_MSK,
	.cfg_reg = MAX20356_REG_BBSTCFG,
	.actdsc_mask = MAX20356_BBSTCFG_BBSTACTDSC_MSK,
	.iset_reg = MAX20356_REG_BBSTISET,
	.iset_mask = MAX20356_BBSTISET_BBSTIPSET1_MSK,
	.lock = MAX20356_LOCK_BBST,
	.lockable = true,
};

#define MAX20356_LDO12_DESC(n)                                                                     \
	static const struct regulator_max20356_desc __maybe_unused ldo##n##_desc = {               \
		.ena_reg = MAX20356_REG_LDO##n##ENA,                                               \
		.ena_mask = MAX20356_LDO##n##ENA_LDO##n##EN_MSK,                                   \
		.vset_reg = MAX20356_REG_LDO##n##VSET,                                             \
		.vset_mask = MAX20356_LDO##n##VSET_LDO##n##VSET_MSK,                               \
		.cfg_reg = MAX20356_REG_LDO##n##CFG,                                               \
		.actdsc_mask = MAX20356_LDO##n##CFG_LDO##n##ACTDSC_MSK,                            \
		.mode_mask = MAX20356_LDO##n##CFG_LDO##n##MODE_MSK,                                \
		.lock = MAX20356_LOCK_LDO##n,                                                      \
		.lockable = true,                                                                  \
	}

MAX20356_LDO12_DESC(1);
MAX20356_LDO12_DESC(2);

static const struct regulator_max20356_desc __maybe_unused ldo3_desc = {
	.ena_reg = MAX20356_REG_LDO3ENA,
	.ena_mask = MAX20356_LDO3ENA_LDO3EN_MSK,
	.vset_reg = MAX20356_REG_LDO3VSET,
	.vset_mask = MAX20356_LDO3VSET_LDO3VSET_MSK,
	.cfg_reg = MAX20356_REG_LDO3CFG,
	.actdsc_mask = MAX20356_LDO3CFG_LDO3ACTDSC_MSK,
	.lock = MAX20356_LOCK_LDO3,
	.lockable = true,
};

static const struct regulator_max20356_desc __maybe_unused ldo4_desc = {
	.ena_reg = MAX20356_REG_LDO4ENA,
	.ena_mask = MAX20356_LDO4ENA_LDO4EN_MSK,
	.cfg_reg = MAX20356_REG_LDO4CFG,
	.lock = MAX20356_LOCK_LDO4,
	.lockable = true,
	.is_ldo4 = true,
};

#define MAX20356_LSW_DESC(n)                                                                       \
	static const struct regulator_max20356_desc __maybe_unused lsw##n##_desc = {               \
		.ena_reg = MAX20356_REG_LSW##n##ENA,                                               \
		.ena_mask = MAX20356_LSW##n##ENA_LSW##n##EN_MSK,                                   \
		.cfg_reg = MAX20356_REG_LSW##n##CFG,                                               \
		.actdsc_mask = MAX20356_LSW##n##CFG_LSW##n##ACTDSC_MSK,                            \
		.lock = MAX20356_LOCK_BUCK1,                                                       \
	}

MAX20356_LSW_DESC(1);
MAX20356_LSW_DESC(2);
MAX20356_LSW_DESC(3);

/* Buck output range from the per-rail voltage-step property. */
#define MAX20356_BUCK_RANGE(node_id)                                                               \
	((DT_ENUM_IDX(node_id, adi_buck_voltage_step_microvolt) == 0)                              \
		 ? &buck_range_10mv                                                                \
		 : (DT_ENUM_IDX(node_id, adi_buck_voltage_step_microvolt) == 1) ? &buck_range_25mv \
									       : &buck_range_50mv)

/* SPI spec only for a buck node carrying adi,dvs-spi; empty otherwise. */
#define MAX20356_DVS_SPI_INIT(node_id)                                                             \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, adi_dvs_spi),                                        \
		    (.dvs_spi = SPI_DT_SPEC_GET(DT_PHANDLE(node_id, adi_dvs_spi),                  \
						SPI_WORD_SET(8) | SPI_TRANSFER_MSB),), ())

#define REGULATOR_MAX20356_DEFINE(node_id, id, _desc, _range)                                      \
	static const struct regulator_max20356_config regulator_max20356_config_##id = {          \
		.common = REGULATOR_DT_COMMON_CONFIG_INIT(node_id),                               \
		.mfd_dev = DEVICE_DT_GET(DT_GPARENT(node_id)),                                    \
		.desc = &(_desc),                                                                 \
		.uv_range = (_range),                                                             \
		.ldo4_rtc = DT_PROP_OR(node_id, adi_ldo4_always_on_off_on_pfn1, 0),               \
		.dvs_mode = DT_ENUM_IDX_OR(node_id, adi_dvs_mode, MAX20356_DVS_MODE_I2C),          \
		.dvs_mpc_pair = DT_PROP_OR(node_id, adi_dvs_mpc_pair, {0}),                        \
		.dvs_mpc_pair_len = DT_PROP_LEN_OR(node_id, adi_dvs_mpc_pair, 0),                  \
		.dvs_voltages = DT_PROP_OR(node_id, adi_dvs_voltages, {0}),                        \
		.dvs_valley_ua = DT_PROP_OR(node_id, adi_dvs_valley_current_microamp, 500000),     \
		MAX20356_DVS_SPI_INIT(node_id)                                                     \
	};                                                                                         \
                                                                                                   \
	static struct regulator_max20356_data regulator_max20356_data_##id;                        \
	DEVICE_DT_DEFINE(node_id, regulator_max20356_init, NULL, &regulator_max20356_data_##id,    \
			 &regulator_max20356_config_##id, POST_KERNEL,                            \
			 CONFIG_REGULATOR_MAX20356_INIT_PRIORITY, &api);

#define REGULATOR_MAX20356_DEFINE_COND(inst, child, desc, range)                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_INST_CHILD(inst, child)),                                    \
		    (REGULATOR_MAX20356_DEFINE(DT_INST_CHILD(inst, child), child##inst, desc,      \
					       range)),                                            \
		    ())

#define REGULATOR_MAX20356_DEFINE_BUCK(inst, child, desc)                                          \
	COND_CODE_1(DT_NODE_EXISTS(DT_INST_CHILD(inst, child)),                                    \
		    (REGULATOR_MAX20356_DEFINE(DT_INST_CHILD(inst, child), child##inst, desc,      \
					       MAX20356_BUCK_RANGE(DT_INST_CHILD(inst, child)))),  \
		    ())

#define REGULATOR_MAX20356_DEFINE_ALL(inst)                                                        \
	static const struct regulator_max20356_common_config common_config_##inst = {             \
		.mfd_dev = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, regulator_max20356_common_init, NULL, NULL,                    \
			      &common_config_##inst, POST_KERNEL,                                 \
			      CONFIG_REGULATOR_MAX20356_COMMON_INIT_PRIORITY, &parent_api);        \
                                                                                                   \
	REGULATOR_MAX20356_DEFINE_BUCK(inst, buck1, buck1_desc)                                    \
	REGULATOR_MAX20356_DEFINE_BUCK(inst, buck2, buck2_desc)                                    \
	REGULATOR_MAX20356_DEFINE_BUCK(inst, buck3, buck3_desc)                                    \
	REGULATOR_MAX20356_DEFINE_COND(inst, buckboost, bbst_desc, &bbst_range)                    \
	REGULATOR_MAX20356_DEFINE_COND(inst, ldo1, ldo1_desc, &ldo1_2_range)                       \
	REGULATOR_MAX20356_DEFINE_COND(inst, ldo2, ldo2_desc, &ldo1_2_range)                       \
	REGULATOR_MAX20356_DEFINE_COND(inst, ldo3, ldo3_desc, &ldo3_range)                         \
	REGULATOR_MAX20356_DEFINE_COND(inst, ldo4, ldo4_desc, NULL)                                \
	REGULATOR_MAX20356_DEFINE_COND(inst, lsw1, lsw1_desc, NULL)                                \
	REGULATOR_MAX20356_DEFINE_COND(inst, lsw2, lsw2_desc, NULL)                                \
	REGULATOR_MAX20356_DEFINE_COND(inst, lsw3, lsw3_desc, NULL)

DT_INST_FOREACH_STATUS_OKAY(REGULATOR_MAX20356_DEFINE_ALL)
