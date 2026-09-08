/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register-model emulator for the MAX20356/MAX20358 PMIC. Backs the MFD parent's
 * I2C accesses with a 256-byte register file. Read-only registers (RevID, the
 * Status/DVS-SPI/PFN/BootCfg group) ignore writes from the I2C path so they read
 * back a constant value; tests seed them through mfd_max20356_emul_set_reg().
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

LOG_MODULE_REGISTER(max20356_emul, CONFIG_MFD_LOG_LEVEL);

#define MAX20356_EMUL_NUM_REGS 256

/* Silicon revision reported by RevID (0x00); value is arbitrary for emulation. */
#define MAX20356_EMUL_REVID 0x01U

/* Passwords accepted by the LockUnlock1 register. */
#define MAX20356_EMUL_PASSWD_UNLOCK 0x55U
#define MAX20356_EMUL_PASSWD_LOCK   0xAAU

struct max20356_emul_data {
	uint8_t regs[MAX20356_EMUL_NUM_REGS];
	/* Per-LockMsk1-bit lock state: bit set => that domain's registers are
	 * locked and writes to them from the I2C path are dropped.
	 */
	uint8_t locked_mask;
	bool fail;
};

/* LockMsk1 bit guarding a lockable data register, or 0 if the register is not
 * password-protected. Covers the regulator rails driven by the child driver.
 */
static uint8_t max20356_emul_reg_lock_bit(uint8_t reg)
{
	switch (reg) {
	case MAX20356_REG_BUCK1ENA:
	case MAX20356_REG_BUCK1CFG0:
	case MAX20356_REG_BUCK1ISET:
	case MAX20356_REG_BUCK1VSET:
	case MAX20356_REG_BUCK1DVSCFG0:
	case MAX20356_REG_BUCK1DVSCFG1:
	case MAX20356_REG_BUCK1DVSCFG2:
	case MAX20356_REG_BUCK1DVSCFG3:
	case MAX20356_REG_BUCK1DVSCFG4:
		return MAX20356_LOCKMSK1_BK1LCK_MSK;
	case MAX20356_REG_BUCK2ENA:
	case MAX20356_REG_BUCK2VSET:
	case MAX20356_REG_BUCK2ISET:
		return MAX20356_LOCKMSK1_BK2LCK_MSK;
	case MAX20356_REG_BUCK3ENA:
	case MAX20356_REG_BUCK3VSET:
	case MAX20356_REG_BUCK3ISET:
		return MAX20356_LOCKMSK1_BK3LCK_MSK;
	case MAX20356_REG_BBSTENA:
	case MAX20356_REG_BBSTVSET:
	case MAX20356_REG_BBSTISET:
		return MAX20356_LOCKMSK1_BBLCK_MSK;
	case MAX20356_REG_LDO1ENA:
	case MAX20356_REG_LDO1VSET:
	case MAX20356_REG_LDO1CFG:
		return MAX20356_LOCKMSK1_LD1LCK_MSK;
	case MAX20356_REG_LDO2ENA:
	case MAX20356_REG_LDO2VSET:
	case MAX20356_REG_LDO2CFG:
		return MAX20356_LOCKMSK1_LD2LCK_MSK;
	case MAX20356_REG_LDO3ENA:
	case MAX20356_REG_LDO3VSET:
	case MAX20356_REG_LDO3CFG:
		return MAX20356_LOCKMSK1_LD3LCK_MSK;
	case MAX20356_REG_LDO4ENA:
	case MAX20356_REG_LDO4CFG:
		return MAX20356_LOCKMSK1_LD4LCK_MSK;
	default:
		return 0U;
	}
}

struct max20356_emul_cfg {
	uint16_t addr;
};

/* Registers whose every field is read-only: writes from the I2C path are
 * dropped so they present a constant value to the driver.
 */
static bool max20356_emul_reg_is_ro(uint8_t reg)
{
	switch (reg) {
	case MAX20356_REG_REVID:
	case MAX20356_REG_STATUS0:
	case MAX20356_REG_STATUS1:
	case MAX20356_REG_STATUS2:
	case MAX20356_REG_STATUS3:
	case MAX20356_REG_STATUS4:
	case MAX20356_REG_BUCK1DVSSPI:
	case MAX20356_REG_BUCK2DVSSPI:
	case MAX20356_REG_BUCK3DVSSPI:
	case MAX20356_REG_MPCITRSTS:
	case MAX20356_REG_PFN:
	case MAX20356_REG_BOOTCFG:
		return true;
	default:
		return false;
	}
}

void mfd_max20356_emul_set_reg(const struct emul *target, uint8_t reg, uint8_t val)
{
	struct max20356_emul_data *data = target->data;

	data->regs[reg] = val;
}

void mfd_max20356_emul_get_reg(const struct emul *target, uint8_t reg, uint8_t *val)
{
	struct max20356_emul_data *data = target->data;

	*val = data->regs[reg];
}

