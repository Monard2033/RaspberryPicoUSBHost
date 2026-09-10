---
name: "rp2040-ota-slot-swap"
description: "Fix RP2040 OTA slot-swap bootsel entry: never call XIP-flash code while erasing slot 0; use for OTA bootsel or flash-swap crashes."
---

# RP2040 OTA Slot Swap

Repair an RP2040 in-place OTA update that erases and rewrites the active image at flash offset 0 and then reboots into BOOTSEL instead of the new firmware. Trigger phrases: "intra in bootsel dupa OTA", "OTA bootsel", "slot swap reboot", "flash swap bootsel".

## Root-cause rule

Erase or program of flash slot 0 (offset 0) destroys the running image's own XIP content. From the first erased sector until the last programmed page, **no instruction or data may be fetched from XIP flash**. Any call into a `.text` function (VMA 0x100xxxxx in the ELF map) — e.g. `watchdog_update()` — during that window crashes the core; the watchdog then resets the chip into a half-written slot whose boot2 CRC fails, and the ROM enters BOOTSEL. Verify function placement with the linker map before assuming a helper is RAM-resident.

## Procedure

1. Copy the complete staged image from XIP (staging offset) into a SRAM buffer **before** any erase. Completion check: the loop reads only from XIP and writes only to the `.bss` buffer; SRAM buffer size >= aligned image size.
2. Disable the watchdog (`watchdog_disable()`) before the first erase. The watchdog update function lives in XIP flash and cannot be called during the swap. Completion check: no `watchdog_update()` call occurs between `watchdog_disable()` and the final `watchdog_reboot()`.
3. Save and disable interrupts, then run `flash_range_erase`/`flash_range_program` in plain loops with no logging, asserts, or SDK calls that resolve to `.text`. Erase+program of ~128 KB completes in well under a second; if the image could exceed that budget, chunk the swap instead of feeding the watchdog. Completion check: the swap function is compiled into RAM (`__no_inline_not_in_flash_func`) and its VMA in the ELF map is 0x2000xxxx.
4. Restore interrupts, then re-arm the watchdog **before** rebooting: `watchdog_enable(RP2040_WATCHDOG_TIMEOUT_MS, true)` followed by `watchdog_update()`, then `watchdog_reboot(0, 0, 0)`. With the watchdog left fully disabled, `watchdog_reboot()` can wedge the board in a powered-down state — no BOOTSEL drive, no USB, no radio — until a manual power cycle (observed on WeAct RP2040 4MB). Completion check: no XIP call exists between the first erase and the reboot; grep the swap function for `watchdog_update|printf|LOG`; the watchdog is enabled immediately before the final reboot.
5. Build the release target, regenerate the OTA package, and size-check: binary payload must fit the SRAM swap buffer, and `.bss` end must stay below the stack region in the map. Completion check: clean build, package CRC printed by `make_ota_package.py`.
6. Verify end-to-end on hardware: flash the fixed image once via BOOTSEL/UF2 (the fix itself must be installed by cable), run the OTA tool over the radio, then confirm recovery — no RPI-RP2 BOOTSEL drive appears and the radio probe returns `BOOT_OK` with the new image CRC. Completion check: probe echo shows `status=0x0C` and the CRC equals the package payload CRC.

## Pitfalls

- A staged-image CRC match only proves the staging area is intact; it says nothing about the swap. Do not trust "CRC MATCH" as evidence the swap works — always confirm the post-reboot probe.
- `multicore_reset_core1()` alone does not make flash writes safe: core 0's own XIP fetches are the hazard.
- The board drive letter disappearing after UF2 copy means success (device rebooted); the same drive *appearing* after OTA means the swap corrupted slot 0.
