# RP2040 USB HID Host Controller & SPI Master

This firmware runs on the **Raspberry Pi Pico (RP2040)** inside the custom wireless keyboard build. It acts as the central brain of the keyboard: operating a 1000 Hz software USB HID host through PIO-USB, decoding physical keyboard matrices and media keys, handling battery ADC telemetry with dual-mode CC/CV charging detection and RGB PWM visual feedback, and forwarding input packets over an 8 MHz SPI master bus to the nRF52840 Transmitter.

---

## Ecosystem & Repositories (v1.0 Stable)

This firmware is part of the 3-tier custom wireless keyboard project:
- 🧠 **[RP2040 Keyboard Controller](https://github.com/Monard2033/RaspberryPicoUSBHost)** (this repository): USB Host, Battery ADC & 8 MHz SPI Master.
- 📻 **[nRF52840 Transmitter](https://github.com/Monard2033/nRF52840-Transmitter)**: Pro Micro SPI Slave & 2.4 GHz ESB PTX (+8 dBm).
- 📡 **[nRF52840 Receiver](https://github.com/Monard2033/nRF52840-Receiver)**: USB Dongle 2.4 GHz ESB PRX & 1000 Hz USB HID Bridge.

### Primary Release Artifacts
- **Firmware Binary**: [`firmware/WirelessKeyboard.uf2`](firmware/WirelessKeyboard.uf2)
- **SHA-256 Checksum**: `B5D6648AE805D4E2FA4B50BF3EB4223CE8851FC9AAE9F0E654EB87BE12F36DDC`
- **Wireless OTA Package**: [`firmware/WirelessKeyboard_OTA.wkota`](firmware/WirelessKeyboard_OTA.wkota)

---

## Key Features & Architecture

1. **Software USB HID Host (PIO-USB @ 1000 Hz)**:
   - Utilizes RP2040 PIO state machines on Core 0 (`GP4` D+, `GP5` D-) to poll USB Full-Speed HID keyboards at 1000 Hz.
   - Decodes standard 6KRO (8-byte / 9-byte) reports and NKRO bitmap descriptors (10 to 64 bytes) with full ErrorRollOver containment.
   - Dedicated separate endpoints for Keyboard and Consumer Control to eliminate host-stall collisions during fast modifier bursts.
2. **Deterministic 8 MHz SPI Master (Link Protocol 0x03)**:
   - Dedicated 8 MHz SPI bus transmits 12-byte fixed link frames: `magic (0xA5)`, `version (0x03)`, `type (1 byte)`, `sequence (1 byte)`, `payload (8 bytes)`.
   - **`SPI_REARM_GUARD_US = 1000u` ($1.0\text{ ms}$)**: Guarantees deterministic $1000\text{ Hz}$ inter-frame pacing matching the USB standard, allowing the nRF52840 SPIS EasyDMA hardware ample time to re-arm without timing overruns or dropped keys.
3. **Calibrated Battery Telemetry & Dual-Mode CC/CV Detection**:
   - **High-Precision ADC Sampling**: Sampled at 1 Hz on `GP28` (ADC2) with 64x hardware oversampling and calibrated scale `#define BATT_ADC_SCALE_NUM 9831u` for exact 1:1 physical multimeter matching ($0.001\text{ V}$ accuracy).
   - **CC Phase Detection**: Detects constant-current charging slope ($\Delta V \ge +15\text{ mV}$) across a 16-sample rolling window.
   - **CV Phase Detection**: Detects constant-voltage top saturation ($\ge 4160\text{ mV}$) maintaining flat voltage for 16 consecutive seconds.
   - **Visual RGB LED Feedback**: Local 4-pin RGB LED on `GP21` (Red), `GP20` (Green), `GP19` (Blue) with hardware PWM for 8-second boot-up/event status indication.
4. **Intelligent Power Management & Sleep**:
   - After 5 minutes of idle time with all keys released, queues `LINK_CONTROL_SYSTEM_OFF` to place the transmitter into $0.5\ \mu A$ deep sleep, signaled by 4 blue LED pulses.
   - First physical keypress wakes the transmitter via active-low CSN pulse without losing the wake-up key stroke.
5. **Dual-Bank 4MB Wireless OTA DFU**:
   - Integrated dual-bank flash partition layout with 32-bit CRC32 verification and hardware target locks for over-the-air firmware updates via `flash_ota.exe`.

---

## Active Hardware Pinout & Wiring

### 1. USB Keyboard Matrix / Converter to RP2040

| USB Side | RP2040 Pin | Function / Description |
| :---: | :---: | :--- |
| **`D+`** | **`GP4`** | PIO-USB Software Host D+ |
| **`D-`** | **`GP5`** | PIO-USB Software Host D- |
| **`GND`** | **`GND`** | Common Ground Reference |

### 2. RP2040 Master to nRF52840 Transmitter

| RP2040 Pin | nRF52840 ProMicro Pin | Signal Name | Description |
| :---: | :---: | :---: | :--- |
| **`GP6`** | **`P0.17`** | **`SPI SCK`** | 8 MHz SPI Clock from RP2040 |
| **`GP7`** | **`P0.20`** | **`SPI MOSI`** | Serial Data from RP2040 to Transmitter |
| **`GP8`** | **`P0.08`** | **`SPI MISO`** | Reverse ACK / LED Status from Transmitter |
| **`GP9`** | **`P0.22`** | **`SPI CSN`** | Active-Low Chip Select & Hardware Wake Sense |
| **`3V3 (OUT)`** | **`VCC / 3V3`** | **`3.3V Power`** | Regulated 3.3V power rail for nRF52840 |
| **`GND`** | **`GND`** | **`Ground`** | Common Ground Reference |

> [!CAUTION]
> Never connect 5V VBUS/VSYS to the direct 3.3V/VCC pin of the nRF52840 module. Keep all ground connections common.

### 3. Battery Voltage Divider & RGB Status LED

| Component | RP2040 Pin | Configuration & Notes |
| :---: | :---: | :--- |
| **RGB Red** | **`GP21`** | PWM Red Channel (through 220–330 Ω resistor) |
| **RGB Green** | **`GP20`** | PWM Green Channel (through 220–330 Ω resistor) |
| **RGB Blue** | **`GP19`** | PWM Blue Channel (through 220–330 Ω resistor) |
| **RGB Common** | **`GND`** | Default Common-Cathode configuration (`LED_COMMON_ANODE=0`) |
| **Battery (+) Tap** | **`GP28 / ADC2`** | Voltage divider: 200 kΩ to Vbat (+), 100 kΩ to GND ($V_{meas} = V_{batt} / 3$) |
| **Battery (-)** | **`GND`** | Li-Ion Cell Ground Reference |

---

## Flashing & Programming Guide

### Flashing via BOOTSEL (USB Cable):

1. Hold down the **`BOOTSEL`** button on the RP2040 board and connect it to your PC (or press reset while holding BOOTSEL).
2. A mass storage drive named **`RPI-RP2`** will appear in Windows Explorer.
3. Drag and drop [`firmware/WirelessKeyboard.uf2`](firmware/WirelessKeyboard.uf2) onto the drive.
4. The RP2040 will flash immediately and reboot into wireless keyboard mode.

### Wireless OTA Flashing (via Receiver Dongle):

To update the RP2040 over the air without opening the keyboard enclosure:
```powershell
.\tools\flash_ota.exe firmware\WirelessKeyboard_OTA.wkota
```

---

## Critical Timing & Parameter Boundary Specifications

| Parameter | Value / Constraint | Architectural Rationale |
| :--- | :---: | :--- |
| **`SPI Rearm Guard`** | **`1000 µs`** (`SPI_REARM_GUARD_US`) | Matches native 1000 Hz USB rate; gives nRF52840 SPIS EasyDMA deterministic headroom to re-arm between burst frames. |
| **`SPI CSN Setup Time`** | **`2 µs`** (`sleep_us(2)`) | Ensures stable CSN falling edge before master clock and valid hold time before rising edge. |
| **`System Clock`** | **`96 MHz`** (or `120 MHz`) | Minimum clock required for PIO-USB 96 MHz receive sampler. |
| **`Battery ADC Scale`** | **`9831u`** (`BATT_ADC_SCALE_NUM`) | Hardware-calibrated multiplier matching physical cell voltage 1:1 down to $0.001\text{ V}$. |
| **`Battery Telemetry Period`** | **`20000 ms`** (20s) | Periodic telemetry update interval during keyboard activity without interrupting urgent input. |

---

## Windows Battery Monitoring App (`WirelessKeyboardTray`)

A dedicated lightweight Windows utility is available in [`tools/WirelessKeyboardTray/dist/`](tools/WirelessKeyboardTray/dist/):

- **Executable**: [`tools/WirelessKeyboardTray/dist/WirelessKeyboardTray.exe`](tools/WirelessKeyboardTray/dist/WirelessKeyboardTray.exe)
- **Archive package**: [`tools/WirelessKeyboardTray/dist/WirelessKeyboardTray.zip`](tools/WirelessKeyboardTray/dist/WirelessKeyboardTray.zip)

### Features & Capabilities:
- 🔋 **Live Real-Time Telemetry**: Queries the Receiver's Vendor Feature Interface to display battery percentage and cell voltage with $0.001\text{ V}$ multimeter accuracy.
- ⚡ **Charging & Power State Display**: Real-time status indicator for *Discharging*, *Charging (CC/CV)*, and *Full*.
- 🕒 **Telemetry Age & Liveness**: Monitors telemetry freshness with an instant manual *Refresh now* action.
- 🖥️ **System Tray Integration**: Native Windows notification area icon, low-battery alert popups, and *Start with Windows* autostart support.

