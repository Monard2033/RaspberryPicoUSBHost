#!/usr/bin/env python3
"""One-shot OTA bridge: old 6-byte protocol, 2 KB/s paced, safe recovery.

Installs a protocol-v2 firmware (chunk-sequence DFU handler) over a device
that still runs the v1 receiver. Re-sends only after 2 s of frozen ACK
progress, exactly from the device count, and verifies the staging CRC
before ACTIVATE.
"""
import ctypes
import struct
import sys
import time
import zlib

sys.path.insert(0, r"C:\Users\Monard\Raspberry\WirelessKeyboard\tools")
from flash_ota import (  # noqa: E402
    open_receiver, set_feature, get_status, load_package,
    DFU_CMD_START, DFU_CMD_CRC, DFU_CMD_DATA, DFU_CMD_FINISH,
    DFU_CMD_ACTIVATE, DFU_STATUS_OK, DFU_STATUS_BUSY, DFU_STATUS_VERIFIED,
    DFU_STATUS_APPLYING, DFU_STATUS_BOOT_OK,
    OTA_TARGET_RP2040, OTA_PROTOCOL_VERSION, OTA_BOARD_WEACT_RP2040_4MB,
)

CHUNK = 6
PAGE = 256


def main():
    pkg = sys.argv[1]
    payload, expected_crc = load_package(pkg)
    total = len(payload)
    print(f"Package: {total} bytes CRC32=0x{expected_crc:08X}")

    handle = open_receiver()
    base_status, base_session, _, _, _ = get_status(handle)
    session = (int(time.time() * 1000) ^ expected_crc ^ total) & 0xFF or 1
    if session == base_session:
        session = (session + 1) & 0xFF or 1
    print(f"Session {session}")

    start = bytes([DFU_CMD_START, session, OTA_TARGET_RP2040,
                   OTA_PROTOCOL_VERSION, total & 0xFF, (total >> 8) & 0xFF,
                   (total >> 16) & 0xFF, (total >> 24) & 0xFF])
    while not set_feature(handle, start):
        time.sleep(0.002)
    time.sleep(0.3)

    crc_cmd = bytes([DFU_CMD_CRC, session, expected_crc & 0xFF,
                     (expected_crc >> 8) & 0xFF, (expected_crc >> 16) & 0xFF,
                     (expected_crc >> 24) & 0xFF,
                     OTA_BOARD_WEACT_RP2040_4MB & 0xFF,
                     (OTA_BOARD_WEACT_RP2040_4MB >> 8) & 0xFF])
    while not set_feature(handle, crc_cmd):
        time.sleep(0.002)
    time.sleep(0.3)

    offset = 0
    t0 = time.time()
    last_progress = time.time()
    last_val = -1
    while offset < total:
        target = min((offset // PAGE + 1) * PAGE, total)
        if offset % 4096 == 0 and offset > 0:
            time.sleep(0.040)
        while offset < target:
            chunk = payload[offset:offset + CHUNK]
            cmd = bytes([DFU_CMD_DATA, session]) + chunk + bytes(CHUNK - len(chunk))
            while not set_feature(handle, cmd):
                time.sleep(0.0002)
            time.sleep(0.0024)
            offset += len(chunk)

        while time.time() - t0 < 600:
            st = get_status(handle)
            if st is not None:
                status, s_session, s_token, s_detail, val = st
                if s_session != session:
                    # Stale frames from previous sessions (e.g. BOOT_OK
                    # replays) carry a different session id: ignore them.
                    time.sleep(0.0005)
                    continue
                if val >= target:
                    break
                if val != last_val:
                    last_val = val
                    last_progress = time.time()
                if time.time() - last_progress > 2.0:
                    last_progress = time.time()
                    print(f"[RESEND] frozen 2s at device {val} B; re-sending {val}..{target}")
                    p = val
                    while p < target:
                        c = payload[p:p + CHUNK]
                        cmd = bytes([DFU_CMD_DATA, session]) + c + bytes(CHUNK - len(c))
                        while not set_feature(handle, cmd):
                            time.sleep(0.0002)
                        time.sleep(0.0024)
                        p += len(c)
                    offset = p
                    break
            time.sleep(0.0005)
        else:
            raise TimeoutError("page commit timeout")
        pct = offset * 100 // total
        sys.stdout.write(f"\r{pct:3d}% {offset}/{total}")
        sys.stdout.flush()

    print("\nVerifying staging...")
    fin = bytes([DFU_CMD_FINISH, session, expected_crc & 0xFF,
                 (expected_crc >> 8) & 0xFF, (expected_crc >> 16) & 0xFF,
                 (expected_crc >> 24) & 0xFF, 0, 0])
    set_feature(handle, fin)
    deadline = time.time() + 20
    while time.time() < deadline:
        st = get_status(handle)
        if st is not None:
            status, s_session, s_token, s_detail, val = st
            if s_session == session:
                if status == DFU_STATUS_VERIFIED and val == expected_crc:
                    print("Staging verified OK")
                    break
                if status not in (DFU_STATUS_OK, DFU_STATUS_BUSY):
                    raise RuntimeError(f"verify failed: status={status} value=0x{val:08X}")
        time.sleep(0.01)
    else:
        raise TimeoutError("verify timeout")

    act = bytes([DFU_CMD_ACTIVATE, session, expected_crc & 0xFF,
                 (expected_crc >> 8) & 0xFF, (expected_crc >> 16) & 0xFF,
                 (expected_crc >> 24) & 0xFF, 0, 0])
    set_feature(handle, act)
    print("ACTIVATE sent; waiting for reboot + BOOT_OK...")
    deadline = time.time() + 30
    while time.time() < deadline:
        st = get_status(handle)
        if st is not None:
            status, s_session, s_token, s_detail, val = st
            if s_session != session:
                time.sleep(0.01)
                continue
            if status == DFU_STATUS_APPLYING:
                continue
            if status == DFU_STATUS_BOOT_OK and val == expected_crc:
                print(f"BOOT_OK CRC=0x{val:08X} — firmware v2 active!")
                return 0
        time.sleep(0.1)
    print("BOOT_OK not caught (RP2040 rebooted; verify with probe).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
