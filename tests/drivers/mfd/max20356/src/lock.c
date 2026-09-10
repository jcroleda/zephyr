/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX20356 password-lock tests. The emulator enforces LockMsk1/LockUnlock1:
 * writes to a register whose lock domain is engaged are dropped on the I2C path
 * until the correct unlock password is written. These tests prove
 * mfd_max20356_reg_update_locked() performs the unmask/unlock/write/re-lock
 * sequence and that a plain update to a locked register is rejected.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/mfd/max20356.h>
#include <zephyr/sys/util.h>

#include "mfd_max20356.h"
#include "mfd_max20356_emul.h"

/* Buck1 registers are guarded by LockMsk1.Bk1Lck. */
#define LOCK_TEST_REG  MAX20356_REG_BUCK1VSET
#define LOCK_TEST_MASK MAX20356_BUCK1VSET_BUCK1VSET_MSK
#define LOCK_TEST_BIT  MAX20356_LOCKMSK1_BK1LCK_MSK

struct max20356_lock_fixture {
	const struct device *dev;
	const struct emul *emul;
};

static void *lock_setup(void)
{
	static struct max20356_lock_fixture fixture = {
		.dev = DEVICE_DT_GET(DT_NODELABEL(pmic)),
		.emul = EMUL_DT_GET(DT_NODELABEL(pmic)),
	};

	zassert_true(device_is_ready(fixture.dev), "pmic not ready");

	return &fixture;
}

static void lock_before(void *f)
{
	struct max20356_lock_fixture *fixture = f;

	mfd_max20356_emul_reset(fixture->emul);
}

ZTEST_SUITE(max20356_lock, NULL, lock_setup, lock_before, NULL, NULL);

/* A plain update to a locked register is dropped by the hardware. */
ZTEST_F(max20356_lock, test_locked_plain_write_dropped)
{
	uint8_t val;

	mfd_max20356_emul_set_locked(fixture->emul, LOCK_TEST_BIT, true);

	zassert_ok(mfd_max20356_reg_update(fixture->dev, LOCK_TEST_REG, LOCK_TEST_MASK, 0x15));
	mfd_max20356_emul_get_reg(fixture->emul, LOCK_TEST_REG, &val);
	zassert_equal(val, 0x00, "locked register accepted a plain write: 0x%02x", val);
}

/* The locked helper unlocks, writes, and re-locks a protected register. */
ZTEST_F(max20356_lock, test_locked_helper_writes)
{
	uint8_t val, unlock;

	mfd_max20356_emul_set_locked(fixture->emul, LOCK_TEST_BIT, true);

	zassert_ok(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK1, LOCK_TEST_REG,
						  LOCK_TEST_MASK, 0x15));

	mfd_max20356_emul_get_reg(fixture->emul, LOCK_TEST_REG, &val);
	zassert_equal(val, 0x15, "locked helper did not write: 0x%02x", val);

	/* The sequence ends by re-locking (last password write is 0xAA). */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK1, &unlock);
	zassert_equal(unlock, 0xAA, "domain not re-locked: 0x%02x", unlock);
}

/* The helper unmasks only the targeted domain in LockMsk1. */
ZTEST_F(max20356_lock, test_locked_helper_unmasks_single_domain)
{
	uint8_t lockmsk;

	zassert_ok(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK2,
						  MAX20356_REG_BUCK2VSET,
						  MAX20356_BUCK2VSET_BUCK2VSET_MSK, 0x0A));

	/* Only Bk2Lck is unmasked (0); every other bit stays masked (1). */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKMSK1, &lockmsk);
	zassert_equal(lockmsk, (uint8_t)~MAX20356_LOCKMSK1_BK2LCK_MSK,
		      "unexpected LockMsk1: 0x%02x", lockmsk);
}

/* After the helper re-locks, a later plain write is dropped again. */
ZTEST_F(max20356_lock, test_domain_relocked_after_helper)
{
	uint8_t val;

	zassert_ok(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK1, LOCK_TEST_REG,
						  LOCK_TEST_MASK, 0x15));

	/* Domain is locked again: a plain write must not land. */
	zassert_ok(mfd_max20356_reg_update(fixture->dev, LOCK_TEST_REG, LOCK_TEST_MASK, 0x2A));
	mfd_max20356_emul_get_reg(fixture->emul, LOCK_TEST_REG, &val);
	zassert_equal(val, 0x15, "write landed after re-lock: 0x%02x", val);
}

/* The sequencing field of a rail is guarded by its own LockMsk2 domain: the
 * matching *_SEQ helper unlocks LockUnlock2 and lands the write, while the main
 * domain's unlock leaves it dropped.
 */
ZTEST_F(max20356_lock, test_seq_lock_uses_lockmsk2)
{
	uint8_t val, lockmsk2;

	/* Lock both the main Buck1 domain and its sequencing domain. */
	mfd_max20356_emul_set_locked(fixture->emul, MAX20356_LOCKMSK1_BK1LCK_MSK, true);
	mfd_max20356_emul_set_locked2(fixture->emul, MAX20356_LOCKMSK2_BK1SEQLCK_MSK, true);

	/* A seq-field write via the *_SEQ domain unlocks LockUnlock2 and lands. */
	zassert_ok(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK1_SEQ,
						  MAX20356_REG_BUCK1ENA,
						  MAX20356_BUCK1ENA_BUCK1SEQ_MSK,
						  FIELD_PREP(MAX20356_BUCK1ENA_BUCK1SEQ_MSK, 3)));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BUCK1ENA, &val);
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1SEQ_MSK, val), 3,
		      "seq field not written: 0x%02x", val);

	/* LockUnlock2 ended re-locked (0xAA). */
	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_LOCKUNLOCK2, &lockmsk2);
	zassert_equal(lockmsk2, 0xAA, "seq domain not re-locked: 0x%02x", lockmsk2);
}

/* Unlocking a rail's main domain does not unlock its sequencing domain: a seq
 * write routed through the main domain is dropped.
 */
ZTEST_F(max20356_lock, test_main_unlock_leaves_seq_locked)
{
	uint8_t val;

	mfd_max20356_emul_set_locked2(fixture->emul, MAX20356_LOCKMSK2_BK1SEQLCK_MSK, true);

	/* Route the seq write through the MAIN Buck1 domain (wrong domain). */
	zassert_ok(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK1,
						  MAX20356_REG_BUCK1ENA,
						  MAX20356_BUCK1ENA_BUCK1SEQ_MSK,
						  FIELD_PREP(MAX20356_BUCK1ENA_BUCK1SEQ_MSK, 3)));

	mfd_max20356_emul_get_reg(fixture->emul, MAX20356_REG_BUCK1ENA, &val);
	zassert_equal(FIELD_GET(MAX20356_BUCK1ENA_BUCK1SEQ_MSK, val), 0,
		      "seq field wrongly written via main domain: 0x%02x", val);
}

/* An invalid lock domain is rejected. */
ZTEST_F(max20356_lock, test_invalid_domain)
{
	zassert_equal(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_MAX, LOCK_TEST_REG,
						     LOCK_TEST_MASK, 0x15),
		      -EINVAL);
}

/* A bus error during the sequence propagates as a negative errno. */
ZTEST_F(max20356_lock, test_bus_error_propagates)
{
	mfd_max20356_emul_set_fail(fixture->emul, true);

	zassert_true(mfd_max20356_reg_update_locked(fixture->dev, MAX20356_LOCK_BUCK1, LOCK_TEST_REG,
						    LOCK_TEST_MASK, 0x15) < 0);

	mfd_max20356_emul_set_fail(fixture->emul, false);
}
