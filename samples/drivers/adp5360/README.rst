.. zephyr:code-sample:: adp5360
   :name: ADP5360 Multi-Function PMIC
   :relevant-api: charger_interface fuel_gauge_interface regulator_interface mfd_interfaces

   Demonstrate the ADP5360 power management IC using charger, fuel gauge, and regulator subsystems.

Overview
********

This sample demonstrates how to use the ADP5360 multi-function power management IC (PMIC)
with Zephyr's driver subsystems. The ADP5360 is a highly integrated battery management and
power delivery solution that combines a battery charger, fuel gauge, buck regulator, and
buck-boost regulator in a single device.

The sample shows how to:

* Initialize and configure the ADP5360 MFD (Multi-Function Device) driver
* Monitor battery charging status and health using the :ref:`Charger API <charger_api>`
* Read battery voltage, state of charge, and cycle count using the :ref:`Fuel Gauge API <fuel_gauge_api>`
* Control and monitor voltage regulators using the :ref:`Regulator API <regulator_api>`
* Handle interrupts, power-good signals, and reset signals from the device

The application continuously polls the charger and fuel gauge to display real-time
battery and charging information, including:

* Charger configuration (current limits, charge currents, voltages)
* Charging status (online, present, charging state, health)
* Battery fuel gauge data (voltage, state of charge, cycle count)
* Regulator output voltages

Requirements
************

The sample requires a board with an ADP5360 PMIC connected via I2C. The device must be
defined in the devicetree with the compatible string ``adi,adp5360``.

This sample has been tested on the :zephyr:board:`nucleo_u575zi_q` board with an ADP5360
evaluation board connected via I2C. An overlay file is provided for this configuration.

Hardware Setup
==============

The ADP5360 requires the following connections:

* I2C bus (SDA and SCL)
* Interrupt pin (INT) - optional
* Power-good pins (PGOOD1/PGOOD2) - optional
* Reset status pin - optional
* Manual reset pin - optional

A battery must be connected to the ADP5360 for proper operation of the charger and
fuel gauge subsystems.

Building and Running
********************

The code can be found in :zephyr_file:`samples/drivers/adp5360`.

To build and flash the application for the Nucleo U575ZI-Q board:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/adp5360
   :board: nucleo_u575zi_q
   :goals: build flash
   :compact:

For other boards, create an appropriate devicetree overlay file in the ``boards/``
subdirectory. See the existing overlay file for reference.

Sample Output
=============

.. code-block:: console

   [0:00:00.003]: Found device "adp5360_charger@46", getting charger data
   [0:00:00.006]: Current Limit: 100000 uA
   [0:00:00.009]: Current (Weak Charge): 5000 uA
   [0:00:00.012]: Current (Fast Charge): 20000 uA
   [0:00:00.015]: Adaptive Voltage: 3800000 uV
   [0:00:00.018]: Current (Termination): 5000 uA
   [0:00:03.021]: reset trigger set.
   [0:00:03.024]: Fuel gauge Battery Capacity: 1000
   [0:00:03.027]: Fuel gauge SoC Alarm: 10
   [0:00:03.030]: Buck Output Voltage: 1300000
   [0:00:03.033]: Buck Boost Output Voltage: 3300000
   [0:00:03.036]: Online: Charger is active.
   [0:00:03.039]: Present: Battery is Present.
   [0:00:03.042]: Status: Charger is Charging.
   [0:00:03.045]: Mode: Charger is in Fast Mode (CC).
   [0:00:03.048]: Health: Battery is Cool.

   [0:00:03.051]: Fuel gauge Voltage: 3950000
   [0:00:03.054]: Fuel gauge Cycle Count: 15
   [0:00:03.057]: Fuel gauge Status 0x0
   [0:00:03.060]: Fuel gauge SoC: 75

.. note:: The values shown above will vary depending on the actual battery connected
   and its state of charge.