void mfd_max20356_emul_reset(const struct emul *target)
{
	struct max20356_emul_data *data = target->data;

	memset(data->regs, 0, sizeof(data->regs));
	data->regs[MAX20356_REG_REVID] = MAX20356_EMUL_REVID;
	data->locked_mask = 0U;
	data->fail = false;
}

void mfd_max20356_emul_set_fail(const struct emul *target, bool fail)
{
	struct max20356_emul_data *data = target->data;

	data->fail = fail;
}

void mfd_max20356_emul_set_locked(const struct emul *target, uint8_t lockmsk1_bits, bool locked)
{
	struct max20356_emul_data *data = target->data;

	if (locked) {
		data->locked_mask |= lockmsk1_bits;
	} else {
		data->locked_mask &= (uint8_t)~lockmsk1_bits;
	}
}

static int max20356_emul_transfer_i2c(const struct emul *target, struct i2c_msg *msgs,
				      int num_msgs, int addr)
{
	struct max20356_emul_data *data = target->data;
	uint8_t reg;

	ARG_UNUSED(addr);

	if (data->fail) {
		return -EIO;
	}

	i2c_dump_msgs_rw(target->dev, msgs, num_msgs, addr, false);

	if (num_msgs < 1) {
		LOG_ERR("Invalid number of messages: %d", num_msgs);
		return -EIO;
	}

	/* Every transaction starts with a 1-byte register address write. */
	if ((msgs->flags & I2C_MSG_READ) == I2C_MSG_READ || msgs->len < 1) {
		LOG_ERR("Unexpected msg0 (flags 0x%x len %d)", msgs->flags, msgs->len);
		return -EIO;
	}

	reg = msgs->buf[0];

	if (num_msgs == 1) {
		/* Write: register address followed by one or more data bytes. */
		for (int i = 1; i < msgs->len; i++) {
			uint8_t target_reg = reg + (i - 1);
			uint8_t lock_bit;

			if (max20356_emul_reg_is_ro(target_reg)) {
				continue;
			}

			/* Password write: unlock/lock the unmasked domains. A domain
			 * is unmasked when its LockMsk1 bit is 0.
			 */
			if (target_reg == MAX20356_REG_LOCKUNLOCK1) {
				uint8_t unmasked = (uint8_t)~data->regs[MAX20356_REG_LOCKMSK1];

				if (msgs->buf[i] == MAX20356_EMUL_PASSWD_UNLOCK) {
					data->locked_mask &= (uint8_t)~unmasked;
				} else if (msgs->buf[i] == MAX20356_EMUL_PASSWD_LOCK) {
					data->locked_mask |= unmasked;
				}

				data->regs[target_reg] = msgs->buf[i];
				continue;
			}

			/* Drop writes to a register whose lock domain is engaged. */
			lock_bit = max20356_emul_reg_lock_bit(target_reg);
			if ((lock_bit != 0U) && ((data->locked_mask & lock_bit) != 0U)) {
				continue;
			}

			data->regs[target_reg] = msgs->buf[i];
		}

		return 0;
	}

	if (num_msgs == 2) {
		/* Read: address write followed by a read of one or more bytes. */
		msgs++;

		if ((msgs->flags & I2C_MSG_READ) != I2C_MSG_READ) {
			LOG_ERR("Unexpected msg1 (flags 0x%x)", msgs->flags);
			return -EIO;
		}

		for (int i = 0; i < msgs->len; i++) {
			msgs->buf[i] = data->regs[reg + i];
		}

		return 0;
	}

	LOG_ERR("Unexpected number of messages: %d", num_msgs);
	return -EIO;
}

static int max20356_emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(parent);

	mfd_max20356_emul_reset(target);

	return 0;
}

static const struct i2c_emul_api max20356_emul_api_i2c = {
	.transfer = max20356_emul_transfer_i2c,
};

#define MAX20356_EMUL_DEFINE(inst, part)                                                           \
	static struct max20356_emul_data max20356_emul_data_##part##_##inst;                              \
                                                                                                   \
	static const struct max20356_emul_cfg max20356_emul_cfg_##part##_##inst = {                       \
		.addr = DT_INST_REG_ADDR(inst),                                                                  \
	};                                                                                                \
                                                                                                   \
	EMUL_DT_INST_DEFINE(inst, max20356_emul_init, &max20356_emul_data_##part##_##inst,                \
			    &max20356_emul_cfg_##part##_##inst, &max20356_emul_api_i2c, NULL)

#define DT_DRV_COMPAT adi_max20356
#define MAX20356_EMUL_DEFINE_356(inst) MAX20356_EMUL_DEFINE(inst, max20356)
DT_INST_FOREACH_STATUS_OKAY(MAX20356_EMUL_DEFINE_356)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT adi_max20358
#define MAX20356_EMUL_DEFINE_358(inst) MAX20356_EMUL_DEFINE(inst, max20358)
DT_INST_FOREACH_STATUS_OKAY(MAX20356_EMUL_DEFINE_358)
#undef DT_DRV_COMPAT
