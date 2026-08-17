# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the wireless keyboard build.
It reads the keyboard as a USB HID host through PIO-USB, applies local keyboard
filters, then forwards keyboard state and supported Consumer Control keys to
the nRF52840 transmitter over SPI.

## Current handoff (2026-08-17)

- Current working trees are authoritative; cloud copies are older fallbacks.
  Work on the existing `codex/*` branches and back up every file before edits.
- RP2040: `C:\Users\Monard\Raspberry\WirelessKeyboard`, branch
  `codex/sticky-key-release-hardening`. Key commits: `0e0156d` (Consumer frames),
  `5410728` (four HID interfaces), `68516f4` (complete enumeration), `94a9d29`
  (release diagnostics off).
- Transmitter: `C:\ncs\v3.4.0\myproject\Transmitter`, branch
  `codex/sticky-key-release-hardening`; the matched artifact is
  `firmware/transmitter.uf2`, SHA-256
  `39752BBAB30D0BFE655A21C82A2996D9A4A71A6632ED0E6343589ACD99D89837`.
- Receiver: `C:\ncs\v3.4.0\myproject\Receiver`, branch
  `codex/sticky-key-release-hardening`; matched artifact
  `firmware/receiver.hex`, SHA-256
  `40CB154AC61EDFBE82812DEC32CECAFC70E167B54DCE1522C969A9867DFEAC94`.
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
- Sticky-key release hardening is implemented across the matched set. RP2040
  sends each changed keyboard state twice with a 1 ms SPIS re-arm guard, then
  restates its absolute state over SPI every 10 ms while the radio is awake;
  the Transmitter deduplicates these frames and retries an unacknowledged
  all-released radio report until delivery; the Receiver prioritizes the newest
  keyboard state and locally releases all keys after 250 ms without a valid
  keyboard packet. A legitimately held key remains active because its 8 ms
  radio keepalive continuously refreshes this watchdog.
- Normal RP2040 UART output and all Transmitter/Receiver logging, console,
  boot-banner, UART and USB-CDC debug output are disabled in release builds.
  Receiver enumerates as HID only; its former debug COM port is intentionally
  absent.
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
- At the exact moment this System OFF command is sent, the local RGB LED performs
  four rapid blue flashes (100 ms ON, 100 ms OFF). This temporary indication has
  priority over the battery/charging animation and confirms the RP2040 request;
  only a current measurement proves that nRF52840 physically reached System OFF.
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
- RP2040 sends every changed keyboard state twice, separated by a 1 ms SPIS
  re-arm guard, and repeats the complete state every 10 ms while awake.
  Identical restatements are discarded before radio transmission, so this
  repairs a lost write-only SPI transition without adding normal RF traffic or
  exposing the previous 250 ms recovery delay to normal typing.
- A keyboard release that misses its ESB acknowledgement remains pending in the
  Transmitter and is retried; released-state keepalive stops only after a
  successful delivery. Application-level ESB failure handling makes up to two
  additional attempts before the next pending retry.
- Receiver HID writes use a 20 ms bounded wait instead of blocking forever. Its
  queue keeps the newest absolute keyboard state ahead of stale transitions,
  and a 250 ms link watchdog sends an all-released report if pressed state is
  no longer refreshed. The normal 8 ms held-key keepalive prevents false
  releases during legitimate long holds.
- Num Lock, Caps Lock, and Scroll Lock are tracked locally by the RP2040. On
  each new physical press, RP2040 toggles the matching HID Output bit and sends
  it back to the attached USB keyboard. No lock-state data is added to the
  nRF52840 radio protocol.
- Whenever the keyboard is mounted, the local state starts with Num Lock ON,
  Caps Lock OFF and Scroll Lock OFF. RP2040 schedules the corresponding HID
  Output report immediately after enumeration, matching the target Windows
  boot state and preventing the keypad/LED from starting inverted. Because
  lock state remains intentionally local, a later change made by software or
  another keyboard can still make the physical LEDs differ from the operating
  system until the local lock key is toggled again.
- At startup the RGB LED shows battery level for five seconds: green at
  75–100%, yellow at 50–74%, orange at 25–49%, and red at 0–24%.
- Battery voltage continues to be sampled once per second in the background.
  Charging/discharging indications are event-based: the raw averaged sample
  must change by at least 50 mV at GP28 versus the immediately previous sample
  (equivalent to 150 mV at the battery through the x3 divider). A large rise
  shows the charging animation for five seconds; a large drop shows the current
  battery color for five seconds. Stable or slowly changing voltage keeps the
  LED off after the five-second boot display, so an old event cannot latch a
  continuous animation.

## Firmware

The build copies the current UF2 to:

```text
firmware/WirelessKeyboard.uf2
```

The current sticky-key-hardened RP2040 artifact has SHA-256
`1DCE3A0D0619FD13E56D45F1ECE67E822003E58779C6B5BB98FE642524C73B79`.

Flash all three artifacts as one matched protocol `0x02` set. Mixing one of
these images with an older peer can restore the exact release-loss behavior
that this hardening is designed to eliminate.

For a normal board, flash either the build output or the committed UF2 artifact.

For a damaged/debug board whose external QSPI flash cannot erase or program,
`WirelessKeyboardRam` is a RAM-only target. It is diagnostic and does not survive
reset or power loss.
