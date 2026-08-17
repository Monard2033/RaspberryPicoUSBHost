# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the wireless keyboard build.
It reads the keyboard as a USB HID host through PIO-USB, applies local keyboard
filters, then forwards keyboard state and supported Consumer Control keys to
the nRF52840 transmitter over SPI.

## Current handoff (2026-08-17)

- Current working trees are authoritative; cloud copies are older fallbacks.
  Work on the existing `codex/*` branches and back up every file before edits.
- RP2040: `C:\Users\Monard\Raspberry\WirelessKeyboard`, branch
  `codex/low-power-stage-1`. Key commits: `0e0156d` (Consumer frames),
  `5410728` (four HID interfaces), `68516f4` (complete enumeration), `94a9d29`
  (release diagnostics off).
- Transmitter: `C:\ncs\v3.4.0\myproject\Transmitter`, branch
  `codex/low-power-stage-1`; the Stage 1 artifact is
  `firmware/transmitter.uf2`, SHA-256
  `E80C6D7D730C566358B7CDF54086B8B0F8393249072E9AA90320318E33FD7931`.
- Receiver: `C:\ncs\v3.4.0\myproject\Receiver`, branch
  `codex/wireless-keyboard-receiver`, commit `5bdbfa1`; artifact
  `firmware/receiver.hex`, SHA-256
  `427C9E14587067CB422F2ABF32F872D733A81840BF3E8A3C9D12FF4052B1C3E5`.
- A4Tech `VID 09DA / PID EA04` has three HID interfaces: keyboard (`inst=0`,
  EP `0x81`), mouse (`inst=1`, EP `0x82`), multimedia (`inst=2`, EP `0x83`,
  Report ID 3). `Fn+F2` was verified as Play/Pause `0x00CD`; multimedia works
  end-to-end in Windows.
- Keep `CFG_TUH_HID=4`. Select REPORT before `tuh_init()` with
  `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`. Do not call
  `tuh_hid_set_protocol()` from the first mount callback; it interrupts
  enumeration before `inst=1/2`.
- `CONSUMER_DEBUG=0` is release mode; temporarily set `1` for changed raw HID
  reports and descriptor dumps over DAPLink UART (`COM25`).
- Link protocol is version `0x02` in all three firmware images. The fixed
  12-byte SPI/ESB frame is magic `A5`, version, type, sequence, payload[8].
  Types: Keyboard `0x01`, Consumer `0x02`, local Control `0x03`; Consumer
  contains a little-endian 16-bit usage and preserves press/release. Control
  command `0x01` requests Transmitter System OFF and is never relayed by ESB.
- Num/Caps/Scroll LED feedback and battery/RGB behavior stay local to RP2040;
  neither lock state nor battery data is sent by radio.
- RGB uses red `GP21`, green `GP20`, blue `GP19`; this pin-only adjustment was
  isolated in commit `ea4827d` before the low-power implementation.
- Low-power Stage 1 is implemented. RP2040 remains at 120 MHz to service the
  PIO-USB host, while nRF52840 stops released-state keepalives and enters System
  OFF after five minutes without changed HID input. CSN/P0.22 wakes it and the
  RP2040 retransmits keyboard and Consumer state after the boot guard time.
  Hardware sleep-current and wake-latency validation remains required.
- Battery is `1S2P`: two `3.7 V / 2000 mAh / 7.4 Wh` cells in parallel, total
  `3.7 V / 4000 mAh / 14.8 Wh`, boosted to 5 V. No-sleep estimate: 30–50 hours
  continuously (central 35–40 hours); replace with a measured 5 V current.

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
| GP9 | P0.22 | SPI CSN, driven low for each 12-byte typed frame |
| VSYS | RAW / VIN only | Use only when the nRF board has an onboard input regulator |
| 3V3(OUT) | 3V3 / VCC direct supply | Use for a direct 3.3 V nRF52840 supply pin |
| GND | GND | Common ground |

Keep all data-side grounds common: USB converter GND to RP2040 GND, and
RP2040 GND to nRF52840 GND.

Never connect a 5 V VSYS/VBUS rail to a direct nRF52840 `3V3` or `VCC` pin.
Choose exactly one nRF power row above according to the transmitter board's
schematic; do not connect both power inputs at the same time.

### Battery monitor and 4-pin RGB LED

The default firmware setting (`LED_COMMON_ANODE=0`) is for a common-cathode
RGB LED. Use one current-limiting resistor for each color channel.

| Component | RP2040 connection | Notes |
| --- | --- | --- |
| RGB red pin | GP21 through 220–330 Ω | PWM red; current working tree |
| RGB green pin | GP20 through 220–330 Ω | PWM green; current working tree |
| RGB blue pin | GP19 through 220–330 Ω | PWM blue; current working tree |
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
- The SPI/radio frame is 12 bytes: magic, protocol version, input type,
  sequence number, and an 8-byte payload. Keyboard payloads contain the
  standard boot-keyboard report; Consumer Control payloads contain a
  normalized 16-bit usage ID.
- Next, Previous, Mute, Play/Pause, Volume Down, and Volume Up are detected from
  HID Consumer Control reports and forwarded to the PC through the nRF52840
  receiver. Press and release transitions are preserved, while radio
  keepalives repeat only the latest non-released keyboard state.
- Every HID interface contributes to the inactivity timer when its report
  content changes; identical USB polling reports do not keep the radio awake.
- After five inactive minutes, sleep is requested only when the normal keyboard
  report is all released, Consumer Usage is zero, and no local lock-LED control
  transfer is active. The RP2040 sends Control type `0x03`, command `0x01`.
- The Transmitter consumes the control frame locally, disables ESB, arms
  P0.22/CSN for LOW sense and enters nRF52840 System OFF. On the next HID change,
  RP2040 pulses CSN without clocks, waits for the nRF boot, then retransmits the
  queued press/release transitions followed by the complete keyboard and
  Consumer state. No additional wake wire is required, and a short tap during
  the boot guard interval is not reduced to its final released state.
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

The current Stage 1 RP2040 artifact has SHA-256
`BBCBA37B3F4BBFE74A7A9BF7CE568B1EB841E057E13EDE17B6DD3DFDA2B2BD7A`.

For a normal board, flash either the build output or the committed UF2 artifact.

For a damaged/debug board whose external QSPI flash cannot erase or program,
`WirelessKeyboardRam` is a RAM-only target. It is diagnostic and does not survive
reset or power loss.
