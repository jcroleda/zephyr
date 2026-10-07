/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/dac/ad524x.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(AD524X, CONFIG_DAC_LOG_LEVEL);

/* There are 4 variations of this device that support 2.5k, 10k, 50k, and 100k
 * end-to-end (R_AB) resistances. The wiper position is set by an 8-bit code
 * (0-255), and the wiper-to-terminal resistance is:
 *
 *   R_WB(D) = (D / 256) * R_AB + R_W
 *
 * where R_W is the fixed wiper resistance (~320 ohms). The user supplies a
 * target resistance in whole ohms; the code is computed directly with a 64-bit
 * intermediate to avoid overflow and per-LSB rounding tables.
 */

#define AD524X_WR_CHANNEL_BIT   BIT(7)
#define AD524X_WR_SHUTDOWN_BIT  BIT(6)

#define AD524X_WIPER_OHM        320U
#define AD524X_STEPS            256U
#define AD524X_CODE_MAX         255U
#define AD524X_RESOLUTION       8U

enum ad524x_resistance_max {
	DAC_AD524X_RES_2P5K = 0,
	DAC_AD524X_RES_10K,
	DAC_AD524X_RES_50K,
	DAC_AD524X_RES_100K,
};

/* End-to-end resistance (R_AB) in ohms, indexed by enum ad524x_resistance_max. */
static const uint32_t dac_ad524x_rab_ohm[] = {
	[DAC_AD524X_RES_2P5K] = 2500U,
	[DAC_AD524X_RES_10K] = 10000U,
	[DAC_AD524X_RES_50K] = 50000U,
	[DAC_AD524X_RES_100K] = 100000U,
};

struct dac_ad524x_config {
	struct i2c_dt_spec i2c;
	enum ad524x_resistance_max max_res;
	uint32_t ch1_init_res;
	uint32_t ch2_init_res;
};

static inline const struct i2c_dt_spec *dac_ad524x_get_i2c(const struct device *dev)
{
	const struct dac_ad524x_config *config = dev->config;

	return &config->i2c;
}

/* Builds the instruction byte addressing the given channel (1 or 2) and the shutdown bit */
static inline uint8_t dac_ad524x_instruction(uint8_t channel, bool shutdown)
{
	return FIELD_PREP(AD524X_WR_CHANNEL_BIT, channel == 2) |
	       FIELD_PREP(AD524X_WR_SHUTDOWN_BIT, shutdown);
}

/* Writes the wiper code to the given channel (1 or 2). */
static int dac_ad524x_reg_write(const struct device *dev, uint8_t channel, uint8_t val)
{
	uint8_t buf[2] = { dac_ad524x_instruction(channel, false), val };

	return i2c_write_dt(dac_ad524x_get_i2c(dev), buf, ARRAY_SIZE(buf));
}

/* accepts a target resistance in whole ohms, internally converts to a code */
static int dac_ad524x_write_value(const struct device *dev,
				  uint8_t channel, uint32_t value)
{
	const struct dac_ad524x_config *cfg = dev->config;
	uint32_t rab_ohm;
	uint32_t code;

	if (channel < 1 || channel > 2) {
		return -EINVAL;
	}

	if (cfg->max_res >= ARRAY_SIZE(dac_ad524x_rab_ohm)) {
		return -ENOTSUP;
	}

	rab_ohm = dac_ad524x_rab_ohm[cfg->max_res];

	if (value < AD524X_WIPER_OHM) {
		value = AD524X_WIPER_OHM;
	}

	/* code = round((value - R_W) * 256 / R_AB) */
	code = (uint32_t)DIV_ROUND_CLOSEST((uint64_t)(value - AD524X_WIPER_OHM) * AD524X_STEPS,
					   rab_ohm);
	code = MIN(code, AD524X_CODE_MAX);

	return dac_ad524x_reg_write(dev, channel, (uint8_t)code);
}

int dac_ad524x_shutdown(const struct device *dev, bool shutdown)
{
	/* Shutdown is a device-wide instruction with no data byte, so the wiper
	 * registers are left untouched. The channel-select bit is irrelevant for
	 * this command; address channel 1.
	 */
	uint8_t instruction = dac_ad524x_instruction(1, shutdown);

	return i2c_write_dt(dac_ad524x_get_i2c(dev), &instruction, sizeof(instruction));
}

int dac_ad524x_read_value(const struct device *dev, uint8_t channel, uint8_t *code)
{
	uint8_t instruction;
	int ret;

	if (code == NULL || channel < 1 || channel > 2) {
		return -EINVAL;
	}

	/* Point the device at the requested channel then read back value */
	instruction = dac_ad524x_instruction(channel, false);

	ret = i2c_write_dt(dac_ad524x_get_i2c(dev), &instruction, sizeof(instruction));
	if (ret != 0) {
		return ret;
	}

	return i2c_read_dt(dac_ad524x_get_i2c(dev), code, sizeof(*code));
}

static int dac_ad524x_channel_setup(const struct device *dev,
				const struct dac_channel_cfg *channel_cfg)
{
	const struct dac_ad524x_config *cfg = dev->config;
	uint32_t init_res = 0;

	if (channel_cfg->channel_id < 1 || channel_cfg->channel_id > 2) {
		return -EINVAL;
	}

	if (channel_cfg->resolution != AD524X_RESOLUTION) {
		return -EINVAL;
	}

	if (channel_cfg->internal) {
		return -ENOTSUP;
	}

	init_res = (channel_cfg->channel_id == 2) ? cfg->ch2_init_res : cfg->ch1_init_res;

	return dac_ad524x_write_value(dev, channel_cfg->channel_id, init_res);
}

static int dac_ad524x_init(const struct device *dev)
{
	const struct dac_ad524x_config *cfg = dev->config;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->i2c.bus);
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(dac, ad524x_driver_api) = {
	.channel_setup = dac_ad524x_channel_setup,
	.write_value = dac_ad524x_write_value,
};

#define DAC_AD524X_DEFINE(inst, name)								\
	static const struct dac_ad524x_config config_##name##_##inst = {				\
		.i2c = I2C_DT_SPEC_INST_GET(inst),						\
		.max_res = DT_INST_ENUM_IDX(inst, max_resistance),				\
		.ch1_init_res = DT_INST_PROP(inst, channel_1_initial_resistance),		\
		.ch2_init_res = DT_INST_PROP(inst, channel_2_initial_resistance),		\
	};											\
												\
	DEVICE_DT_INST_DEFINE(inst, dac_ad524x_init, NULL, NULL,				\
			      &config_##name##_##inst, POST_KERNEL, DAC_AD524X_INIT_PRIORITY,	\
			      &ad524x_driver_api);

#define DT_DRV_COMPAT adi_ad5243
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
DT_INST_FOREACH_STATUS_OKAY_VARGS(DAC_AD524X_DEFINE, ad5243)
#endif
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT adi_ad5248
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
DT_INST_FOREACH_STATUS_OKAY_VARGS(DAC_AD524X_DEFINE, ad5248)
#endif
#undef DT_DRV_COMPAT
