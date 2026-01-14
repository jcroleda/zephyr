/*
 * Copyright (c) 2026 Analog Devices Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

#include "ltc4286.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(LTC4286, CONFIG_SENSOR_LOG_LEVEL);

static void ltc4286_handle_trigger(const struct device *dev)
{
	const struct ltc4286_dev_config *cfg = dev->config;
	struct ltc4286_data *dev_data = dev->data;
	bool overpower_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_OP);
	bool overcurrent_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_OC);
	bool comp_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_COMP);
	bool fault_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_FET);
	bool power_good_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_PWR_GD);
	bool alert_triggered =
		atomic_test_and_clear_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_ALERT);

	if (overpower_triggered && dev_data->overpower_trig.trig.handler != NULL) {
		dev_data->overpower_trig.trig.handler(dev, &dev_data->overpower_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->overpower_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}

	if (overcurrent_triggered && dev_data->overcurrent_trig.trig.handler != NULL) {
		dev_data->overcurrent_trig.trig.handler(dev,
							&dev_data->overcurrent_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->overcurrent_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}

	if (comp_triggered && dev_data->comp_out_trig.trig.handler != NULL) {
		dev_data->comp_out_trig.trig.handler(dev, &dev_data->comp_out_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->comp_out_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}

	if (fault_triggered && dev_data->fault_out_trig.trig.handler != NULL) {
		dev_data->fault_out_trig.trig.handler(dev, &dev_data->fault_out_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->fault_out_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}

	if (power_good_triggered && dev_data->power_good_trig.trig.handler != NULL) {
		dev_data->power_good_trig.trig.handler(dev,
						       &dev_data->power_good_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->power_good_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}

	if (alert_triggered && dev_data->alert_trig.trig.handler != NULL) {
		dev_data->alert_trig.trig.handler(dev, &dev_data->alert_trig.trig.trigger);
		gpio_pin_interrupt_configure_dt(&cfg->alert_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}
}

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
static void ltc4286_thread_cb(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ltc4286_data *dev_data = p1;

	while (true) {
		k_sem_take(&dev_data->gpio_sem, K_FOREVER);
		ltc4286_handle_trigger(dev_data->dev);
	}
}
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
static void ltc4286_work_cb(struct k_work *work)
{
	struct ltc4286_data *dev_data = CONTAINER_OF(work, struct ltc4286_data, work);

	ltc4286_handle_trigger(dev_data->dev);
}
#endif

static void ltc4286_gpio_trig_op_cb(const struct device *dev, struct gpio_callback *cb,
				    uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->overpower_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_OP);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static void ltc4286_gpio_trig_oc_cb(const struct device *dev, struct gpio_callback *cb,
				    uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->overcurrent_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_OC);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static void ltc4286_gpio_trig_comp_cb(const struct device *dev, struct gpio_callback *cb,
				      uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->comp_out_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_COMP);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static void ltc4286_gpio_trig_fault_cb(const struct device *dev, struct gpio_callback *cb,
				       uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->fault_out_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_FET);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static void ltc4286_gpio_trig_pwr_good_cb(const struct device *dev, struct gpio_callback *cb,
					  uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->power_good_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_PWR_GD);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static void ltc4286_gpio_trig_alert_cb(const struct device *dev, struct gpio_callback *cb,
				       uint32_t pins)
{
	struct ltc4286_data *dev_data = dev->data;
	const struct ltc4286_dev_config *cfg = dev->config;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->alert_gpio, GPIO_INT_DISABLE);

	atomic_set_bit(&dev_data->trig_flags, LTC4286_TRIG_IDX_ALERT);

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&dev_data->work);
#endif
}

static int ltc4286_init_trig_pin(struct ltc4286_gpio_trig *gpio_int, const struct gpio_dt_spec *pin,
				 gpio_callback_handler_t gpio_handler)
{
	int ret;

	if (!pin || !pin->port) {
		return 0;
	}

	if (!gpio_is_ready_dt(pin)) {
		LOG_DBG("%s trigger pin not ready", pin->port->name);
		return -ENODEV;
	}

	gpio_init_callback(&gpio_int->gpio_cb, gpio_handler, BIT(pin->pin));

	ret = gpio_pin_interrupt_configure_dt(pin, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		return ret;
	}

	ret = gpio_add_callback(pin->port, &gpio_int->gpio_cb);
	if (ret) {
		return ret;
	}

	gpio_int->is_enabled = true;

	return 0;
}

int ltc4286_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler)
{
	const struct ltc4286_dev_config *cfg = dev->config;
	struct ltc4286_data *dev_data = dev->data;
	const struct gpio_dt_spec *gpio = NULL;
	struct ltc4286_trig *trig_data = NULL;

	if (trig == NULL || trig->type != SENSOR_TRIG_THRESHOLD) {
		return -EINVAL;
	}

	/* Determine which GPIO and trigger structure to use */
	switch ((int)trig->chan) {
	case SENSOR_CHAN_CURRENT:
		gpio = &cfg->overcurrent_gpio;
		trig_data = &dev_data->overcurrent_trig.trig;
		break;
	case SENSOR_CHAN_POWER:
		gpio = &cfg->overpower_gpio;
		trig_data = &dev_data->overpower_trig.trig;
		break;
	case SENSOR_CHAN_LTC4286_STATUS:
		gpio = &cfg->fault_out_gpio;
		trig_data = &dev_data->fault_out_trig.trig;
		break;
	case SENSOR_CHAN_LTC4286_POWER_GOOD:
		gpio = &cfg->power_good_gpio;
		trig_data = &dev_data->power_good_trig.trig;
		break;
	case SENSOR_CHAN_LTC4286_COMPARATOR:
		gpio = &cfg->comp_out_gpio;
		trig_data = &dev_data->comp_out_trig.trig;
		break;
	case SENSOR_CHAN_LTC4286_ALERT:
		gpio = &cfg->alert_gpio;
		trig_data = &dev_data->alert_trig.trig;
		break;
	default:
		return -ENOTSUP;
	}

	/* Disable interrupt during configuration to avoid race conditions */
	if (gpio && gpio->port) {
		gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_DISABLE);
	}

	/* Update handler and trigger (copy by value) */
	if (handler != NULL) {
		trig_data->handler = handler;
		trig_data->trigger = *trig; /* Copy by value to avoid use-after-free */

		/* Re-enable interrupt */
		if (gpio && gpio->port) {
			gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_EDGE_TO_ACTIVE);

			/* Check if GPIO is already active (ADT7310 pattern) */
			if (gpio_pin_get_dt(gpio) > 0) {
				/* Manually trigger work/semaphore if pin is already asserted */
#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
				k_sem_give(&dev_data->gpio_sem);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
				k_work_submit(&dev_data->work);
#endif
			}
		}
	} else {
		/* NULL handler means disable this trigger */
		trig_data->handler = NULL;
	}

	return 0;
}

