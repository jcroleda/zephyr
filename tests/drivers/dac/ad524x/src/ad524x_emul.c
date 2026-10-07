/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal I2C emulator for the AD5243/AD5248 digital potentiometer. It stores the
 * 8-bit wiper code per channel, tracks the device-wide shutdown state, and models
 * the readback protocol: a read returns the code of the most recently addressed
 * channel.
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ad524x_emul.h"

LOG_MODULE_REGISTER(ad524x_emul, CONFIG_DAC_LOG_LEVEL);

/* Instruction byte layout, mirrored from the driver. */
#define AD524X_EMUL_CHANNEL_BIT  BIT(7)
#define AD524X_EMUL_SHUTDOWN_BIT BIT(6)

struct ad524x_emul_data {
	/* Wiper code, indexed by channel (0 -> ch1, 1 -> ch2). */
	uint8_t code[AD524X_EMUL_NUM_CHANNELS];
	/* Channel addressed by the most recent instruction byte (0 or 1). */
	uint8_t targeted;
	/* Device-wide shutdown state. */
	bool shutdown;
};

int ad524x_emul_get_code(const struct emul *target, uint8_t channel, uint8_t *code)
{
	struct ad524x_emul_data *data = target->data;

	if (code == NULL || channel < 1 || channel > AD524X_EMUL_NUM_CHANNELS) {
		return -EINVAL;
	}

	*code = data->code[channel - 1];

	return 0;
}

int ad524x_emul_get_shutdown(const struct emul *target, bool *shutdown)
{
	struct ad524x_emul_data *data = target->data;

	if (shutdown == NULL) {
		return -EINVAL;
	}

	*shutdown = data->shutdown;

	return 0;
}

static int ad524x_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	struct ad524x_emul_data *data = target->data;
	uint8_t instruction;

	ARG_UNUSED(addr);

	i2c_dump_msgs_rw(target->dev, msgs, num_msgs, addr, false);

	/* The driver issues one message per transfer: a 1- or 2-byte write, or a
	 * single-byte read.
	 */
	if (num_msgs != 1) {
		LOG_ERR("unexpected message count: %d", num_msgs);
		return -EIO;
	}

	if ((msgs[0].flags & I2C_MSG_READ) != 0) {
		/* Readback: return the code of the most recently addressed channel. */
		if (msgs[0].len != 1) {
			LOG_ERR("unexpected read length: %u", msgs[0].len);
			return -EIO;
		}

		msgs[0].buf[0] = data->code[data->targeted];
		LOG_DBG("read channel %u -> code %u", data->targeted + 1, msgs[0].buf[0]);

		return 0;
	}

	if (msgs[0].len < 1 || msgs[0].len > 2) {
		LOG_ERR("unexpected write length: %u", msgs[0].len);
		return -EIO;
	}

	instruction = msgs[0].buf[0];
	data->targeted = FIELD_GET(AD524X_EMUL_CHANNEL_BIT, instruction);
	data->shutdown = (FIELD_GET(AD524X_EMUL_SHUTDOWN_BIT, instruction) != 0);

	if (msgs[0].len == 2) {
		/* [instruction][code] updates the addressed channel's wiper. */
		data->code[data->targeted] = msgs[0].buf[1];
		LOG_DBG("channel %u <- code %u, shutdown %u", data->targeted + 1,
			msgs[0].buf[1], data->shutdown);
	} else {
		/* Data-less instruction (channel select / shutdown command). */
		LOG_DBG("target channel %u, shutdown %u", data->targeted + 1, data->shutdown);
	}

	return 0;
}

static int ad524x_emul_init(const struct emul *target, const struct device *parent)
{
	struct ad524x_emul_data *data = target->data;

	ARG_UNUSED(parent);

	for (size_t i = 0; i < AD524X_EMUL_NUM_CHANNELS; i++) {
		data->code[i] = 0;
	}
	data->targeted = 0;
	data->shutdown = false;

	return 0;
}

static const struct i2c_emul_api ad524x_emul_api_i2c = {
	.transfer = ad524x_emul_transfer,
};

#define AD524X_EMUL_DEFINE(inst)                                                                    \
	static struct ad524x_emul_data ad524x_emul_data_##inst;                                     \
	EMUL_DT_INST_DEFINE(inst, ad524x_emul_init, &ad524x_emul_data_##inst, NULL,                 \
			    &ad524x_emul_api_i2c, NULL)

#define DT_DRV_COMPAT adi_ad5243
DT_INST_FOREACH_STATUS_OKAY(AD524X_EMUL_DEFINE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT adi_ad5248
DT_INST_FOREACH_STATUS_OKAY(AD524X_EMUL_DEFINE)
#undef DT_DRV_COMPAT
