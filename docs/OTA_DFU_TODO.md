# Wireless OTA DFU Roadmap & TODO

This document tracks the implementation status and future roadmap for Over-The-Air (OTA) Device Firmware Updates across the wireless keyboard system (Receiver Dongle, Transmitter ProMicro, and RP2040 Host).

---

## 1. RP2040 Host Wireless OTA DFU (Status: COMPLETED — strict Etapa A port 2026-08-22)

- [x] **HID Feature Report ID 4 (`HID_REPORT_ID_DFU`)**: Vendor feature collection added to USB descriptor on Receiver Dongle.
- [x] **Bidirectional ESB Streaming**: Reverse ACK payload streaming between Receiver and Transmitter.
- [x] **SPI Bridge**: Transmitter forwards DFU control & chunk frames to RP2040 and returns status telemetry.
- [x] **Dual-Bank 4MB Flash Partitioning**: Configured on WeAct RP2040 Winbond Flash:
  - **Slot A (Active)**: `0x10000000` (2 MB capacity)
  - **Slot B (Staging)**: `0x10200000` (`FLASH_STAGING_OFFSET = 2048 KB`, 1.9 MB capacity)
  - **WKOT metadata page**: last 4 kB (`OTA_METADATA_OFFSET`), CRC-protected install record.
- [x] **Strict acknowledged session protocol (link 0x03)**: START/QUERY/CRC/DATA/FINISH/ACTIVATE/ABORT commands, each carrying a session id and command token; replies echo both. The Receiver re-queues a PC command until a DFU_STATUS proves the RP2040 consumed it end-to-end; a redelivered command replays its stored reply instead of double-applying.
- [x] **Full 32-bit CRC32 verification** (replaces the earlier 24-bit truncated compare) **plus on-device vector validation**: the staged image's stack pointer must be in SRAM and its Thumb reset handler inside the active slot before ACTIVATE is possible. Target/protocol/board lock on START, CRC and package header (`WKRPOTA1`, board `0x2040`).
- [x] **Watchdog-safe flash windows**: staging erase chunked per 4 kB sector inside `flash_safe_execute` with the watchdog fed between chunks; streaming programs one 256-byte page per window; the final slot swap runs on core 0 after resetting core 1 in the documented direction (`dfu_apply_and_reboot`, never returns).
- [x] **OTA radio discovery**: DFU traffic refreshes radio activity, and while the radio is in System OFF the RP2040 issues 30 s wake requests so `flash_ota` can start a session against a sleeping keyboard; `spi_ack_poll_task` pumps reverse ACKs at 1 ms during sessions (1 s idle, 20 ms input quiet guard, separate control sequence namespace).
- [x] **Target-locked packages & native tool**: `tools/make_ota_package.py` builds `.wkota` packages (header + payload CRC32, vector validation); `tools/flash_ota.exe` (rebuilt via `tools/build_flash_ota.ps1`) drives the strict flow and confirms the exact package CRC32 through the post-reboot BOOT_OK metadata self-report.
- [x] **RAM Flash Swap & Watchdog Reboot**: Resident `dfu_apply_and_reboot()` in RAM erases Slot A, copies verified binary from Slot B into Slot A, and resets RP2040.
- [x] **Null Movement & Zero Duplicate Fix**: Snap Tap (SOCD Last Win) and 500ms keepalive deduplication integrated.

**Matched set**: flash the Receiver from
`Receiver` repo branch `codex/rp2040-ota-strict-port` (commit `9e4164f`+,
`firmware/receiver.hex` SHA-256 `2A96E2D6...F2C09E`) together with the
RP2040 image; the Transmitter is unchanged (link protocol stays 0x03).
Hardware acceptance of the strict flow is still required.

**Deferred (Etapa B, from the strict branch)**: bootloader + relocated app
layout (`ota_bootloader.c`, `linker/ota_*`) so a power loss during ACTIVATE
is recoverable by the bootloader retrying the staging copy; two-step factory
migration for existing devices.

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
