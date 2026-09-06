# Smart Vape ESP32-C3 fork

This branch starts at Espressif release `esp-develop-9.2.2-20260417`
(commit `40edccac415693c5130f91c01d84176ae6008566`).

Implemented changes:

- Log first accesses to known fallback/GPIO/USB Serial-JTAG stub registers.
- Implement the C3 GPIO output and output-enable latches, including their atomic
  set/clear aliases and reset values, for 32-bit accesses. The register data
  fields cover bits 0..25; this does not imply 26 bonded GPIO pads.
- Return raw source status from both interrupt-matrix status registers (TRM
  v1.4 registers 8.52/8.53). Upstream returned zero, preventing the Rust HAL
  from identifying an asserted source and causing repeated interrupt entry.

Register behavior is cross-checked against the ESP32-C3 technical reference
manual and the esp32c3 0.32.2 register definitions. QTest read/write sequences
exercise reset values, set/clear semantics, masking and independent output-enable
state from the Smart Vape Rust runner.

The awake GPIO implementation covers the 22 digital input/interrupt registers,
IO MUX registers, and simple GPIO output routing (signal 128). Output data and
enable inversion, open-drain release, and weak pulls in the simple GPIO fixture
are modeled. The shared CPU IRQ reflects all enabled pending pins. The Rust
runner tests GPIO5, pod GPIO1, charger GPIO3/10, and GPIO7 output feedback.

QTest drives named `pad` inputs (driving a pin marks it externally driven);
`release-pad` releases them. QMP uses `padN-level` on `/machine/gpio`. Read-only
`drive-level`, `drive-enable`, `drive-valid`, and `input-known` masks distinguish
low, high impedance and unknown. External digital contention is unknown. Unknown
input bits return zero with an explicit diagnostic, and cannot count as accepted
physical input. Non-GPIO peripheral routing is still unsupported. Default mux
readback follows TRM register 5.21; direct peripheral reset-pad behavior is not
inferred from that register value.

GPIO7 changes are logged with QEMU virtual timestamps; drive -2 means unknown,
-1 means high impedance, and 0/1 means a digital output. They are not a MOSFET,
gate voltage or load-current simulation. Tests do not validate synchronizer
delay, analog thresholds, filters, NMI, sleep/wakeup, contact dynamics, JTAG
ownership, pad hold or supply behavior. Unsupported modes retain diagnostics.

This is bring-up work, not a complete board emulator. Peripheral GPIO routing,
ADC, RMT, USB behavior and electrical power models remain incomplete.
An unsupported access is not a successful test. Existing upstream behavior may
also contain approximations that have not yet been audited.
