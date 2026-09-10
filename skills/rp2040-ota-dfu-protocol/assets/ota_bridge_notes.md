One-shot installer for a protocol-v2 firmware over a v1 RP2040 DFU receiver.

Usage:
  python tools/ota_bridge_v1_to_v2.py firmware/WirelessKeyboard_OTA.wkota

Key properties (keep them if modified):
- Speaks the v1 wire format: 6-byte DFU_DATA chunks, paced 0.0024 s per chunk (~2 KB/s, the stable ESB ACK throughput).
- Recovery re-sends ONLY after 2 s of frozen device-reported progress, starting exactly at the device count, so applied chunks are never re-sent.
- All status polling filters on the session id; stale BOOT_OK replays from previous installs carry the OLD metadata CRC in the value field and a different session - accepting them as a progress value corrupts the offset (observed val=0x8FEF656D garbage resend).
- Verifies the staging CRC reported by DFU_FINISH against the package CRC before sending ACTIVATE.
- Waits for BOOT_OK echoing the package CRC as final confirmation.
