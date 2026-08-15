# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the wireless keyboard build.
It reads the keyboard as a USB HID host through PIO-USB, applies local keyboard
filters, then forwards the 8-byte boot keyboard report to the nRF52840
transmitter over SPI.

## Active wiring

### USB keyboard / converter board to RP2040

| USB side | RP2040 | Purpose |
| --- | --- | --- |
| D+ | GP4 | PIO-USB host D+ |
| D- | GP5 | PIO-USB host D- |
| GND | GND | Common reference for D+/D- |

### RP2040 to nRF52840 transmitter

| RP2040 | nRF52840 transmitter | Purpose |
| --- | --- | --- |
| GP6 | P0.17 | SPI SCK |
| GP7 | P0.20 | SPI MOSI, RP2040 to nRF |
| GP8 | P0.08 | SPI MISO, reserved; unused by the current write-only protocol |
| GP9 | P0.22 | SPI CSN, driven low for each 8-byte frame |
| VSYS | VCC / RAW / VIN | nRF power from the Pico board |
| GND | GND | Common ground |

Keep all data-side grounds common: USB converter GND to RP2040 GND, and
RP2040 GND to nRF52840 GND.

### Battery monitor and 4-pin RGB LED

The default firmware setting (`LED_COMMON_ANODE=0`) is for a common-cathode
RGB LED. Use one current-limiting resistor for each color channel.

| Component | RP2040 connection | Notes |
| --- | --- | --- |
| RGB red pin | GP10 through 220–330 Ω | PWM red channel |
| RGB green pin | GP11 through 220–330 Ω | PWM green channel |
| RGB blue pin | GP12 through 220–330 Ω | PWM blue channel |
| RGB common cathode | GND | Default 4-pin LED configuration |
| Battery positive | 200 kΩ to measurement node | High side of divider |
| Measurement node | GP28 / ADC2 and 100 kΩ to GND | Battery voltage is divided by three |
| Battery negative | GND | Must share ground with RP2040 and nRF52840 |

For a common-anode LED, connect its common pin to `3V3(OUT)` and change
`LED_COMMON_ANODE` to `1` before building. Confirm the LED's actual pin order
from its datasheet; the physical R/common/G/B order is not universal.

Do not connect a Li-ion/LiPo battery directly to GP28. The documented
200 kΩ/100 kΩ divider is required to keep the ADC input below 3.3 V.

## Runtime behavior

- USB host D+ is `GP4`; PIO-USB uses `GP5` as D-.
- SPI is `spi0`, 8 MHz, mode 0, MSB first.
- The forwarded report is the standard 8-byte boot keyboard report.
- A/D and W/S use last-input-wins null movement filtering.
- Per-report UART logging is disabled by default because it breaks 1 kHz input.
- Long key holds are forwarded as state changes instead of being cut after the
  first few repeats.
- Num Lock, Caps Lock, and Scroll Lock are tracked locally by the RP2040. On
  each new physical press, RP2040 toggles the matching HID Output bit and sends
  it back to the attached USB keyboard. No lock-state data is added to the
  nRF52840 radio protocol.
- The local lock state starts with all three LEDs off whenever the keyboard is
  mounted. Because the state is intentionally local, a lock-state change made
  by software or by another keyboard on the PC can make the physical LEDs
  differ from the operating-system state until they are toggled again.
- At startup the RGB LED shows battery level for five seconds: green at
  75–100%, yellow at 50–74%, orange at 25–49%, and red at 0–24%.
- Charging is inferred locally from a sustained filtered rise in battery
  voltage. While charging below 100%, the current battery color performs two
  ON/OFF cycles in each two-second window. At 100% it remains solid green.
  After a confirmed drop of at least 1% from full, it shows the current color
  for five more seconds and then turns off.

## Firmware

The build copies the current UF2 to:

```text
firmware/WirelessKeyboard.uf2
```

For a normal board, flash either the build output or the committed UF2 artifact.

For a damaged/debug board whose external QSPI flash cannot erase or program,
`WirelessKeyboardRam` is a RAM-only target. It is diagnostic and does not survive
reset or power loss.
