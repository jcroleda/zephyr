/*
 * Copyright (c) 2026 Analog Devices Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_MFD_ADP5360_H_
#define ZEPHYR_DRIVERS_MFD_ADP5360_H_

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/adp5360.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define ADP5360_MFD_REG_PGOOD_STATUS       0x2Fu
#define ADP5360_MFD_REG_INT_STATUS1        0x34u
#define ADP5360_MFD_REG_INT_STATUS2        0x35u

#define ADP5360_STATUS_MANUAL_RESET_INT_MASK     BIT(7)

/* INT Pin Callbacks */
struct mfd_adp5360_interrupt_handler {
	struct child_interrupt_callback soc_low_callback;
	struct child_interrupt_callback soc_acm_callback;
	struct child_interrupt_callback adpichg_callback;
	struct child_interrupt_callback bat_protection_callback;
	struct child_interrupt_callback temp_threshold_callback;
	struct child_interrupt_callback bat_voltage_threshold_callback;
	struct child_interrupt_callback charger_mode_change_callback;
	struct child_interrupt_callback vbus_voltage_threshold_callback;
	struct child_interrupt_callback manual_reset_callback;
	struct child_interrupt_callback watchdog_timeout_callback;
	struct child_interrupt_callback buckpgood_callback;
	struct child_interrupt_callback buckbstpgood_callback;
};

/* PGOOD1 and PGOOD2 Pin Callbacks */
struct mfd_adp5360_pgood_handler {
	struct child_interrupt_callback manual_reset_pressed_callback;
	struct child_interrupt_callback charge_complete_callback;
	struct child_interrupt_callback vbus_ok_callback;
	struct child_interrupt_callback battery_ok_callback;
	struct child_interrupt_callback vout2_ok_callback;
	struct child_interrupt_callback vout1_ok_callback;
};

struct mfd_adp5360_data {
	struct mfd_adp5360_interrupt_handler interrupt_handlers;
	struct mfd_adp5360_pgood_handler pgood_handlers;
	struct child_interrupt_callback reset_status_handler;
	struct gpio_callback int_callback;
	struct gpio_callback pgood1_callback;
	struct gpio_callback pgood2_callback;
	struct gpio_callback reset_status_callback;

	const struct device *dev;
	/* user data for interrupt handler */
#if defined(CONFIG_ADP5360_TRIGGER_OWN_THREAD)
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_ADP5360_THREAD_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(pgood1_thread_stack, CONFIG_ADP5360_THREAD_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(pgood2_thread_stack, CONFIG_ADP5360_THREAD_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(reset_thread_stack, CONFIG_ADP5360_THREAD_STACK_SIZE);
	struct k_thread thread;
	struct k_sem sem;
	struct k_thread pgood1_thread;
	struct k_thread pgood2_thread;
	struct k_thread reset_thread;
	struct k_sem pgood1_sem;
	struct k_sem pgood2_sem;
	struct k_sem reset_status_sem;
#elif defined(CONFIG_ADP5360_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
	struct k_work pgood1_work;
	struct k_work pgood2_work;
	struct k_work reset_status_work;
#endif
};

struct mfd_adp5360_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec interrupt_gpio;
	struct gpio_dt_spec manual_reset_gpio;
	struct gpio_dt_spec reset_status_gpio;
	struct gpio_dt_spec charger_en_gpio;
	struct gpio_dt_spec pgood1_gpio;
	struct gpio_dt_spec pgood2_gpio;
	const uint8_t *int_sources;
	uint8_t int_sources_size;
	uint8_t watchdog_time;
	bool en_vout1_rst;
	bool en_vout2_rst;
	bool reset_time_1p6s;
	bool en_watchdog;
	bool en_mr_shipment;
};

enum adp5360_fg_mode {
	ADP5360_FG_MODE_SLEEP = 0,
	ADP5360_FG_MODE_ACTIVE,
};

#if defined(CONFIG_ADP5360_TRIGGER)

int mfd_adp5360_enable_interrupt_mask(const struct device *dev, enum adp5360_interrupt_type type);

int mfd_adp5360_interrupt_trigger_set(const struct device *dev, enum adp5360_interrupt_type type,
				      mfd_adp5360_interrupt_callback handler, void *user_data);

int mfd_adp5360_init_interrupt(const struct device *dev);

int mfd_adp5360_init_pgood_interrupt(const struct device *dev);

int mfd_adp5360_init_reset_status_interrupt(const struct device *dev);

#endif /* CONFIG_ADP5360_TRIGGER */

#endif /* ZEPHYR_DRIVERS_MFD_ADP5360_H_ */
