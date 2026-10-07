/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the AD5243/AD5248 digital potentiometer driver. The driver takes a
 * target wiper resistance in ohms and converts it to an 8-bit wiper code using
 *
 *   code = round((target - R_W) * 256 / R_AB),  clamped to [0, 255]
 *
 * where R_W is the fixed wiper resistance (320 ohms) and R_AB is the device
 * end-to-end resistance. The realized resistance is
 *
 *   R_WB = code / 256 * R_AB + R_W
 *
 * These tests drive the public DAC API and read the captured code back from the
 * emulator to check that each channel reaches the requested resistance to within
 * half a least-significant bit across the full 8-bit range and all four R_AB
 * variants.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/dac/ad524x.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/ztest.h>

#include "ad524x_emul.h"

/* Mirrors the driver's fixed wiper resistance and code range. */
#define AD524X_WIPER_OHM  320U
#define AD524X_STEPS      256U
#define AD524X_CODE_MAX   255U
#define AD524X_RESOLUTION 8U

struct ad524x_variant {
	const char *name;
	const struct device *dev;
	const struct emul *emul;
	uint32_t rab_ohm;
};

#define AD524X_VARIANT(nodelabel, rab)                                                              \
	{                                                                                          \
		.name = #nodelabel,                                                                 \
		.dev = DEVICE_DT_GET(DT_NODELABEL(nodelabel)),                                      \
		.emul = EMUL_DT_GET(DT_NODELABEL(nodelabel)),                                       \
		.rab_ohm = (rab),                                                                   \
	}

static struct ad524x_variant variants[] = {
	AD524X_VARIANT(ad5243_2k5, 2500U),
	AD524X_VARIANT(ad5243_10k, 10000U),
	AD524X_VARIANT(ad5243_50k, 50000U),
	AD524X_VARIANT(ad5243_100k, 100000U),
};

/* Reference conversion, computed independently of the driver. */
static uint8_t expected_code(uint32_t rab_ohm, uint32_t target_ohm)
{
	uint32_t code;

	if (target_ohm < AD524X_WIPER_OHM) {
		target_ohm = AD524X_WIPER_OHM;
	}

	code = (uint32_t)(((uint64_t)(target_ohm - AD524X_WIPER_OHM) * AD524X_STEPS + rab_ohm / 2) /
			  rab_ohm);

	return (uint8_t)MIN(code, AD524X_CODE_MAX);
}

/* Realized wiper-to-terminal-B resistance for a given code. */
static uint32_t code_to_ohm(uint32_t rab_ohm, uint8_t code)
{
	return (uint32_t)(((uint64_t)code * rab_ohm) / AD524X_STEPS) + AD524X_WIPER_OHM;
}

static void *ad524x_setup(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		zassert_true(device_is_ready(variants[i].dev), "%s not ready",
			     variants[i].name);
	}

	return NULL;
}

/* Every variant must be configurable on both channels at 8-bit resolution. */
ZTEST(ad524x, test_channel_setup)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		for (uint8_t ch = 1; ch <= 2; ch++) {
			struct dac_channel_cfg cfg = {
				.channel_id = ch,
				.resolution = AD524X_RESOLUTION,
			};
			int ret = dac_channel_setup(variants[i].dev, &cfg);

			zassert_ok(ret, "%s ch%u setup failed (%d)", variants[i].name, ch, ret);
		}
	}
}

/* A non-8-bit resolution must be rejected. */
ZTEST(ad524x, test_channel_setup_bad_resolution)
{
	struct dac_channel_cfg cfg = {
		.channel_id = 1,
		.resolution = 12,
	};

	zassert_equal(dac_channel_setup(variants[0].dev, &cfg), -EINVAL,
		      "non-8-bit resolution should be rejected");
}

/* Channel ids outside [1, 2] must be rejected by both API entry points. */
ZTEST(ad524x, test_invalid_channel)
{
	const struct device *dev = variants[0].dev;
	struct dac_channel_cfg cfg = {
		.channel_id = 0,
		.resolution = AD524X_RESOLUTION,
	};

	zassert_equal(dac_channel_setup(dev, &cfg), -EINVAL, "channel 0 setup should fail");

	cfg.channel_id = 3;
	zassert_equal(dac_channel_setup(dev, &cfg), -EINVAL, "channel 3 setup should fail");

	zassert_equal(dac_write_value(dev, 0, 1000), -EINVAL, "channel 0 write should fail");
	zassert_equal(dac_write_value(dev, 3, 1000), -EINVAL, "channel 3 write should fail");
}

/*
 * The core accuracy check: sweep a set of target resistances spanning each
 * variant's range and confirm the driver programs the nearest 8-bit code and
 * lands within half an LSB of the request.
 */
