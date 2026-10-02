.. zephyr:code-sample:: mfd_max20356
   :name: MAX20356 PMIC validation
   :relevant-api: charger_interface sensor_interface regulator_interface mfd_interfaces

   Exercise every rail and configuration block of the MAX20356/MAX20358 PMIC.

Overview
********

This sample brings up the Analog Devices MAX20356 wearable PMIC as a
Multi-Function Device and validates its subsystems. Unlike the generic
:zephyr:code-sample:`mfd_charger` sample, it targets the full configuration
surface of the part:

* Every regulator rail is instantiated: buck1-3, buck-boost, ldo1-4 and
  lsw1-3.
* A broad subset of the device-, charger- and regulator-level init options from
  the devicetree bindings is set in the board overlay.
* The three buck dynamic-voltage-scaling (DVS) modes are represented: buck1 and
  buck3 use I2C DVS, buck2 uses GPIO DVS.

The sample validates **that the regulator write operations succeed**, not the
resulting analog output. For each rail it calls the regulator write API
(``regulator_enable``, ``regulator_set_voltage``,
``regulator_set_current_limit``, ``regulator_set_mode`` and
``regulator_set_active_discharge``) and classifies the return code:

* ``0`` - the I2C write was accepted.
* ``-ENOTSUP`` / ``-ENOSYS`` - the operation does not apply to that rail (for
  example ``set_voltage`` on a GPIO-DVS buck or a load switch, or
  ``set_current_limit`` on an LDO). Reported as ``n/a``, not a failure.
* any other negative value - a real failure.

The actual output voltages and register contents are expected to be confirmed
with a probe on the hardware.

After the regulator pass, the sample exercises the two pin-driven DVS modes.
For buck3 (SPI DVS) it clocks a setpoint over the parent's ``spi-dvs`` controller
with ``regulator_max20356_spi_set_voltage`` and reads it back over I2C. For buck2
(GPIO DVS) it drives the MPC pin pair (``adi,dvs-mpc-pair``) through all four
select states over the MPC GPIO controller and prints the preset
(``adi,dvs-voltages``) each state selects, so a probe can confirm buck2's output
tracks the selected preset. A direct ``set_voltage`` on buck2 is rejected with
``-ENOTSUP`` because the live voltage is chosen by the pins, not a single
setpoint.

The sample then runs a fast INTB pin self-test using the
charger status interrupt, which is triggerable purely over I2C by toggling charge
enable: it installs a charger status notifier (which unmasks the charger-status
source onto INTB), toggles charging, and confirms the notifier fires. Finally it
reads the charger configuration, enables charging, and continuously polls the
charger status and the MAX17262 fuel gauge.

The MAX20356 watchdog is exercised by a separate application,
:zephyr:code-sample:`mfd_max20356_watchdog` (``samples/drivers/mfd_max20356_wdog``).
The two are mutually exclusive by design: the watchdog claims the parent's INTB
path exclusively while armed, so it cannot coexist with this sample's
charger-driven INTB event self-test.

.. note::

   On this board the MAX17262 fuel gauge is implemented as a Zephyr **sensor**
   device, so it is read through the sensor API (``SENSOR_CHAN_GAUGE_*``) rather
   than the fuel-gauge API.

Lock domains
============

Each lockable rail (bucks, buck-boost and LDOs) sets ``adi,lock-enable`` in the
overlay. This makes the driver wrap every register write in the PMIC password
unlock/write/re-lock sequence, so writes land even on a rail that powered up
OTP-locked. With the password sequence in place, a ``0`` return from the
regulator API means the register actually changed, which is what a probe
confirms. The load switches have no lock domain and take the direct-write path.

MPC-controlled rails
====================

In the overlay, lsw2 and lsw3 have their enable routed to MPC6 and MPC7
respectively (``adi,mpc-enable-rails``) and sit in sequencing slot 7. Their
enable is therefore driven by hardware, so the sample does not software-toggle
them; it only confirms they are present and ready.

Requirements
************

A board with a MAX20356 (or MAX20358) PMIC and a MAX17262 fuel gauge on an I2C
bus. The sample ships an overlay for :zephyr:board:`nucleo_u575zi_q`; adapt it
for other boards by copying it to ``boards/<board>.overlay`` and adjusting the
bus, interrupt and MPC GPIO assignments.

The devicetree must provide these node labels: ``charger``, ``fuelgauge`` and
the regulator rails (``reg_buck1`` .. ``reg_lsw3``).

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/mfd_max20356
   :board: nucleo_u575zi_q
   :goals: build flash
   :compact:

Sample Output
=============

.. code-block:: console

   [0:00:00.003]: === MAX20356 regulator validation ===
   [0:00:00.006]: buck1 (I2C DVS): validating I2C write operations
   [0:00:00.009]:   enable                 -> ok (0)
   [0:00:00.012]:   set_voltage(1000000)   -> ok (0)
   [0:00:00.015]:   set_current_limit(200000) -> ok (0)
   [0:00:00.018]:   set_active_discharge   -> ok (0)
   [0:00:00.021]:   get_voltage            -> 1000000 uV
   ...
   [0:00:00.100]: Regulator validation PASSED: all I2C writes accepted
   [0:00:00.103]: Charger "charger": reading configuration
   [0:00:00.106]:   Constant charge current: 100000 uA
   [0:00:00.109]:   Charge termination voltage: 4200000 uV
   [0:00:01.112]: Online: Charger is active.
   [0:00:01.115]: Status: Charging.
   [0:00:01.118]: Health code: 1
   [0:00:01.121]: Fuel gauge voltage: 3.950000 V
   [0:00:01.124]: Fuel gauge SoC: 75%

.. note:: Values vary with the actual device, battery state and configuration.
