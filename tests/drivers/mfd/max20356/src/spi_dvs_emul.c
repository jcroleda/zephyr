/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal SPI target emulator for the MAX20356 SPI DVS (Mode 2) test. It backs
 * the vnd,spi-device node wired to the emulated SPI controller, captures the last
 * command byte {AD[1:0], VLT[5:0]} clocked to it, and mirrors VLT into the MFD
 * I2C emulator's Buck<n>DvsSpi register so the driver can read the voltage back.
 */

#define DT_DRV_COMPAT vnd_spi_device

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"
#include "spi_dvs_emul.h"

#define SPI_DVS_ADD_MSK GENMASK(7, 6)
#define SPI_DVS_VLT_MSK GENMASK(5, 0)

/* AD[1:0] -> Buck<n>DvsSpi mirror register. */
static const uint8_t spi_dvs_mirror_reg[] = {
	MAX20356_REG_BUCK1DVSSPI,
	MAX20356_REG_BUCK2DVSSPI,
	MAX20356_REG_BUCK3DVSSPI,
};

static struct spi_dvs_emul_state {
	const struct emul *mfd_emul;
	uint8_t last_cmd;
	bool got_cmd;
} spi_dvs_state;

void spi_dvs_emul_bind(const struct emul *mfd_emul)
{
	spi_dvs_state.mfd_emul = mfd_emul;
	spi_dvs_state.got_cmd = false;
	spi_dvs_state.last_cmd = 0;
}

uint8_t spi_dvs_emul_last_cmd(void)
{
	return spi_dvs_state.last_cmd;
}

bool spi_dvs_emul_got_cmd(void)
{
	return spi_dvs_state.got_cmd;
}

static int spi_dvs_emul_io(const struct emul *target, const struct spi_config *config,
			   const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	ARG_UNUSED(target);
	ARG_UNUSED(config);
	ARG_UNUSED(rx_bufs);

	if ((tx_bufs == NULL) || (tx_bufs->count < 1U) || (tx_bufs->buffers[0].buf == NULL) ||
	    (tx_bufs->buffers[0].len < 1U)) {
		return -EIO;
	}

	const uint8_t cmd = ((const uint8_t *)tx_bufs->buffers[0].buf)[0];
	uint8_t add = FIELD_GET(SPI_DVS_ADD_MSK, cmd);

	spi_dvs_state.last_cmd = cmd;
	spi_dvs_state.got_cmd = true;

	if ((spi_dvs_state.mfd_emul != NULL) && (add < ARRAY_SIZE(spi_dvs_mirror_reg))) {
		mfd_max20356_emul_set_reg(spi_dvs_state.mfd_emul, spi_dvs_mirror_reg[add],
					  FIELD_GET(SPI_DVS_VLT_MSK, cmd));
	}

	return 0;
}

static const struct spi_emul_api spi_dvs_emul_api = {
	.io = spi_dvs_emul_io,
};

static int spi_dvs_emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(parent);
	ARG_UNUSED(target);

	return 0;
}

/* Stub device init: the SPI emul controller links to each child's device. */
static int spi_dvs_dev_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

#define SPI_DVS_EMUL_DEFINE(n)                                                                     \
	DEVICE_DT_INST_DEFINE(n, spi_dvs_dev_init, NULL, NULL, NULL, POST_KERNEL,                  \
			      CONFIG_SPI_INIT_PRIORITY, NULL);                                    \
	EMUL_DT_INST_DEFINE(n, spi_dvs_emul_init, NULL, NULL, &spi_dvs_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(SPI_DVS_EMUL_DEFINE)