ZTEST(ad524x, test_resistance_accuracy)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		const struct ad524x_variant *v = &variants[i];
		/* Half an LSB in ohms, rounded up, is the worst-case quantization error. */
		uint32_t half_lsb = DIV_ROUND_UP(v->rab_ohm, 2U * AD524X_STEPS);
		/*
		 * Full-scale R_AB would need code 256, which the 8-bit range cannot
		 * reach, so the sweep spans only the achievable window
		 * [R_W, R_W + 255/256 * R_AB]. Out-of-range targets are covered by
		 * test_saturation.
		 */
		uint32_t span_ohm = code_to_ohm(v->rab_ohm, AD524X_CODE_MAX) - AD524X_WIPER_OHM;

		/* Ten evenly spaced targets across the achievable window. */
		for (uint32_t step = 0; step <= 10U; step++) {
			uint32_t target = AD524X_WIPER_OHM + (span_ohm * step) / 10U;
			uint8_t want = expected_code(v->rab_ohm, target);
			uint8_t got;
			uint32_t realized;
			int ret;

			ret = dac_write_value(v->dev, 1, target);
			zassert_ok(ret, "%s write %u ohm failed (%d)", v->name, target, ret);

			ret = ad524x_emul_get_code(v->emul, 1, &got);
			zassert_ok(ret, "%s code read failed (%d)", v->name, ret);
			zassert_equal(got, want, "%s target %u ohm: code %u, expected %u",
				      v->name, target, got, want);

			realized = code_to_ohm(v->rab_ohm, got);
			zassert_true(realized <= target + half_lsb &&
					     target <= realized + half_lsb,
				     "%s target %u ohm: realized %u ohm exceeds half-LSB %u",
				     v->name, target, realized, half_lsb);
		}
	}
}

/* Every code from 0 to 255 must be reachable by requesting its nominal resistance. */
ZTEST(ad524x, test_full_code_range)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		const struct ad524x_variant *v = &variants[i];

		for (uint32_t code = 0; code <= AD524X_CODE_MAX; code++) {
			uint32_t target = code_to_ohm(v->rab_ohm, (uint8_t)code);
			uint8_t got;
			int ret;

			ret = dac_write_value(v->dev, 2, target);
			zassert_ok(ret, "%s write code %u failed (%d)", v->name, code, ret);

			ret = ad524x_emul_get_code(v->emul, 2, &got);
			zassert_ok(ret, "%s code read failed (%d)", v->name, ret);
			zassert_equal(got, (uint8_t)code,
				      "%s nominal %u ohm: code %u, expected %u", v->name, target,
				      got, code);
		}
	}
}

/* Requests below the wiper floor clamp to code 0; requests above R_AB clamp to 255. */
ZTEST(ad524x, test_saturation)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		const struct ad524x_variant *v = &variants[i];
		uint8_t got;
		int ret;

		/* Below the 320 ohm wiper floor -> minimum code. */
		ret = dac_write_value(v->dev, 1, 0);
		zassert_ok(ret, "%s write 0 ohm failed (%d)", v->name, ret);
		ret = ad524x_emul_get_code(v->emul, 1, &got);
		zassert_ok(ret, "%s code read failed (%d)", v->name, ret);
		zassert_equal(got, 0, "%s below floor: code %u, expected 0", v->name, got);

		/* Well above end-to-end resistance -> maximum code. */
		ret = dac_write_value(v->dev, 1, v->rab_ohm * 2U);
		zassert_ok(ret, "%s write over-range failed (%d)", v->name, ret);
		ret = ad524x_emul_get_code(v->emul, 1, &got);
		zassert_ok(ret, "%s code read failed (%d)", v->name, ret);
		zassert_equal(got, AD524X_CODE_MAX, "%s over-range: code %u, expected %u",
			      v->name, got, AD524X_CODE_MAX);
	}
}

/* The two channels must be programmable independently. */
ZTEST(ad524x, test_channels_independent)
{
	const struct ad524x_variant *v = &variants[1]; /* 10k */
	uint32_t target_ch1 = v->rab_ohm / 4U + AD524X_WIPER_OHM;
	uint32_t target_ch2 = (3U * v->rab_ohm) / 4U + AD524X_WIPER_OHM;
	uint8_t code1, code2;

	zassert_ok(dac_write_value(v->dev, 1, target_ch1), "ch1 write failed");
	zassert_ok(dac_write_value(v->dev, 2, target_ch2), "ch2 write failed");

	zassert_ok(ad524x_emul_get_code(v->emul, 1, &code1), "ch1 read failed");
	zassert_ok(ad524x_emul_get_code(v->emul, 2, &code2), "ch2 read failed");

	zassert_equal(code1, expected_code(v->rab_ohm, target_ch1), "ch1 code mismatch");
	zassert_equal(code2, expected_code(v->rab_ohm, target_ch2), "ch2 code mismatch");
	zassert_not_equal(code1, code2, "channels should hold distinct codes");
}

