/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 GPIO (MPC0..MPC7) child-driver tests.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

#define MPC_REG(pin) (MAX20356_REG_MPC0CFG + (pin))

struct max20356_gpio_fixture {
	const struct device *gpio;
	const struct emul *emul;
};

static void *gpio_setup(void)
{
	static struct max20356_gpio_fixture fixture = {
		.gpio = DEVICE_DT_GET(DT_NODELABEL(gpio_mpc)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
	};

	zassert_true(device_is_ready(fixture.gpio), "gpio device not ready");

	return &fixture;
}

static void gpio_before(void *f)
{
	struct max20356_gpio_fixture *fixture = f;

	mfd_max20356_emul_reset(fixture->emul);
}

ZTEST_SUITE(max20356_gpio, NULL, gpio_setup, gpio_before, NULL, NULL);

/* Configuring a pin as output enables the driver (Hi-Z-bar) and sets the level. */
ZTEST_F(max20356_gpio, test_configure_output_high)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 3, GPIO_OUTPUT_HIGH));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(3), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0HIZB_MSK) != 0, "output not enabled");
	zassert_true((reg & MAX20356_MPC0CFG_MPC0OUT_MSK) != 0, "out level not high");
}

ZTEST_F(max20356_gpio, test_configure_output_low)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 0, GPIO_OUTPUT_LOW));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(0), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0HIZB_MSK) != 0, "output not enabled");
	zassert_equal(reg & MAX20356_MPC0CFG_MPC0OUT_MSK, 0, "out level not low");
}

/* Pull-up flag maps to MPC<n>Pup. */
ZTEST_F(max20356_gpio, test_configure_input_pullup)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 5, GPIO_INPUT | GPIO_PULL_UP));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(5), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0PUP_MSK) != 0, "pull-up not set");
}

/* Open-drain output maps to MPC<n>OD. */
ZTEST_F(max20356_gpio, test_configure_open_drain)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 2, GPIO_OUTPUT | GPIO_OPEN_DRAIN));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(2), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0OD_MSK) != 0, "open-drain not set");
}

/* Unsupported flag combinations are rejected. Out-of-range pins are caught by
 * the GPIO subsystem wrapper before reaching the driver, so they are not tested
 * through the public API here.
 */
ZTEST_F(max20356_gpio, test_configure_unsupported)
{
	zassert_equal(gpio_pin_configure(fixture->gpio, 0, GPIO_INPUT | GPIO_PULL_DOWN), -ENOTSUP);
	zassert_equal(gpio_pin_configure(fixture->gpio, 1, GPIO_OUTPUT | GPIO_OPEN_SOURCE),
		      -ENOTSUP);
}

/* Setting and clearing an output pin toggles MPC<n>Out only. */
ZTEST_F(max20356_gpio, test_set_clear)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 4, GPIO_OUTPUT_LOW));

	zassert_ok(gpio_pin_set_raw(fixture->gpio, 4, 1));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(4), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0OUT_MSK) != 0, "set failed");

	zassert_ok(gpio_pin_set_raw(fixture->gpio, 4, 0));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(4), &reg);
	zassert_equal(reg & MAX20356_MPC0CFG_MPC0OUT_MSK, 0, "clear failed");
}

/* Input reads reflect the live MPC<n>Pin state seeded through the back door. */
ZTEST_F(max20356_gpio, test_input_read)
{
	zassert_ok(gpio_pin_configure(fixture->gpio, 6, GPIO_INPUT));

	mfd_max20356_emul_set_reg(fixture->emul, MPC_REG(6), MAX20356_MPC0CFG_MPC0PIN_MSK);
	zassert_equal(gpio_pin_get_raw(fixture->gpio, 6), 1, "expected pin high");

	mfd_max20356_emul_set_reg(fixture->emul, MPC_REG(6), 0);
	zassert_equal(gpio_pin_get_raw(fixture->gpio, 6), 0, "expected pin low");
}

/* Toggle flips the stored output level. */
ZTEST_F(max20356_gpio, test_toggle)
{
	uint8_t reg;

	zassert_ok(gpio_pin_configure(fixture->gpio, 7, GPIO_OUTPUT_LOW));

	zassert_ok(gpio_pin_toggle(fixture->gpio, 7));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(7), &reg);
	zassert_true((reg & MAX20356_MPC0CFG_MPC0OUT_MSK) != 0, "toggle to high failed");

	zassert_ok(gpio_pin_toggle(fixture->gpio, 7));
	mfd_max20356_emul_get_reg(fixture->emul, MPC_REG(7), &reg);
	zassert_equal(reg & MAX20356_MPC0CFG_MPC0OUT_MSK, 0, "toggle to low failed");
}

/* port_get_raw aggregates all eight live pin states. */
ZTEST_F(max20356_gpio, test_port_get_raw)
{
	gpio_port_value_t value = 0;

	mfd_max20356_emul_set_reg(fixture->emul, MPC_REG(0), MAX20356_MPC0CFG_MPC0PIN_MSK);
	mfd_max20356_emul_set_reg(fixture->emul, MPC_REG(3), MAX20356_MPC0CFG_MPC0PIN_MSK);
	mfd_max20356_emul_set_reg(fixture->emul, MPC_REG(7), MAX20356_MPC0CFG_MPC0PIN_MSK);

	zassert_ok(gpio_port_get_raw(fixture->gpio, &value));
	zassert_equal(value, BIT(0) | BIT(3) | BIT(7), "port value 0x%x", value);
}
