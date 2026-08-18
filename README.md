# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the wireless keyboard build.
It reads the keyboard as a USB HID host through PIO-USB, applies local keyboard
filters, then forwards keyboard state and supported Consumer Control keys to
the nRF52840 transmitter over SPI.

## Current handoff (2026-08-18)

- Current working trees are authoritative; cloud copies are older fallbacks.
  Work on the existing `codex/*` branches and back up every file before edits.
- RP2040: `C:\Users\Monard\Raspberry\WirelessKeyboard`, branch
  `codex/fix-sonix-keyboard-toggle-resync`. The current artifact is the silent
  release image for the keyboard host-stall investigation; its Keyboard HID
  endpoint has explicit halt recovery and DATA-toggle resynchronization.
- Transmitter: `C:\ncs\v3.4.0\myproject\Transmitter`, branch
  `codex/ultra-fast-input-reliability`; the matched artifact is
  `firmware/transmitter.uf2`, SHA-256
  `6111667082C1A91297C663901D25FBFD05EC4D0FFF685633A5F95FB70726C6E9`.
- Receiver: `C:\ncs\v3.4.0\myproject\Receiver`, branch
  `codex/ultra-fast-input-reliability`; matched artifact
  `firmware/receiver.hex`, SHA-256
  `EA869379BB86AF601607D0A5783F6B7BD1D94FE1741CE54AFBCE3466906A6ACB`.
- A4Tech `VID 09DA / PID EA04` has three HID interfaces: keyboard (`inst=0`,
  EP `0x81`), mouse (`inst=1`, EP `0x82`), multimedia (`inst=2`, EP `0x83`,
  Report ID 3). `Fn+F2` was verified as Play/Pause `0x00CD`; multimedia works
  end-to-end in Windows.
- Keep `CFG_TUH_HID=4` and use REPORT from start to finish with
  `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`. Do not call
  `tuh_hid_set_protocol()` for this composite keyboard: no BOOT switch is
  issued after mounting, so the 1 kHz Keyboard interrupt-IN stream never
  competes with a runtime HID control transfer.
- `WIRELESS_KEYBOARD_UART_DIAGNOSTIC=OFF` is release mode. It is used for the
  current image, so UART tracing cannot affect the 1 kHz input path. A separate
  diagnostic build may enable bounded DAPLink traces when needed; it keeps
  `HOT_PATH_DEBUG`, `PERIODIC_DEBUG` and `CONSUMER_DEBUG` disabled.
- Link protocol is version `0x03` in all three firmware images. The fixed
  12-byte SPI/ESB frame is magic `A5`, version, type, sequence, payload[8].
  Types: Keyboard `0x01`, Consumer `0x02`, Control `0x03`, Battery `0x04`.
  Consumer contains a little-endian 16-bit usage and preserves every ordered
  press/release edge. Control `0x01` requests Transmitter System OFF; Control
  `0x02` is a low-rate reverse LED-state poll and is relayed to Receiver over
  ESB. Receiver ACK payloads use magic `0x5A`, version `0x03`, type `0x01` and
  return the latest valid Windows Num/Caps/Scroll bits plus a Receiver boot
  epoch so a reconnect can restart the 8-bit LED sequence safely.
- Num/Caps/Scroll LED state remains locally simulated on RP2040 until the first
  authoritative Windows HID Output report arrives through the Receiver. Battery
  telemetry is forwarded as a low-priority latest-state packet and cached on
  Receiver; the RGB animation and ADC sampling remain local to RP2040.
- RGB uses red `GP21`, green `GP20`, blue `GP19`; this pin-only adjustment was
  isolated in commit `ea4827d` before the low-power implementation.
- Low-power Stage 1 is implemented. RP2040 remains at 120 MHz to service the
  PIO-USB host, while nRF52840 stops released-state keepalives and enters System
  OFF after five minutes without changed HID input. CSN/P0.22 wakes it and the
  RP2040 retransmits keyboard and Consumer state after the boot guard time.
  Hardware sleep-current and wake-latency validation remains required.