/* Enabling and disabling shutdown sets and clears the device-wide shutdown bit. */
ZTEST(ad524x, test_shutdown_toggle)
{
	const struct ad524x_variant *v = &variants[1]; /* 10k */
	bool shutdown;

	zassert_ok(dac_ad524x_shutdown(v->dev, true), "enter shutdown failed");
	zassert_ok(ad524x_emul_get_shutdown(v->emul, &shutdown), "shutdown read failed");
	zassert_true(shutdown, "shutdown bit not set");

	zassert_ok(dac_ad524x_shutdown(v->dev, false), "leave shutdown failed");
	zassert_ok(ad524x_emul_get_shutdown(v->emul, &shutdown), "shutdown read failed");
	zassert_false(shutdown, "shutdown bit not cleared");
}

/* Shutdown must preserve the programmed wiper codes on both channels. */
ZTEST(ad524x, test_shutdown_preserves_code)
{
	const struct ad524x_variant *v = &variants[2]; /* 50k */
	uint32_t target1 = v->rab_ohm / 3U + AD524X_WIPER_OHM;
	uint32_t target2 = (2U * v->rab_ohm) / 3U + AD524X_WIPER_OHM;
	uint8_t want1 = expected_code(v->rab_ohm, target1);
	uint8_t want2 = expected_code(v->rab_ohm, target2);
	uint8_t got1, got2;

	zassert_ok(dac_write_value(v->dev, 1, target1), "ch1 write failed");
	zassert_ok(dac_write_value(v->dev, 2, target2), "ch2 write failed");

	zassert_ok(dac_ad524x_shutdown(v->dev, true), "enter shutdown failed");
	zassert_ok(ad524x_emul_get_code(v->emul, 1, &got1), "ch1 code read failed");
	zassert_ok(ad524x_emul_get_code(v->emul, 2, &got2), "ch2 code read failed");
	zassert_equal(got1, want1, "shutdown changed ch1 code: got %u, expected %u", got1, want1);
	zassert_equal(got2, want2, "shutdown changed ch2 code: got %u, expected %u", got2, want2);

	zassert_ok(dac_ad524x_shutdown(v->dev, false), "leave shutdown failed");
	zassert_ok(ad524x_emul_get_code(v->emul, 1, &got1), "ch1 code read failed");
	zassert_ok(ad524x_emul_get_code(v->emul, 2, &got2), "ch2 code read failed");
	zassert_equal(got1, want1, "resume changed ch1 code: got %u, expected %u", got1, want1);
	zassert_equal(got2, want2, "resume changed ch2 code: got %u, expected %u", got2, want2);
}

/* The readback API returns the code programmed through dac_write_value(). */
ZTEST(ad524x, test_read_value)
{
	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		const struct ad524x_variant *v = &variants[i];

		for (uint8_t ch = 1; ch <= 2; ch++) {
			uint32_t target = (v->rab_ohm * ch) / 3U + AD524X_WIPER_OHM;
			uint8_t want = expected_code(v->rab_ohm, target);
			uint8_t code;
			int ret;

			zassert_ok(dac_write_value(v->dev, ch, target),
				   "%s ch%u write failed", v->name, ch);

			ret = dac_ad524x_read_value(v->dev, ch, &code);
			zassert_ok(ret, "%s ch%u readback failed (%d)", v->name, ch, ret);
			zassert_equal(code, want, "%s ch%u readback %u, expected %u", v->name,
				      ch, code, want);
		}
	}
}

/* Readback returns the two channels' distinct codes independently. */
ZTEST(ad524x, test_read_value_independent)
{
	const struct ad524x_variant *v = &variants[3]; /* 100k */
	uint32_t target1 = v->rab_ohm / 4U + AD524X_WIPER_OHM;
	uint32_t target2 = (3U * v->rab_ohm) / 4U + AD524X_WIPER_OHM;
	uint8_t code1, code2;

	zassert_ok(dac_write_value(v->dev, 1, target1), "ch1 write failed");
	zassert_ok(dac_write_value(v->dev, 2, target2), "ch2 write failed");

	zassert_ok(dac_ad524x_read_value(v->dev, 1, &code1), "ch1 readback failed");
	zassert_ok(dac_ad524x_read_value(v->dev, 2, &code2), "ch2 readback failed");

	zassert_equal(code1, expected_code(v->rab_ohm, target1), "ch1 readback mismatch");
	zassert_equal(code2, expected_code(v->rab_ohm, target2), "ch2 readback mismatch");
	zassert_not_equal(code1, code2, "channels should read back distinct codes");
}

/* The readback API rejects invalid channels and a NULL destination. */
ZTEST(ad524x, test_read_value_invalid)
{
	const struct device *dev = variants[0].dev;
	uint8_t code;

	zassert_equal(dac_ad524x_read_value(dev, 0, &code), -EINVAL, "channel 0 should fail");
	zassert_equal(dac_ad524x_read_value(dev, 3, &code), -EINVAL, "channel 3 should fail");
	zassert_equal(dac_ad524x_read_value(dev, 1, NULL), -EINVAL, "NULL code should fail");
}

ZTEST_SUITE(ad524x, NULL, ad524x_setup, NULL, NULL, NULL);
