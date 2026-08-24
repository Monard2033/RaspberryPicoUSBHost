#!/usr/bin/env python3
"""Create a target-locked WirelessKeyboard RP2040 OTA package."""

from __future__ import annotations

import argparse
import pathlib
import struct
import zlib


MAGIC = b"WKRPOTA1"
FORMAT_VERSION = 1
TARGET_RP2040 = 1
PROTOCOL_VERSION = 1
BOARD_WEACT_RP2040_4MB = 0x2040
HEADER_FORMAT = "<8sHBBHHIIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
VECTOR_OFFSET = 0x100
SRAM_START = 0x20000000
SRAM_END = 0x20042000
# The application is linked at the start of XIP flash (boot2 at 0, ARM
# vectors at 0x100); the Etapa A port does not relocate it to a slot.
ACTIVE_XIP_START = 0x10000000
ACTIVE_XIP_END = 0x10200000


def validate_rp2040_image(payload: bytes) -> None:
    if len(payload) < VECTOR_OFFSET + 8:
        raise ValueError("image is too short to contain RP2040 application vectors")
    stack_pointer, reset_handler = struct.unpack_from("<II", payload, VECTOR_OFFSET)
    if not SRAM_START <= stack_pointer <= SRAM_END:
        raise ValueError(f"invalid RP2040 initial stack pointer 0x{stack_pointer:08X}")
    if not reset_handler & 1:
        raise ValueError(f"reset handler 0x{reset_handler:08X} is not Thumb code")
    reset_address = reset_handler & ~1
    if not ACTIVE_XIP_START <= reset_address < ACTIVE_XIP_END:
        raise ValueError(
            f"reset handler 0x{reset_address:08X} is not linked for XIP flash"
        )


def build_header(payload: bytes) -> bytes:
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    header_without_crc = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        TARGET_RP2040,
        PROTOCOL_VERSION,
        BOARD_WEACT_RP2040_4MB,
        HEADER_SIZE,
        len(payload),
        payload_crc,
        VECTOR_OFFSET,
        0,
    )
    header_crc = zlib.crc32(header_without_crc[:-4]) & 0xFFFFFFFF
    return header_without_crc[:-4] + struct.pack("<I", header_crc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    payload = args.input.read_bytes()
    validate_rp2040_image(payload)
    header = build_header(payload)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + payload)
    print(
        f"Created {args.output}: payload={len(payload)} "
        f"crc32=0x{zlib.crc32(payload) & 0xFFFFFFFF:08X} target=RP2040"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
