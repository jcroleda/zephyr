.. zephyr:code-sample:: ltc4286
   :name: LTC4286 High Power Hot-Swap Controller and Power Monitor
   :relevant-api: sensor_interface

   Get telemetry data and alerts (V_OUT, V_IN, I_OUT, P_IN, Temperature)
   from the device via polling.

Description
***********

This sample application periodically measures telemetry sensor data
(V_OUT, V_IN, I_OUT, P_IN, Temperature) from the device.
The result is written to the console upon each fetch.

When configured in trigger mode, the controller can receive interrupts from the
device's GPIO pins, corresponding to different states and user-configurable
threshold warnings for various telemetry measurements.
Handlers can be configured for each interrupt and programmed accordingly.

References
**********

 - LTC4286: https://www.analog.com/ltc4286

Wiring
*******

This sample uses the LTC4286 sensor controlled using the I2C interface as PMBus.
Connect External Supply: **VDD**, **GND**,
a current-sense resistor between **SENSE+**, **SENSE-**,
and connect a separate controller Interface: **SDA**, **SCL**, **DVCC**, **GND**
For interrupt and reboot, connect the controller to **INTVCC**, **GND**, **GPIO1-8**
The supply voltage can be in the 8.5V to 80V range.
Depending on the baseboard used, the **SDA** and **SCL** lines require Pull-Up
resistors.

Building and Running
********************

This project outputs sensor data to the console. It requires an LTC4286
sensor. It should work with any platform featuring a I2C peripheral interface.
It does not work on QEMU.
In this example below the :zephyr:board:`nucleo_u575zi_q` board is used.


.. zephyr-app-commands::
   :zephyr-app: samples/sensor/ltc4286
   :board: st/nucleo_u575zi_q
   :goals: build flash

Sample Output
=============

.. code-block:: console

   *** Booting Zephyr OS build zephyr-v2.1.0-538-g12b2ed2cf7c3  ***
   device is 0x2000101c, name is LTC4286
   [0:0:10.490]: temperature - 17.000085 Cel
   [0:0:10.494]: voltage out - 10.050000 V
   [0:0:10.497]: voltage in - 9.525000 V
   [0:0:10.501]: current out - 0.100000 A
   [0:0:10.504]: power in - 0.000000 W
   [0:0:10.507]: alert flags - 0x0941

<repeats endlessly>
