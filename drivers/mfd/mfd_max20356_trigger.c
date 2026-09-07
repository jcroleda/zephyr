/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356/MAX20358 INTB trigger support. The INTB pin (open-drain, active-low)
 * is the device's aggregate interrupt output: any enabled Int0-5 source pulls it
 * low. A GPIO callback schedules a work item that reads the Int0-5 registers
 * (clear-on-read), classifies each pending source into an event group via a
 * single lookup table, and invokes the registered per-group callback. IntMask0-5
 * (1 = unmasked) is programmed so only groups with a registered callback assert
 * INTB.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"

LOG_MODULE_REGISTER(mfd_max20356_trig, CONFIG_MFD_LOG_LEVEL);

/* Int0-5 (0x07-0x0C) and IntMask0-5 (0x0D-0x12) are each six contiguous
 * registers; reg_index selects both via MAX20356_REG_INT0 / _INTMASK0. IntMask
 * bit positions mirror the Int bit positions, so the INT field masks index both.
 */
#define MAX20356_INT_REG_COUNT 6U

struct max20356_int_source {
	uint8_t reg_index;
	uint8_t mask;
	enum max20356_event evt;
};

/* Combined INTB source table: every Int0-5 status bit and the event group that
 * consumes it. Groups gather bits across registers; DVS/PGOOD completion
 * (MAX20356_EVT_DVS_DONE) has no Int0-5 backing and is surfaced from MPCITRSTS
 * by the regulator child, so it does not appear here.
 */
static const struct max20356_int_source max20356_int_sources[] = {
	/* Int0 (0x07) */
	{0, MAX20356_INT0_CHGSTATINT_MSK, MAX20356_EVT_CHARGER},
	{0, MAX20356_INT0_CC1TMOINT_MSK, MAX20356_EVT_CHARGER},
	{0, MAX20356_INT0_THMSTATINT_MSK, MAX20356_EVT_THERMAL},
	/* Int1 (0x08) */
	{1, MAX20356_INT1_THMSDINT_MSK, MAX20356_EVT_THERMAL},
	{1, MAX20356_INT1_CHGJEITAREGINT_MSK, MAX20356_EVT_CHARGER},
	{1, MAX20356_INT1_CHGJEITASDINT_MSK, MAX20356_EVT_CHARGER},
	{1, MAX20356_INT1_USBOKINT_MSK, MAX20356_EVT_USB},
	{1, MAX20356_INT1_USBOVPINT_MSK, MAX20356_EVT_USB},
	{1, MAX20356_INT1_ILIMINT_MSK, MAX20356_EVT_MISC},
	{1, MAX20356_INT1_CHGSYSLIMINT_MSK, MAX20356_EVT_MISC},
	{1, MAX20356_INT1_SYSBATLIMINT_MSK, MAX20356_EVT_MISC},
	/* Int2 (0x09) */
	{2, MAX20356_INT2_THMLDO1INT_MSK, MAX20356_EVT_THERMAL},
	{2, MAX20356_INT2_UVLOLDO1INT_MSK, MAX20356_EVT_REG_FAULT},
	{2, MAX20356_INT2_THMLDO2INT_MSK, MAX20356_EVT_THERMAL},
	{2, MAX20356_INT2_UVLOLDO2INT_MSK, MAX20356_EVT_REG_FAULT},
	{2, MAX20356_INT2_THMLDO3INT_MSK, MAX20356_EVT_THERMAL},
	{2, MAX20356_INT2_UVLOLDO3INT_MSK, MAX20356_EVT_REG_FAULT},
	{2, MAX20356_INT2_SCLDO3INT_MSK, MAX20356_EVT_REG_FAULT},
	{2, MAX20356_INT2_DRPLDO3INT_MSK, MAX20356_EVT_REG_FAULT},
	/* Int3 (0x0A) */
	{3, MAX20356_INT3_THMBK1INT_MSK, MAX20356_EVT_THERMAL},
	{3, MAX20356_INT3_THMBK2INT_MSK, MAX20356_EVT_THERMAL},
	{3, MAX20356_INT3_THMBK3INT_MSK, MAX20356_EVT_THERMAL},
	{3, MAX20356_INT3_LSW1TMOINT_MSK, MAX20356_EVT_REG_FAULT},
	{3, MAX20356_INT3_LSW2TMOINT_MSK, MAX20356_EVT_REG_FAULT},
	{3, MAX20356_INT3_LSW3TMOINT_MSK, MAX20356_EVT_REG_FAULT},
	{3, MAX20356_INT3_THMLSWINT_MSK, MAX20356_EVT_THERMAL},
	{3, MAX20356_INT3_BBSTFAULTINT_MSK, MAX20356_EVT_REG_FAULT},
	/* Int4 (0x0B) */
	{4, MAX20356_INT4_HRVBATCMPINT_MSK, MAX20356_EVT_MISC},
	{4, MAX20356_INT4_CHGRESTARTINT_MSK, MAX20356_EVT_CHARGER},
	{4, MAX20356_INT4_CHGVOLTMODEINT_MSK, MAX20356_EVT_CHARGER},
	{4, MAX20356_INT4_STEPCHGINT_MSK, MAX20356_EVT_MISC},
	{4, MAX20356_INT4_BATUVLOBINT_MSK, MAX20356_EVT_MISC},
	/* Int5 (0x0C) */
	{5, MAX20356_INT5_I2CTMOINT_MSK, MAX20356_EVT_MISC},
	{5, MAX20356_INT5_WDTMR_MSK, MAX20356_EVT_WATCHDOG},
};

