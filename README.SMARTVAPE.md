# Smart Vape ESP32-C3 fork

This branch starts at Espressif release `esp-develop-9.2.2-20260417`
(commit `40edccac415693c5130f91c01d84176ae6008566`).

Implemented changes:

- Log first accesses to known fallback/GPIO/USB Serial-JTAG stub registers.
- Implement the C3 GPIO output and output-enable latches, including their atomic
  set/clear aliases and reset values, for 32-bit accesses. The register data
  fields cover bits 0..25; this does not imply 26 bonded GPIO pads.

Register behavior is cross-checked against the ESP32-C3 technical reference
manual and the esp32c3 0.32.2 register definitions. QTest read/write sequences
exercise reset values, set/clear semantics, masking and independent output-enable
state from the Smart Vape Rust runner.

This is bring-up work, not a complete board emulator. GPIO pad routing,
interrupts, ADC, RMT, USB behavior and electrical power models remain incomplete.
An unsupported access is not a successful test. Existing upstream behavior may
also contain approximations that have not yet been audited.