- Sticky Consumer-release and modifier-order hardening is implemented across
  the matched set. RP2040 captures changed USB reports immediately, places them
  in a bounded FIFO, and schedules an exact-sequence duplicate after a 250 us
  SPIS re-arm guard without sleeping in the TinyUSB callback. Transmitter keeps
  the exact frame pending until ESB acknowledgement, including Consumer release
  frames. Receiver deduplicates Consumer packets by sequence, never purges the
  queue on a normal keyboard transition, and uses a nonblocking HID sender; this
  preserves `modifier down -> Ctrl+C -> modifier up` ordering. A legitimately
  held key remains active because its 8 ms radio keepalive refreshes the watchdog.
- Personal performance requirement: all three firmware projects must capture and
  forward input fast enough for a physical 1 kHz polling rate. No blocking HID
  callback, large retry sleep, or delayed deduplication may be used on the
  urgent Keyboard/Consumer path. Hardware acceptance tests are still required.
- Normal RP2040 UART output and all Transmitter/Receiver logging, console,
  boot-banner, UART and USB-CDC debug output are disabled in release builds.
  Receiver enumerates as HID only; its former debug COM port is intentionally
  absent.
- Battery is `1S2P`: two `3.7 V / 2000 mAh / 7.4 Wh` cells in parallel, total
  `3.7 V / 4000 mAh / 14.8 Wh`, boosted to 5 V. No-sleep estimate: 30–50 hours
  continuously (central 35–40 hours); replace with a measured 5 V current.

## TODO

- [x] Sticky Consumer Control release path: RP2040 now retains ordered
  `LINK_TYPE_CONSUMER` edges, Transmitter retries the exact sequence/data pair,
  and Receiver accepts a new sequence even when its payload repeats. The
  confirmed `Fn+F3`/`0x00B5` failure is addressed in source; physical validation
  is still required.
- [x] Rapid multimedia path: Consumer work uses the same nonblocking SPI FIFO
  and ESB-pending mechanism as keyboard input. No sleep is placed in the
  TinyUSB callback and the 250 us guard is outside the callback.
- [x] Ordered Consumer transitions: press -> release -> press remains queued;
  Receiver discards only a same-sequence retransmission after its first queue
  insertion succeeds. Queue-overrun counters remain visible in diagnostic logs.
- [x] Five-key/burst HID liveness: RP2040 still attempts to re-arm the USB HID
  interrupt-IN endpoint immediately after every report. If TinyUSB temporarily
  cannot claim or start that transfer, a per-interface pending bit retries from
  the main loop until it succeeds. The normal callback remains nonblocking and
  no permanent polling or retry delay was added.
- [x] Keyboard rollover containment: all A4Tech HID collections remain in
  REPORT mode after enumeration. The keyboard path uses its parsed Keyboard
  report, preserves simultaneous keys beyond the Boot-protocol limit, and
  treats HID `ErrorRollOver`/`POSTFail`/`ErrorUndefined` usages as a safe
  non-key state rather than forwarding them to the radio link.
- [x] Held-key host-stall recovery: Keyboard and Consumer use independent HID
  interrupt-IN endpoints. If Keyboard (`inst=0`) becomes ready after an error,
  RP2040 re-arms it even after a released state. If a pressed Keyboard transfer
  is stale, it is aborted and re-armed only when the PIO USB frame is no longer
  active; the abort path never waits or blocks Consumer (`inst=2`). The local
  A/D and W/S last-input-wins filter is disabled by default: raw simultaneous
  keys are forwarded unchanged, prioritizing reliability over that game-specific
  filter. A one-second RP2040 hardware watchdog resets a genuine main-loop hard
  hang.
- [x] SONiX Keyboard halt recovery: TinyUSB reports a failed/stalled Keyboard
  interrupt-IN completion as a zero-length callback. RP2040 recognizes this as
  transport state, not a key report, sends asynchronous standard
  `CLEAR_FEATURE(ENDPOINT_HALT)` solely to Keyboard endpoint `0x81`, resets the
  matching PIO-USB DATA toggle to DATA0 after success, then re-arms Keyboard.
  Normal 1 kHz input sends no control requests; Multimedia endpoint `0x83` is
  never reset or delayed by this recovery.
