# AT32 Workbench and BSP-owned I2C

The 20Wh platform uses **I2C1 master interrupts** for SW6306, SC7A20 and
SD3078. The original Artery Technology `i2c_application.c` included slave,
DMA and SMBus paths not used by this project.

## Ownership

- `Bootstrap/src/bsp_i2c.c` owns the subset of master TX/RX, memory-register
  accesses, interrupt dispatch and transaction retry/recovery. It retains the
  original Artery 2025 copyright attribution and project bugfixes.
- `Bootstrap/inc/bsp_i2c.h` owns the I2C handle/status types directly.
  The temporary `Bootstrap/inc/i2c_application.h` compatibility shim has
  been removed because compiled sources no longer include it.
- Workbench removes `wk_i2c_app_init()` from `project/src/main.c` when its
  I2C Application option is unchecked. Therefore BSP initializes
  `hi2c1.i2cx = I2C1` **statically**, independently of generated startup code.
  The legacy init hook is optional.
- Generated `project/src/at32f423_int.c` no longer inserts the vendor I2C
  dispatcher when the option is unchecked. We explicitly call the BSP-owned
  `i2c_evt_irq_handler()` / `i2c_err_irq_handler()` in the Workbench
  **preserved user code** regions, before RTOS task notifications. The BSP
  ISR abort path closes all transaction interrupts and prevents pcount/psize
  underflow even if Workbench regenerates its vendor middleware.
- The obsolete `project/src/i2c_app.c`, `project/inc/i2c_app.h`, and
  `middlewares/i2c_application_library/i2c_application.c/.h` were removed
  from version control after migration. Workbench might regenerate them, but
  Keil must not compile the generated copies.

## When changing pins or clocks with AT32 Workbench

1. Regenerate the Workbench project normally, preserving its `add user code`
   sections. Keep `I2C1` peripheral enabled and its IRQ handlers generated.
2. From the repo root, run
   `python firmware/tools/restore_workbench_project.py`.
3. Run `python firmware/tools/restore_workbench_project.py --check`.
   The script is idempotent and restores the Keil source list **and** checks
   that I2C event/error dispatch is present inside the preserved IRQ user
   code regions.
4. Build with Keil, checking that `bsp_i2c.c` is compiled exactly once and
   `i2c_application.c`/`i2c_app.c` are not compiled. Confirm the I2C event
   and error IRQ wrappers first call their BSP state machines, then notify
   `i2c_wait_task` after status/error evaluation.
5. Test register reads, writes, bus recovery, lost-ACK and repeated resets
   on actual hardware before relying on the migrated driver.

**Important:** Workbench might overwrite its own generated
`Pocket_PowerBank.uvprojx`. Reapplying the checked-in script is required;
it does not change Workbench's peripheral settings. The trimmed Artery
code and its copyright notice remain inside `Bootstrap/src/bsp_i2c.c`;
the unused complete vendor application library has been deleted.

## Regression after disabling Workbench I2C Application (October 2026)

If SW6306/SC7A20/SD3078 all report zero or offline even though the firmware
builds, inspect these two independent requirements:

1. `hi2c1.i2cx` must point to `I2C1`. It is now initialized statically in
   `bsp_i2c.c`, not by a generated `main.c` call.
2. `I2C1_EVT_IRQHandler` must call `i2c_evt_irq_handler(&hi2c1)`, and
   `I2C1_ERR_IRQHandler` must call `i2c_err_irq_handler(&hi2c1)`.
   The FreeRTOS notification by itself does not move any I2C data.

Run the project restoration script after each Workbench regeneration; it
restores missing dispatcher calls in retained user-code regions and avoids
reintroducing the unmodified vendor library. Both PB6/PB7 pin multiplexing
and I2C1 NVIC enabling must remain configured in Workbench.
