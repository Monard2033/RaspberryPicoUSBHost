# Wireless OTA DFU Roadmap & TODO

This document tracks the implementation status and future roadmap for Over-The-Air (OTA) Device Firmware Updates across the wireless keyboard system (Receiver Dongle, Transmitter ProMicro, and RP2040 Host).

---

## 1. RP2040 Host Wireless OTA DFU (Status: COMPLETED & VERIFIED)

- [x] **HID Feature Report ID 4 (`HID_REPORT_ID_DFU`)**: Vendor feature collection added to USB descriptor on Receiver Dongle.
- [x] **Bidirectional ESB Streaming**: Reverse ACK payload streaming between Receiver and Transmitter.
- [x] **SPI Bridge**: Transmitter forwards DFU control & chunk frames to RP2040 and returns status telemetry.
- [x] **Dual-Bank 4MB Flash Partitioning**: Configured on WeAct RP2040 Winbond Flash:
  - **Slot A (Active)**: `0x10000000` (2 MB capacity)
  - **Slot B (Staging)**: `0x10200000` (`FLASH_STAGING_OFFSET = 2048 KB`, 2 MB capacity)
- [x] **Checksum Verification**: Hardware CRC32 verification over entire downloaded image in Slot B.
- [x] **RAM Flash Swap & Watchdog Reboot**: Resident `dfu_apply_and_reboot()` in RAM erases Slot A, copies verified binary from Slot B into Slot A, and resets RP2040.
- [x] **Native Windows Flashing Utility**: `tools/flash_ota.exe` (compiled C++, zero dependencies) with real-time progress bar.
- [x] **Null Movement & Zero Duplicate Fix**: Snap Tap (SOCD Last Win) and 500ms keepalive deduplication integrated.

---

## 2. nRF52840 Transmitter Wireless OTA DFU via Receiver (Status: PLANNED / TODO)

### Objective
Enable complete over-the-air firmware upgrades for the **nRF52840 Transmitter** module wirelessly through the **nRF52840 Receiver Dongle**, eliminating the need to connect a physical USB cable to the Transmitter ProMicro.

### Tasks & Technical Investigation
- [ ] **Flash Partitioning & Bootloader Compatibility**:
  - Investigate nRF52840 1MB internal flash layout under Zephyr / NCS (v3.4.0).
  - Evaluate **MCUboot Dual-Bank** (`slot0_partition` active app vs `slot1_partition` staging slot) vs **Custom RAM ESB DFU Flash Writer**.
  - Check coexistence with the factory **Adafruit nRF52 UF2 Bootloader** currently present on the ProMicro board.
- [ ] **DFU Target Routing Protocol**:
  - Extend `DFU_CMD_START` packet header with a `target_device` field:
    - `0x01`: Target RP2040 Host (forward to SPI).
    - `0x02`: Target nRF52840 Transmitter (consume locally in Transmitter flash staging).
- [ ] **Transmitter Internal Flash Writer**:
  - Implement flash page erase (`flash_erase()`) and programming (`flash_write()`) inside `Transmitter/src/main.c`.
  - Disable ESB radio / interrupts during page programming to prevent memory bus contention.
- [ ] **Image Validation & Failsafe Boot**:
  - Compute CRC32 / SHA-256 over received Transmitter image.
  - Implement MCUboot test-mode or direct flash swap with Watchdog fallback in case of corrupted image.
- [ ] **Windows Flashing Tool (`tools/flash_ota.exe`) Integration**:
  - Add CLI target selection flags:
    ```powershell
    # Flash RP2040 Host:
    .\tools\flash_ota.exe --target rp2040 firmware\WirelessKeyboard_OTA.bin

    # Flash nRF52840 Transmitter:
    .\tools\flash_ota.exe --target transmitter firmware\transmitter_OTA.bin
    ```