- [x] SONiX Keyboard PID resynchronization: EP `0x81` uses complete absolute
  keyboard states. When PIO-USB receives a CRC-valid DATA0/DATA1 mismatch on
  that endpoint, it accepts that one state and resynchronizes the local toggle
  instead of ACKing then discarding it. The RP2040 state comparison prevents a
  duplicate packet from becoming a duplicate key action. This is limited to
  Keyboard EP `0x81`; mouse and Multimedia endpoint semantics are unchanged.
- [ ] Hardware acceptance: validate Play/Pause, Previous, Next, Mute and
  Volume with repeated sub-10-ms actions, then verify `Ctrl+C`, `Ctrl+V`, Shift
  and Alt combinations at the 1 kHz source rate. Also hold and release 5 and 6
  ordinary keys repeatedly, then press Num Lock without reconnecting; input and
  the physical LED must continue responding. Specifically test
  `W+A+D+V+Space+Shift` held for 10 seconds, released, then type and toggle
  Num Lock. Every multimedia release must return to Consumer usage zero without
  a second press.

### Bidirectional lock-state synchronization

- [x] Reverse data path: Receiver captures keyboard HID Output reports from
  Windows through both control `SET_REPORT` and the interrupt OUT endpoint.
  Transmitter reads the ESB ACK payload and exposes it on SPI MISO
  (`GP8 <-> P0.08`); RP2040 applies it to the SONIX keyboard asynchronously.
  Receiver accepts both ID-prefixed and ID-elided control payloads, while
  RP2040 derives the real LED Output Report ID, length and bit positions from
  the SONIX HID descriptor instead of assuming the Input Report layout.
- [x] Reverse sequencing and stale-state handling: ACKs carry a sequence, a
  valid bit and a Receiver boot epoch; RP2040 ignores stale ACKs and accepts a
  fresh sequence after reconnect/wake. It retains Num Lock ON as the local
  fallback until a real Windows LED report arrives. The protocol is versioned
  as `0x03` and all three images are built as a matched set.
- [ ] Hardware acceptance: validate local lock keys, Windows software, another
  keyboard, Remote Desktop, reconnect/resume and synchronization after
  Transmitter System OFF wake-up. The low-rate poll is intentionally bounded
  and sends no radio packet until the RP2040 is awake and otherwise idle.

### Wireless battery telemetry

- [x] Firmware transport and cache: RP2040 samples locally once per second and
  schedules one latest-state Battery packet every 30 seconds after 50 ms of HID
  quiet, only while awake and after urgent Keyboard/Consumer work is idle.
  Transmitter treats it as a one-slot low-priority packet, and Receiver exposes
  the cache through vendor HID report ID `3` without waking the radio. The USB
  control response contains the required ID byte followed by eight payload
  bytes, matching the nine-byte Windows `HidD_GetFeature` buffer.
- [x] Portable Windows tray application: `tools/WirelessKeyboardTray` provides
  one native static EXE that reads only the Receiver's cached vendor Feature
  report. It uses Plug-and-Play notifications plus one coalescable 30-second
  timer, adds no radio packet, and never opens the Keyboard/Consumer HID
  collections. Real Battery values and native Windows battery presentation
  still require hardware validation after flashing the corrected Receiver.

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
| GP8 | P0.08 | SPI MISO, Transmitter ACK/status to RP2040 |
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
  normalized 16-bit usage ID; Battery payloads contain the validated integer
  telemetry record described in `docs/BATTERY_TELEMETRY_TODO.md`.
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
- RP2040 sends every changed keyboard/Consumer state into an ordered FIFO,
  schedules one exact-sequence duplicate nonblocking after a 250 us SPIS
  re-arm guard, and rearms the TinyUSB endpoint immediately. A transient
  endpoint busy/start failure sets only a pending bit; the main loop retries it
  without sleeping until TinyUSB accepts the next interrupt-IN transfer. There
  is no 10 ms reconciliation loop on the urgent path: the duplicate and the
  Transmitter's ESB-pending retry repair a lost write without delaying the next
  1 ms USB report. Keyboard restatements are deduplicated by absolute state;
  distinct Consumer sequences are retained.
