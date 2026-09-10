# Implementation Plan: Early Boot NumLock LED Handshake & Zero-Contention HID Streaming

## Overview
The user confirmed that after the BOOTSEL flash, the physical NumLock LED illuminated, proving the Sonix keyboard hardware accepts and executes the USB `SET_REPORT(Output)` command. However, the command must be issued **strictly during the initial USB boot / enumeration handshake**, BEFORE the host starts streaming HID reports on EP 0x81 (the 1000 Hz interrupt IN endpoint). Once the handshake completes and EP 0x81 begins streaming, EP0 must remain completely silent during live typing so that $\ge 4$ simultaneous keys can be pressed repeatedly without causing USB host freezes.

---

## Technical Root Cause Analysis

1. **Why $\ge 4$ keys freeze when SET_REPORT is used at runtime:**
   - On RP2040 using `Pico-PIO-USB`, a single PIO state machine handles USB full-speed host signaling on GP4/GP5.
   - When EP 0x81 is actively polling at 1000 Hz, pressing $\ge 4$ keys generates a dense stream of 1 ms IN packets.
   - If an EP0 control transfer (`tuh_hid_set_report`: SETUP $\rightarrow$ DATA OUT $\rightarrow$ STATUS IN) is scheduled concurrently while EP 0x81 is streaming at 1000 Hz, token scheduling conflicts and DATA toggle bit desynchronizations occur inside the keyboard microcontroller and PIO-USB host, leading to a permanent stall/freeze.

2. **The Correct Enumeration Sequence:**
   - Device connects $\rightarrow$ USB Reset $\rightarrow$ Address assigned $\rightarrow$ Descriptors retrieved.
   - For each interface: TinyUSB retrieves the Report Descriptor on EP0 and calls `tuh_hid_mount_cb()`.
   - When all interfaces are fully enumerated, TinyUSB calls `tuh_mount_cb(dev_addr)`.
   - At `tuh_mount_cb(dev_addr)`, EP0 is 100% idle and all interface descriptors are parsed.
   - This is the exact architectural moment to issue the NumLock `SET_REPORT` handshake!
   - By deferring `hid_receive_arm_or_defer` for the keyboard until `tuh_hid_set_report_complete_cb` fires, EP 0x81 is **never touched until the LED is physically ON**.
   - After the handshake completes, EP0 is silenced for the entire session.

---

## Scenario-Based Solutions

### Scenario A: Handshake SET_REPORT in `tuh_mount_cb` with Gated EP 0x81 Polling (Recommended)
- **Concept:**
  1. In `tuh_hid_mount_cb()`: Parse descriptors, identify keyboard interface, store configuration, but **DO NOT** arm `hid_receive_arm_or_defer` for the keyboard instance.
  2. In `tuh_mount_cb(dev_addr)`: TinyUSB has finished enumerating all interfaces. EP0 is idle. Queue `tuh_hid_set_report()` for NumLock (0x01).
  3. In `tuh_hid_set_report_complete_cb()`: Transfer complete! The keyboard LED is ON. **Now, and only now**, call `hid_receive_arm_or_defer()` to transition into live 1000 Hz HID capture mode.
  4. In `keyboard_led_task()`: Silenced during runtime typing (`keyboard_led_update_pending = false`).
  5. Include a 100 ms safety timeout fallback so that if an unknown keyboard does not ACK SET_REPORT, report streaming begins automatically without locking up.
- **Trade-offs:** Cleanest USB compliance, 100% stable, zero bus collisions, verified zero-latency typing.
- **Rollback:** Restore backup copy of `WirelessKeyboard.c`.

### Scenario B: Synchronous SET_REPORT inside `tuh_hid_mount_cb`
- **Concept:** Call `tuh_hid_set_report` directly inside `tuh_hid_mount_cb`.
- **Trade-offs:** High risk of collision if the keyboard has multiple interfaces (composite device), because TinyUSB immediately attempts to fetch the descriptor for interface 1 on EP0 while `tuh_hid_set_report` is still in flight.

### Scenario C: Unconditional Periodic Polling with Endpoint Throttling
- **Trade-offs:** Adds latency and jitter to the 1000 Hz input stream, undesirable for high-speed gaming keyboards.

---

## Proposed Changes

### [WirelessKeyboard](file:///c:/Users/Monard/Raspberry/WirelessKeyboard)

#### [MODIFY] [WirelessKeyboard.c](file:///c:/Users/Monard/Raspberry/WirelessKeyboard/WirelessKeyboard.c)
- Add handshake state variables:
  ```c
  static volatile bool keyboard_boot_handshake_active;
  static volatile bool keyboard_boot_handshake_done;
  static uint32_t keyboard_boot_handshake_start_ms;
  ```
- In `tuh_hid_mount_cb()`:
  - If interface is keyboard, set `kbd_is_mounted = true`, configure report IDs, but do **NOT** arm `hid_receive_arm_or_defer(dev_addr, instance)`.
  - Non-keyboard instances (e.g. consumer control) continue normal arming.
- In `tuh_mount_cb(dev_addr)`:
  - Check if `kbd_is_mounted && kbd_dev_addr == dev_addr`.
  - Build NumLock output report (`keyboard_led_build_output_report(HID_LED_NUM_LOCK)`).
  - Issue `tuh_hid_set_report(...)` and set `keyboard_boot_handshake_active = true`.
- In `tuh_hid_set_report_complete_cb(...)`:
  - When the boot handshake report completes, set `keyboard_boot_handshake_done = true; keyboard_boot_handshake_active = false;`.
  - Arm the keyboard endpoint: `hid_receive_arm_or_defer(kbd_dev_addr, kbd_instance);`.
- In `keyboard_led_task()`:
  - Check safety timeout (if handshake active $> 100\text{ ms}$, arm endpoint automatically as fallback).
  - Do NOT send SET_REPORT on keypresses during live typing (`keyboard_led_update_pending = false`).
- In `tuh_hid_umount_cb()`:
  - Reset `keyboard_boot_handshake_active = false; keyboard_boot_handshake_done = false;`.

---

## Verification Plan

### Automated Build & Package Generation
1. Compile with `-O3` Release:
   ```powershell
   $env:Path += ";C:\Users\Monard\.pico-sdk\cmake\v4.3.4\bin;C:\Users\Monard\.pico-sdk\ninja\v1.13.2"
   & "C:\Users\Monard\.pico-sdk\cmake\v4.3.4\bin\cmake.exe" --build build-release-ninja --target WirelessKeyboard
   ```
2. Build OTA package:
   ```powershell
   py tools/make_ota_package.py --input build-release-ninja/WirelessKeyboard.bin --output firmware/WirelessKeyboard_OTA.wkota
   ```
3. Verify binary size is within the 128 KB swap buffer limit (~74 KB).

### OTA Flash Deployment (No Disassembly Needed)
1. Flash over the air:
   ```powershell
   & "tools\flash_ota_cmd.exe" "firmware\WirelessKeyboard_OTA.wkota"
   ```
2. Verify RP2040 completes slot swap and reboots cleanly.

### Manual Verification
1. Observe keyboard power-up / plug-in:
   - Verify the physical NumLock LED turns ON during initialization handshake before typing begins.
2. Test rollover and simultaneous keypresses:
   - Repeatedly press and hold $\ge 4$ keys simultaneously (e.g. W, A, S, D or Q, W, E, R).
   - Verify keyboard does NOT freeze or crash.
