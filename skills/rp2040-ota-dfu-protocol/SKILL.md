---
name: "rp2040-ota-dfu-protocol"
description: "Fix RP2040 wireless OTA staging corruption and false CRC success; use for OTA ERR_CRC/ERR_TARGET failures or chunk-loss on the ESB radio path."
---

# RP2040 Wireless OTA DFU Protocol Reliability

Fix radio-path OTA failures on the WirelessKeyboard project: staging CRC mismatch at DFU_FINISH (ERR_CRC / ERR_TARGET), false "Potrivire 100%" success, and lost/duplicated DFU_DATA chunks on the ESB radio path.

## Root causes (confirmed live 2026-09-10)

- The dongle stamps every host feature-report command with a fresh SPI token, so the RP2040's token-based replay guard never catches a redelivered DFU_DATA chunk. Any PC-side recovery that re-sends already-applied chunks (e.g. after a 200 ms ACK lag) double-appends 6-byte chunks and corrupts staging with a different CRC every run.
- The dongle drains exactly one ESB ACK payload per radio packet: sustained throughput is ~2 KB/s. Faster feature-report writes overflow the receiver ACK queue and lose frames. 2 KB/s is the operator-confirmed stable limit — do not raise it.
- Stale PyInstaller exes (tools/flash_ota*.exe, Flash_Ota.exe) reported "Potrivire 100%" without comparing the device staging CRC against the package CRC. The .py sources have the real check; always rebuild exes after editing the .py sources (PyInstaller is available in the bundled .pico-sdk python).

## Protocol v2 (implemented, live-validated)

- DFU_DATA chunks carry 5 payload bytes + a rolling sequence byte in data[7]. The RP2040 accepts only the exact next sequence (mod 256, chunk index + 1); duplicates and out-of-order chunks are silently dropped with a status reply carrying the current accepted count. This makes PC re-sends idempotent regardless of dongle tokens.
- PC tools derive the sequence from the byte offset (5 bytes per chunk): `seq = ((offset // 5) + 1) & 0xFF`. A recovery re-send starting at the device-reported count therefore automatically carries the sequences the device expects next.
- Recovery policy: re-send only after 2 s of zero ACK progress (far longer than any ACK drain or flash erase), starting exactly at the device-reported count. Never re-send during normal lag.
- A protocol change cannot self-install: a v2 tool against a v1 device consumes the sequence byte as payload and fails ERR_TARGET (vectors invalid). Migrate once with tools/ota_bridge_v1_to_v2.py — it speaks v1 (6-byte chunks) at 2 KB/s, filters stale status frames by session id, recovers after 2 s frozen ACKs from the device count, verifies the staging CRC before ACTIVATE, and waits for BOOT_OK with the package CRC.

## Failure signatures

- ERR_CRC at FINISH with a different value every run → staging corruption from double-append (fixed by v2; if seen again, check the [RESEND] log lines: device accepted bytes vs PC offset).
- ERR_TARGET detail=1 → staged vectors invalid: either a v1 device received v2-format chunks, or chunks were lost mid-stream.
- ERR_SESSION mid-transfer → the RP2040 rebooted; look for a core-0 block longer than the watchdog (2 s).
- GUI/exe claims success but CRC differs → stale exe, rebuild it; verify the device-reported value against the package CRC by hand.

## Verification recipe

1. Bridge (or v2 tool) install: expect `Staging verified OK` with the exact package CRC, then `BOOT_OK` echo carrying the same CRC after the slot swap.
2. Confirm no BOOTSEL drive appears after ACTIVATE (see the slot-swap skill for that failure class).
3. probe_ota_link.exe QUERY must echo BOOT_OK with the installed image CRC.