- A keyboard release that misses its ESB acknowledgement remains pending in the
  Transmitter and is retried; released-state keepalive stops only after a
  successful delivery. Application-level ESB failure handling makes up to two
  additional attempts before the next pending retry.
- Receiver HID writes are nonblocking: a static report buffer is held until the
  interrupt-IN completion callback releases it. The ordered queue no longer
  purges keyboard/modifier transitions; a 250 ms link watchdog still sends an
  all-released report if pressed state is no longer refreshed. The normal 8 ms
  held-key keepalive prevents false releases during legitimate long holds.
- Num Lock, Caps Lock, and Scroll Lock start with the RP2040 Num Lock fallback,
  but Windows becomes authoritative after Receiver captures a HID Output
  report. Receiver returns the LED bits in ESB ACK payloads; Transmitter caches
  them and returns them over SPI MISO. RP2040 applies only newer valid ACKs and
  uses the low-rate Control `0x02` poll while awake and idle.
- Whenever the keyboard is mounted, the local fallback starts with Num Lock ON,
  Caps Lock OFF and Scroll Lock OFF. RP2040 schedules the corresponding HID
  Output report immediately after enumeration, then accepts the first valid
  Windows state received through the reverse path.
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
- Once every 30 seconds, if the radio is already awake and there has been at
  least 50 ms without changed HID input or urgent work, RP2040 queues one latest
  Battery record. Transmitter sends it below Keyboard/Consumer priority and
  Receiver caches it in vendor HID report ID `3`; telemetry never wakes radio
  System OFF or resets the five-minute inactivity deadline.

## Firmware

The build copies the current UF2 to:

```text
firmware/WirelessKeyboard.uf2
```

### Optional UART diagnostic build

The current `firmware/WirelessKeyboard.uf2` is the silent release build with
`-DWIRELESS_KEYBOARD_UART_DIAGNOSTIC=OFF`. If a later hardware capture is
needed, build a separate image with the option set to `ON`; it outputs UART0
at 115200 baud, 8 data bits, no parity, 1 stop bit:

| RP2040 | DAPLink UART |
| --- | --- |
| GP0 / UART0 TX | DAPLink RX |
| GND | DAPLink GND |

GP1/UART0 RX is optional and must not be connected for this capture. Open the
DAPLink virtual COM port (normally `COM25`) at `115200 8N1`, then flash that
separate diagnostic image. The trace is diagnostic only and must not be used as
the 1 kHz release.

### Release matched protocol `0x03` artifacts

The current RP2040 release artifact is
`04C9D86D201694BB84208984A7D06ED691B25C8C80F5FE377FD050D703B94B30`.
It adds universal keyboard report decoding (supporting standard 6KRO 8-byte,
9-byte Report-ID, and NKRO bitmaps from 10 to 64 bytes), eliminates rigid
8-byte length filtering, configures default BOOT protocol for boot keyboards
while keeping Consumer Control in Report protocol, and provides nonblocking
endpoint recovery. The Transmitter and Receiver artifacts remain:

- Transmitter `firmware/transmitter.uf2`:
  `551751E5353223CFDB7CF2456723514039473C2D11304410888080C6B2FAF89D`
- Receiver `firmware/receiver.hex`:
  `3E7B5B1A610147BEFC74AECE6F6D6DBC4123352EAEE6B5FC2A910B5DEE48FBFC`

The Receiver artifact above includes the Windows HID descriptor validation
fix: the eight-byte vendor Battery Input and Feature fields each declare their
complete Usage range, preventing Windows Code 10 during HID device startup.

Flash all three artifacts as one matched protocol `0x03` set. Mixing one of
these images with an older peer can restore the exact release-loss behavior
that this hardening is designed to eliminate.

For a normal board, flash either the build output or the committed UF2 artifact.

For a damaged/debug board whose external QSPI flash cannot erase or program,
`WirelessKeyboardRam` is a RAM-only target. It is diagnostic and does not survive
reset or power loss.