int ltc4286_init_interrupts(const struct device *dev)
{
	const struct ltc4286_dev_config *cfg = dev->config;
	struct ltc4286_data *dev_data = dev->data;
	int ret;

#if defined(CONFIG_LTC4286_TRIGGER_OWN_THREAD)
	k_sem_init(&dev_data->gpio_sem, 0, 1);
	k_thread_create(&dev_data->thread, dev_data->thread_stack, CONFIG_LTC4286_THREAD_STACK_SIZE,
			(k_thread_entry_t)ltc4286_thread_cb, dev_data, NULL, NULL,
			K_PRIO_COOP(CONFIG_LTC4286_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&dev_data->thread, dev->name);
#elif defined(CONFIG_LTC4286_TRIGGER_GLOBAL_THREAD)
	k_work_init(&dev_data->work, ltc4286_work_cb);
#endif

	ret = ltc4286_init_trig_pin(&dev_data->overpower_trig, &cfg->overpower_gpio,
				    ltc4286_gpio_trig_op_cb);
	if (ret) {
		goto cleanup;
	}

	ret = ltc4286_init_trig_pin(&dev_data->overcurrent_trig, &cfg->overcurrent_gpio,
				    ltc4286_gpio_trig_oc_cb);
	if (ret) {
		goto cleanup_overpower;
	}

	ret = ltc4286_init_trig_pin(&dev_data->comp_out_trig, &cfg->comp_out_gpio,
				    ltc4286_gpio_trig_comp_cb);
	if (ret) {
		goto cleanup_overcurrent;
	}

	ret = ltc4286_init_trig_pin(&dev_data->fault_out_trig, &cfg->fault_out_gpio,
				    ltc4286_gpio_trig_fault_cb);
	if (ret) {
		goto cleanup_comp;
	}

	ret = ltc4286_init_trig_pin(&dev_data->power_good_trig, &cfg->power_good_gpio,
				    ltc4286_gpio_trig_pwr_good_cb);
	if (ret) {
		goto cleanup_fault;
	}

	ret = ltc4286_init_trig_pin(&dev_data->alert_trig, &cfg->alert_gpio,
				    ltc4286_gpio_trig_alert_cb);
	if (ret) {
		goto cleanup_power_good;
	}

	return 0;

cleanup_power_good:
	if (dev_data->power_good_trig.is_enabled && cfg->power_good_gpio.port) {
		gpio_pin_interrupt_configure_dt(&cfg->power_good_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->power_good_gpio.port, &dev_data->power_good_trig.gpio_cb);
	}
cleanup_fault:
	if (dev_data->fault_out_trig.is_enabled && cfg->fault_out_gpio.port) {
		gpio_pin_interrupt_configure_dt(&cfg->fault_out_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->fault_out_gpio.port, &dev_data->fault_out_trig.gpio_cb);
	}
cleanup_comp:
	if (dev_data->comp_out_trig.is_enabled && cfg->comp_out_gpio.port) {
		gpio_pin_interrupt_configure_dt(&cfg->comp_out_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->comp_out_gpio.port, &dev_data->comp_out_trig.gpio_cb);
	}
cleanup_overcurrent:
	if (dev_data->overcurrent_trig.is_enabled && cfg->overcurrent_gpio.port) {
		gpio_pin_interrupt_configure_dt(&cfg->overcurrent_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->overcurrent_gpio.port,
				     &dev_data->overcurrent_trig.gpio_cb);
	}
cleanup_overpower:
	if (dev_data->overpower_trig.is_enabled && cfg->overpower_gpio.port) {
		gpio_pin_interrupt_configure_dt(&cfg->overpower_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->overpower_gpio.port, &dev_data->overpower_trig.gpio_cb);
	}
cleanup:
	return ret;
}

int ltc4286_enable_alert_mask(const struct device *dev, enum ltc4286_trig_alert_mask alert)
{
	uint8_t reg_data[] = {alert, 0};

	return ltc4286_cmd_write(dev, LTC4286_CMD_ALERT_MASK, reg_data, 2);
}

int ltc4286_disable_alert_mask(const struct device *dev, enum ltc4286_trig_alert_mask alert)
{
	uint8_t reg_data[] = {alert, 0xFF};

	return ltc4286_cmd_write(dev, LTC4286_CMD_ALERT_MASK, reg_data, 2);
}
