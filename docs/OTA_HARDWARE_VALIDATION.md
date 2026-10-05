# OTA hardware validation

Validated on RP2040 hardware on 2026-10-05 with the current firmware and
flasher artifacts.

- OTA payload: 74,120 bytes
- Payload CRC32: `0xED0CD8FB`
- GUI stream limit: 2,000 firmware bytes/s
- Observed effective transfer speed: approximately 1,031.6 B/s with the
  configured ceiling at 2,000 firmware bytes/s. Effective rate depends on
  transport overhead, HID writes, acknowledgements, and recovery waits.
- Recovery: successful OTA completion, exact CRC verification, `BOOT_OK`, and
  immediate keyboard operation after reboot without a power cycle
- Retry coverage: two no-ACK recovery events were observed; the transfer
  resumed successfully at offsets 56,145 and 73,984

The restart path runs from SRAM, keeps interrupts masked through reset, and
uses direct RP2040 watchdog/PSM MMIO after Slot 0 is erased. Keep that helper
and its IRQ-masked reset invariant unchanged unless a new hardware validation
cycle is performed.

The current power-management behavior and continuous PIO USB SOF handling are
included in the RP2040 source used for this validation.
