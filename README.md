# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the custom wireless keyboard build. It operates as a high-speed USB HID host through PIO-USB, handles physical debouncing, local modifier management and battery monitoring, and forwards keyboard and Consumer Control reports over a dedicated 8 MHz SPI bus to the nRF52840 transmitter for 2.4 GHz Enhanced ShockBurst (ESB) wireless transmission.

## Current handoff (2026-08-25 — v1.0 / v1.1 Stable Release)

- **Branch**: `release/v1.0-stable` across all three repositories ([RP2040 PR #4](https://github.com/Monard2033/RaspberryPicoUSBHost/pull/4), [Transmitter PR #4](https://github.com/Monard2033/nRF52840-Transmitter/pull/4), [Receiver PR #4](https://github.com/Monard2033/nRF52840-Receiver/pull/4)).
- **RP2040**: `C:\Users\Monard\Raspberry\WirelessKeyboard`, artifact `firmware/WirelessKeyboard.uf2`, SHA-256 `4E448A4F2AF2BC20EF9DC40B0314A746F5A5B48C7DDA2BFADE2F0B4CB9EC6FBB`.
- **Transmitter**: `C:\ncs\v3.4.0\myproject\Transmitter`, artifact `firmware/transmitter.uf2`, SHA-256 `52996D63E5B6F7BC4225576D4B5427EA804444F583E5B19AC592C0DDAD9C7295`.
- **Receiver**: `C:\ncs\v3.4.0\myproject\Receiver`, artifact `firmware/receiver.hex`, SHA-256 `9B31317B46A8D0FA8A70F2776F1FD2BA121299516E809B693494B02FA0C30903`.
- **A4Tech Composite Compatibility**: VID `0x09DA` / PID `0xEA04` exposes three HID interfaces: keyboard (`inst=0`, EP `0x81`), mouse (`inst=1`, EP `0x82`), multimedia (`inst=2`, EP `0x83`, Report ID 3).
- **USB Host Engine**: `CFG_TUH_HID=4` in REPORT mode with `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`. No runtime BOOT switch control transfers compete with the 1 kHz interrupt-IN stream.
- **Link Protocol 0x03**: Fixed 12-byte SPI and ESB frame format: `magic (0xA5)`, `version (0x03)`, `type (1 byte)`, `sequence (1 byte)`, `payload (8 bytes)`. Types: Keyboard `0x01`, Consumer `0x02`, Control `0x03`, Battery `0x04`. Reverse ACKs use `magic (0x5A)` carrying the latest Windows Num/Caps/Scroll lock state.
- **Receiver 5-in-1 HID Architecture**: The nRF52840 Dongle receiver enumerates composite USB with 5 distinct collections (customizable via `prj.conf` / Windows registry):
  1. `A4TECH USB Receiver` — 1000 Hz Standard Keyboard Interface (Report ID 1)
  2. `A4TECH Consumer Media Control` — Multimedia Keys Interface (Report ID 2)
  3. `A4TECH Battery & Power Management` — Battery Telemetry Vendor Interface (Report ID 3)
  4. `A4TECH Wireless Configuration Interface` — OTA DFU Session Controller (Report ID 4)
  5. `A4TECH 1000Hz Ultra-Fast Diagnostic Interface` — Diagnostic Trace Ring Buffer (Report ID 5)

---

## Critical Timing & Parameter Boundary Specifications

The following hardware and driver bounds **must be strictly preserved** to maintain zero-loss communication and sub-millisecond input latency:

| Parameter | Value / Constraint | Rationale & Architectural Rule |
| --- | --- | --- |
| **`ESB Retransmit Delay`** | **`450 µs`** (Lower bound $\ge \mathbf{435\ \mu s}$) | **CRITICAL SDK RULE**: Nordic ESB driver (`nrf/subsys/esb/esb.c`) enforces `#define RETRANSMIT_DELAY_MIN 435`. Setting `retransmit_delay < 435` causes `esb_init()` to fail with `-EINVAL (-22)`, completely disabling the radio! `450 µs` is the verified optimum. |
| **`ESB Hardware Retransmits`** | **`6`** (count) | Allows hardware-level fast retries in 2.4 GHz ISM noise before application backoff. |
| **`ESB Radio Thread Queue`** | **`while(esb_send_once(&frame) != 0)`** | **Zero Dropped Keys Guarantee**: In a keyboard bridge, frames (especially key *Release* frames) must **never** be dropped after arbitrary retry counts. Dropping release frames causes stuck repeating keys. Retrying until ACK ensures 100% reliable state. |
| **`SPI Rearm & Retry Guard`** | **`150 µs` / `100 µs`** (`SPI_MIN_GUARD_US` / `SPI_RETRY_GUARD_US`) | Adaptive MISO-based SPI transport: detects reverse-ACK byte `0x5A` immediately. If EasyDMA is not armed (`0x00`), RP2040 automatically triggers fast retries up to 8 times with a 100 µs guard, achieving sub-millisecond latency with zero dropped frames. |
| **`SPI CSN Setup Time`** | **`2 µs`** (`sleep_us(2)`) | Ensures stable CSN falling edge before master clocks and valid hold time before CSN rising edge. |
| **`RF Frequency Channel`** | **`Canal 90 (2490 MHz)`** | Upper bound for PCB antenna resonance. Lies +17 MHz above Wi-Fi Channel 11 (2473 MHz) and +10 MHz above Bluetooth (2480 MHz). Channels $\ge 95$ (2495 MHz) suffer severe VSWR reflection and attenuation on PCB trace antennas. |
| **`System Clock`** | **`96 MHz`** (Lower bound for PIO-USB) | Lowest clock supporting PIO-USB 96 MHz FS receive sampler. Lowering clock below 96 MHz corrupts USB packet capture. |
| **`Battery ADC Divider`** | **`200 kΩ / 100 kΩ`** ($3\times$) | Divides max 4.35V battery down to $< 1.45\text{ V}$, well below the RP2040 3.3V ADC reference. |
| **`Battery History Window`**| **`16` samples** (16 seconds) | Rolling 16-sample history buffer filter at 1 Hz for dual-mode CC/CV charge detection. |

---

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
| GP6 | P0.17 | SPI SCK (8 MHz) |
| GP7 | P0.20 | SPI MOSI, RP2040 to nRF |
| GP8 | P0.08 | SPI MISO, Transmitter reverse ACK/status to RP2040 |
| GP9 | P0.22 | SPI CSN, driven low for each 12-byte typed frame |
| VSYS | RAW / VIN only | Use only when the nRF board has an onboard input regulator |
| 3V3(OUT) | 3V3 / VCC direct supply | Use for a direct 3.3 V nRF52840 supply pin |
| GND | GND | Common ground |

> [!CAUTION]
> Keep all grounds common. Never connect a 5 V VSYS/VBUS rail to a direct nRF52840 `3V3` or `VCC` pin.

### Battery monitor and 4-pin RGB LED

The default firmware setting (`LED_COMMON_ANODE=0`) is for a common-cathode RGB LED.

| Component | RP2040 connection | Notes |
| --- | --- | --- |
| RGB red pin | GP21 through 220–330 Ω | PWM red channel |
| RGB green pin | GP20 through 220–330 Ω | PWM green channel |
| RGB blue pin | GP19 through 220–330 Ω | PWM blue channel |
| RGB common cathode | GND | Default 4-pin common cathode configuration |
| Battery positive | 200 kΩ to measurement node | High side of divider connected to Li-ion cell |
| Measurement node | GP28 / ADC2 and 100 kΩ to GND | Divided voltage ($V_{batt} / 3$) |
| Battery negative | GND | Common ground reference |

---

## Runtime behavior

- **High-Speed Input Processing**: USB host D+ on `GP4` and D- on `GP5` are polled via PIO-USB at 1 kHz. Changed HID states are immediately pushed into a non-blocking queue.
- **SPI Frame Transmission**: The master clocks 12-byte frames at 8 MHz. `SPI_MIN_GUARD_US = 150u` enforces a physical minimum inter-frame guard, and `spi_write_frame()` inspects the slave's MISO ACK response (`0x5A`) to deterministically retry unacknowledged frames up to 8 times (`SPI_RETRY_GUARD_US = 100u`), guaranteeing zero loss without blind delay penalties.
- **Guaranteed ESB Radio Delivery**: The transmitter sends 12-byte packets with 6 hardware retries (`retransmit_delay = 450 µs`, `retransmit_count = 6`) on Channel 90 (2490 MHz). The transmission thread maintains `while(esb_send_once(&frame) != 0)` to guarantee zero dropped keystrokes and zero stuck keys.
- **Bidirectional Lock LEDs**: Windows Num/Caps/Scroll lock state is returned in ESB ACK payloads from Receiver to Transmitter and transferred over SPI MISO (`GP8`) on every transfer. Lock states are automatically applied to the physical keyboard.
- **Dual-Mode CC/CV Battery Telemetry**:
  - **Measurement**: Sampled once per second on GP28 (64x hardware oversampling) with an exponential IIR filter and 16-sample rolling buffer.
  - **CC Phase Detection**: Detected when slope $\Delta V \ge +15\text{ mV}$ (`BATT_CC_DELTA_MV`) over rolling window.
  - **CV Phase Detection**: Detected when voltage reaches saturation $\ge 4160\text{ mV}$ (`BATT_CV_TOP_MV`) and stays flat without dropping for 16 consecutive seconds.
  - **Power State Reporting**: Reports State `0`/`2` (*Discharging*) when operating on battery, State `1` (*Charging*) during active charge, and State `3` (*Full*) upon CV completion.
  - **8-Second LED Indication**: Boot-up and plug/unplug events show battery percentage / charging animation for 8 seconds (`BATT_BOOT_SHOW_MS = 8000`), allowing full ADC filter stabilization before going dark.
- **Power Management & Inactivity**:
  - After 5 minutes without changed HID input, RP2040 sends Control type `0x03` command `0x01` to put the nRF52840 transmitter into ultra-low-power System OFF.
  - Local RGB LED indicates sleep entry with 4 rapid blue flashes (100 ms ON, 100 ms OFF).
  - First keypress wakes the transmitter via CSN line without data loss.

---

## Firmware artifacts and their roles

| File | Role |
| --- | --- |
| `firmware/WirelessKeyboard.uf2` | Current RP2040 BOOTSEL cable image (flash or revert from any state) |
| `firmware/WirelessKeyboard_OTA.bin` | Raw flash payload used by `tools/make_ota_package.py` |
| `firmware/WirelessKeyboard_OTA.wkota` | Strict OTA package consumed by `tools/flash_ota.exe` (auto-built) |
| `firmware/transmitter.uf2` | nRF52840 Transmitter ProMicro firmware (UF2 format for bootloader) |
| `firmware/receiver.hex` | nRF52840 Receiver USB Dongle firmware (Intel HEX format) |
| `tools/WirelessKeyboardTray/dist/WirelessKeyboardTray.exe` | Native Windows system tray telemetry monitor |

---

## TODO & Implementation Status

- [x] ~~Sticky Consumer Control release path: RP2040 retains ordered `LINK_TYPE_CONSUMER` edges, Transmitter retries exact sequence/data pair, and Receiver accepts new sequences even when payload repeats.~~ *(Resolved and validated in Link Protocol 0x03)*
- [x] ~~Rapid multimedia path: Consumer work uses nonblocking SPI FIFO and ESB-pending mechanism with zero sleep in TinyUSB callbacks.~~ *(Fully implemented)*
- [x] ~~Ordered Consumer transitions: press -> release -> press remains strictly ordered without queue purging.~~ *(Fully implemented)*
- [x] ~~Five-key/burst HID liveness: USB HID interrupt-IN endpoint re-arms immediately after each report with main-loop retry bit.~~ *(Fully implemented)*
- [x] ~~Keyboard rollover containment: Preserves simultaneous keys beyond Boot-protocol limit and handles ErrorRollOver safely.~~ *(Fully implemented)*
- [x] ~~Held-key host-stall recovery: Independent endpoints for Keyboard and Consumer with watchdog-backed stall recovery.~~ *(Fully implemented)*
- [x] ~~SONiX Keyboard halt recovery & PID resynchronization: Asynchronous CLEAR_FEATURE on endpoint halt with automatic DATA toggle resync.~~ *(Fully implemented)*
- [x] ~~Hardware acceptance: validate 1000 Hz source rate, rapid modifier combos, held keys (`W+A+D+V+Space+Shift`), and Num Lock toggle.~~ *(Completely validated on physical hardware with zero stuck keys and zero loss)*
- [x] ~~Bidirectional lock-state synchronization: Windows LED state captured by Receiver, relayed via ESB ACK and SPI MISO to RP2040.~~ *(Fully operational with epoch tracking)*
- [x] ~~Wireless battery telemetry with dual-mode CC/CV detection: Rolling 16-sample history buffer detecting CC slope ($\ge +15\text{ mV}$) and CV top saturation ($\ge 4160\text{ mV}$).~~ *(Implemented and reporting Discharging / Charging / Full states)*
- [x] ~~High-speed SPI / ESB zero-loss transport: Upgraded to adaptive MISO-based SPI retry protocol (150 µs min guard, 100 µs retry guard, max 8 retries) and guaranteed ESB retry loop (`while`).~~ *(Deterministic zero-loss transport with sub-millisecond latency)*
- [x] ~~RP2040 Host Wireless OTA DFU: Dual-Bank 4MB Flash partitioning, 32-bit CRC32 verification, target-locked `.wkota` packages and native C++ `flash_ota.exe` tool.~~ *(Fully functional on Link Protocol 0x03)*
- [ ] **nRF52840 Transmitter Wireless OTA DFU via Receiver**: Research and implement dual-target DFU to flash Transmitter module over ESB link from Receiver Dongle.
