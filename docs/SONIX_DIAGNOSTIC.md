# Sonix EP1 diagnostic and NKRO routing record

This file records the authoritative Sonix diagnostic and hardware validation
results for the production firmware.

The diagnostic capture `build-sonix-diagnostic/daplink-nkro-verified-20261008-170035.log`
identified the Sonix FS300 routing transition:

- BOOT keyboard stream: EP `0x81`
- application keyboard stream: EP `0x83`, Report ID `1`
- shared Consumer stream: Report ID `3`
- captured application keyboard report: instance `2`, length `21`
- LED callback: `CB state=00`, then `01`, each length `1`

The host now classifies usage from the parsed descriptor for every HID
instance, strips a Report ID only when the descriptor declares it, and uses a
global latest-valid keyboard state plus delivery deduplication when the source
hands off from BOOT to the application stream. A release is handed off between
interfaces without a per-instance cache suppressing it. It does not restore an
`instance == kbd_instance` filter
and it does not guess a bitmap layout from report length.

After installing the production candidate through the normal OTA path, the
user confirmed the NumLock LED responds immediately, normal typing works, and
simultaneous multi-key input works through the previously failing rollover
case. The package has payload size 77,416 bytes and CRC32 `0xE929F70F`. This
validates the keyboard routing and LED fix, including true full NKRO at the
physical Sonix report beyond the six-key downstream budget. The radio link
still carries the existing 8-byte SPI frame, so it exposes six ordinary key
slots after normalization; that transport limitation is explicit and does not
describe the physical keyboard report.

The validated UART capture shows the keyboard stream moving from boot endpoint
`0x81` to application endpoint `0x83`, with Report ID 1 for keyboard input and
Report ID 3 for Consumer Control. Routing is based on the parsed descriptor for
every HID instance; the latest valid keyboard state is global and delivery
deduplication preserves a release during the interface handoff. Downstream
bridge normalization remains capped at six key slots.

The quiet-DFU change suppresses runtime UART logging while a DFU session is
moving pages, preventing diagnostic output from starving reverse ACK and flash
processing. The adaptive HID poller widens interrupt-IN intervals to 8 ms only
after 60 seconds without HID activity and restores the fast schedule on input.
The validated production package was installed and exercised over the normal
OTA path; this record makes no transfer-rate or timing claim beyond that result.

The release artifacts are `firmware/WirelessKeyboard_OTA.wkota` for the normal
OTA path and `firmware/WirelessKeyboard.uf2` for BOOTSEL installation.

## Diagnostic capture setup

The UART diagnostic image was built with `PICO_BOARD=pico` and
`WIRELESS_KEYBOARD_UART_DIAGNOSTIC=ON`. The SDK defaults are UART0, GP0 TX,
GP1 RX and 115200 baud. Connect DAPLink GND to RP2040 GND and DAPLink RX to
RP2040 GP0. `BOOT_OK`, its CRC and `READY` confirm the RP2040 OTA boot path;
they do not by themselves confirm physical keyboard recovery.

Connect DAPLink GND to RP2040 GND and DAPLink RX to RP2040 GP0 (UART0 TX).
DAPLink TX to RP2040 GP1 (UART0 RX) is optional because the diagnostic output
is transmit-only. Capture at 115200 baud, 8-N-1. Do not add control-transfer
probes: the image does not issue extra EP0 GET_DESCRIPTOR requests.

Test sequence:

1. Install the OTA and cold-power-cycle the keyboard.
2. Toggle NumLock and confirm the physical LED.
   NumLock SET_REPORT submission is immediate; only USB transfer completion gates the next request.
3. Press four keys, for example `f`, `j`, `h`, `g`, until the failure occurs.
4. Change the system volume afterward to exercise any consumer endpoint.

The UART prints the cached HID report descriptor once at mount, changed raw
reports up to 64 bytes, LED submit/callback evidence, and roughly 1 Hz
summaries for discovered interrupt-IN endpoints. `attempts`, `accepted`,
`NAK`, `STALL`, toggle mismatch, oversize and error counters describe host bus
activity. `last_complete=0`, `no_response` and PID `0x00`/length `-1` identify
an incomplete receive; they distinguish it from a completed device NAK.

These counters describe bus activity. UART logging can alter timing, so compare
the result with the validated production package after capture. `BOOT_OK`, CRC
and `READY` confirm the RP2040 OTA boot path; the hardware result above confirms
the physical keyboard behavior.