/* Set (unmask) or clear (mask) every IntMask bit backing an event group. Each
 * Int source belongs to exactly one group, so masking a group touches only its
 * own bits. Caller holds cb_lock.
 */
static int max20356_set_group_mask(const struct device *dev, enum max20356_event evt, bool unmask)
{
	for (size_t i = 0; i < ARRAY_SIZE(max20356_int_sources); i++) {
		const struct max20356_int_source *src = &max20356_int_sources[i];
		int ret;

		if (src->evt != evt) {
			continue;
		}

		ret = mfd_max20356_reg_update(dev, MAX20356_REG_INTMASK0 + src->reg_index,
					      src->mask, unmask ? src->mask : 0U);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/* Bottom half: read the clear-on-read Int0-5 registers, classify each pending
 * source into an event group via the table, dispatch the registered per-group
 * callbacks, and re-arm INTB. Runs in workqueue or own-thread context, never in
 * the ISR.
 */
static void max20356_process_int(const struct device *dev)
{
	struct mfd_max20356_data *data = dev->data;
	const struct mfd_max20356_config *config = dev->config;
	uint8_t status[MAX20356_INT_REG_COUNT] = {0};
	uint16_t fired = 0;
	int ret;

	/* Int0-5 are clear-on-read: one read per register latches and clears all
	 * pending sources.
	 */
	for (uint8_t i = 0; i < MAX20356_INT_REG_COUNT; i++) {
		ret = mfd_max20356_reg_read(dev, MAX20356_REG_INT0 + i, &status[i]);
		if (ret < 0) {
			LOG_ERR("Int%u read failed: %d", i, ret);
			goto rearm;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(max20356_int_sources); i++) {
		const struct max20356_int_source *src = &max20356_int_sources[i];

		if ((status[src->reg_index] & src->mask) != 0U) {
			fired |= BIT(src->evt);
		}
	}

	k_mutex_lock(&data->cb_lock, K_FOREVER);
	for (uint8_t evt = 0; evt < MAX20356_EVT_MAX; evt++) {
		if ((fired & BIT(evt)) != 0U && data->cb[evt] != NULL) {
			data->cb[evt](dev, (enum max20356_event)evt, data->cb_user[evt]);
		}
	}
	k_mutex_unlock(&data->cb_lock);

rearm:
	ret = gpio_pin_interrupt_configure_dt(&config->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to re-arm INTB: %d", ret);
	}
}

static void max20356_gpio_callback(const struct device *port, struct gpio_callback *cb,
				   gpio_port_pins_t pins)
{
	struct mfd_max20356_data *data = CONTAINER_OF(cb, struct mfd_max20356_data, gpio_cb);
	const struct mfd_max20356_config *config = data->dev->config;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	/* Source decode needs I2C; defer to the bottom half and silence INTB
	 * until it has been serviced.
	 */
	(void)gpio_pin_interrupt_configure_dt(&config->int_gpio, GPIO_INT_DISABLE);

#if defined(CONFIG_MAX20356_TRIGGER_OWN_THREAD)
	k_sem_give(&data->sem);
#else
	k_work_submit(&data->work);
#endif
}

#if defined(CONFIG_MAX20356_TRIGGER_OWN_THREAD)
static void max20356_thread(void *p1, void *p2, void *p3)
{
	struct mfd_max20356_data *data = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->sem, K_FOREVER);
		max20356_process_int(data->dev);
	}
}
#else
static void max20356_work_handler(struct k_work *work)
{
	struct mfd_max20356_data *data = CONTAINER_OF(work, struct mfd_max20356_data, work);

	max20356_process_int(data->dev);
}
#endif

int mfd_max20356_add_callback(const struct device *dev, enum max20356_event evt, max20356_cb_t cb,
			      void *user)
{
	struct mfd_max20356_data *data = dev->data;
	int ret;

	if (evt >= MAX20356_EVT_MAX || cb == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->cb_lock, K_FOREVER);
	data->cb[evt] = cb;
	data->cb_user[evt] = user;
	ret = max20356_set_group_mask(dev, evt, true);
	k_mutex_unlock(&data->cb_lock);

	return ret;
}

int mfd_max20356_remove_callback(const struct device *dev, enum max20356_event evt,
				 max20356_cb_t cb)
{
	struct mfd_max20356_data *data = dev->data;
	int ret;

	if (evt >= MAX20356_EVT_MAX) {
		return -EINVAL;
	}

	k_mutex_lock(&data->cb_lock, K_FOREVER);
	if (data->cb[evt] != cb) {
		k_mutex_unlock(&data->cb_lock);
		return -EINVAL;
	}
	data->cb[evt] = NULL;
	data->cb_user[evt] = NULL;
	ret = max20356_set_group_mask(dev, evt, false);
	k_mutex_unlock(&data->cb_lock);

	return ret;
}

int mfd_max20356_trigger_init(const struct device *dev)
{
	struct mfd_max20356_data *data = dev->data;
	const struct mfd_max20356_config *config = dev->config;
	uint8_t scratch;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->cb_lock);

#if defined(CONFIG_MAX20356_TRIGGER_OWN_THREAD)
	k_sem_init(&data->sem, 0, K_SEM_MAX_LIMIT);
	k_thread_create(&data->thread, data->thread_stack,
			K_THREAD_STACK_SIZEOF(data->thread_stack), max20356_thread, data, NULL,
			NULL, K_PRIO_COOP(CONFIG_MAX20356_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "max20356_trig");
#else
	k_work_init(&data->work, max20356_work_handler);
#endif

	/* int-gpios is optional: without it the driver operates poll-only. */
	if (config->int_gpio.port == NULL) {
		LOG_INF("No int-gpios; INTB trigger disabled (poll-only)");
		return 0;
	}

	if (!gpio_is_ready_dt(&config->int_gpio)) {
		LOG_ERR("INTB GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->int_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("INTB GPIO configure failed: %d", ret);
		return ret;
	}

	gpio_init_callback(&data->gpio_cb, max20356_gpio_callback, BIT(config->int_gpio.pin));

	ret = gpio_add_callback_dt(&config->int_gpio, &data->gpio_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add INTB callback: %d", ret);
		return ret;
	}

	/* Clear any latched Int0-5 (clear-on-read) so a stale source does not fire
	 * the moment the GPIO interrupt is armed. All groups start masked
	 * (IntMask reset 0x00); add_callback() unmasks per subscription.
	 */
	for (uint8_t i = 0; i < MAX20356_INT_REG_COUNT; i++) {
		(void)mfd_max20356_reg_read(dev, MAX20356_REG_INT0 + i, &scratch);
	}

	ret = gpio_pin_interrupt_configure_dt(&config->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to arm INTB: %d", ret);
		return ret;
	}

	return 0;
}
