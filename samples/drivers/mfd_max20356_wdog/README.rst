.. zephyr:code-sample:: mfd_max20356_watchdog
   :name: MAX20356 PMIC watchdog charger reset
   :relevant-api: watchdog_interface charger_interface mfd_interfaces

   Reset the MAX20356/MAX20358 PMIC charger registers with the watchdog.

Overview
********

This sample exercises the Analog Devices MAX20356 PMIC watchdog charger/limiter
reset action (``WDCntl.WDRstType = 0b01``). On expiry this action resets the
charger and input-limiter registers to their defaults instead of resetting the
SoC.

The sample records the charger charge type and the input-limiter registers,
configures the charger (modifying ``ChgCur0.CC1IFChg`` and enabling charging via
``ChgCntl0.ChgEn``) and the limiter (``ILimCtrl1``/``ILimCtrl2``), arms the
watchdog for reset type 1, feeds it a few times to show normal servicing, then
stops feeding so the timer expires. After the reset it reads both back: the charge
type should return to its initial value and the limiter registers to their
defaults, since both the charger and the input limiter are in the reset scope.

The limiter registers reset to known defaults (``ILimCtrl1`` = 0x00; ``ILimCtrl2``
``SysDSCEn``/``SysMinVlt`` = 0), so that check is observable on the bench without
any external charge source, unlike the charge type.

The watchdog is fed with :c:func:`wdt_feed`, which reads ``Int5`` (the kick), so
each feed restarts the timer and the reset does not fire while feeding continues.

The standard watchdog API only selects soft and hard resets, so the sample arms
the watchdog with :c:func:`wdt_setup` and then overrides the reset action to the
charger/limiter-only value with ``mfd_max20356_wdt_set_rsttype()``.

The watchdog is fed by reading ``Int5``, so the reset only fires while nothing
reads ``Int5``. The ``WDTmr`` interrupt is masked on INTB by default
(``IntMask5.WDTmrM`` resets to 0) and this sample registers no INTB callbacks, so
no source asserts INTB and the parent never reads ``Int5`` on its own. Reading the
charge type (``Status0``) during the wait does not feed the timer; only an
``Int5`` read does.

It is a separate application from the :zephyr:code-sample:`mfd_max20356` PMIC
validation sample because the watchdog claims the parent's INTB path exclusively
while armed, so it cannot coexist with that sample's charger-driven INTB event
self-test.

Register-level debug
====================

The sample has an optional debug mode, enabled with ``WDT_DEMO_DEBUG`` in
``src/main.c`` (on by default). When set it reads the watchdog register
(``WDCntl``) and the charger registers (``Status0``, ``ChgCur0``, ``ChgCntl0``)
back over I2C with :c:func:`mfd_max20356_reg_read` and prints the decoded fields,
so the configuration can be verified at the register level and the charger reset
can be confirmed directly (``ChgEn`` and ``CC1IFChg`` returning to their
defaults). Set it to 0 to silence the dumps.

Building and Running
********************

The sample targets a board with the MAX20356 PMIC described in a board overlay
(see ``boards/``). Build and flash as follows:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/mfd_max20356_wdog
   :board: nucleo_u575zi_q
   :goals: build flash
   :compact:

Sample Output
=============

.. code-block:: console

   [0:00:00.123]: === MAX20356 watchdog charger-reset test ===
   [0:00:00.124]: initial charge type: none
   [0:00:00.125]: charger configured (charging enabled)
   [0:00:00.126]: limiter configured (ILimCtrl1=0x9b, ILimCtrl2=0x28)
   [0:00:00.127]:   DBG Status0   (0x01) = 0x01 (ChgStat=1)
   [0:00:00.127]:   DBG ChgCur0   (0x17) = 0x54 (CC1IFChg=84)
   [0:00:00.127]:   DBG ChgCntl0  (0x19) = 0x80 (ChgEn=1)
   [0:00:00.128]:   DBG ILimCtrl1 (0x13) = 0x9b (ILimBlank=2, ILimMax=3, ILimCntl=3)
   [0:00:00.128]:   DBG ILimCtrl2 (0x14) = 0x28 (SysDSCEn=1, SysMinVlt=8)
   [0:00:00.129]: watchdog armed (window 4000 ms, reset type 1 = charger/limiter reset); feeding 3 times
   [0:00:00.129]:   DBG WDCntl    (0x2a) = 0x04 (WDRstType=1, WDTmrSel=0)
   [0:00:02.129]:   feed 0 -> ok (0)
   [0:00:04.129]:   feed 1 -> ok (0)
   [0:00:06.129]:   feed 2 -> ok (0)
   [0:00:06.130]: feeding stopped; waiting for charger-register reset...
   [0:00:10.130]:   window 0: charge type = fast
   [0:00:14.130]:   window 1: charge type = fast
   [0:00:18.130]:   window 2: charge type = none
   [0:00:22.130]:   window 3: charge type = none
   [0:00:22.131]:   DBG Status0   (0x01) = 0x00 (ChgStat=0)
   [0:00:22.131]:   DBG ChgCur0   (0x17) = 0x00 (CC1IFChg=0)
   [0:00:22.131]:   DBG ChgCntl0  (0x19) = 0x00 (ChgEn=0)
   [0:00:22.132]:   DBG ILimCtrl1 (0x13) = 0x00 (ILimBlank=0, ILimMax=0, ILimCntl=0)
   [0:00:22.132]:   DBG ILimCtrl2 (0x14) = 0x00 (SysDSCEn=0, SysMinVlt=0)
   [0:00:22.133]: charge type after watchdog: none (initial none)
   [0:00:22.134]: limiter after watchdog: ILimCtrl1=0x00 (was 0x9b), ILimCtrl2=0x00 (was 0x28)
   [0:00:22.135]: WDT charger-reset test PASSED: charge type returned to initial
   [0:00:22.136]: limiter check PASSED: ILimCtrl1/2 reset by watchdog
