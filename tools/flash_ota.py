#!/usr/bin/env python3
"""
Wireless OTA DFU Flasher for RP2040 Keyboard via nRF52840 Dongle
Usage:
    python tools/flash_ota.py [firmware_file.bin]
"""

import sys
import os
import time
import struct
import zlib
import ctypes
from ctypes import wintypes

# HID Constants
HID_REPORT_ID_DFU = 0x04
DFU_CMD_START  = 0x10
DFU_CMD_DATA   = 0x11
DFU_CMD_FINISH = 0x12
DFU_CMD_STATUS = 0x13

DFU_STATUS_IDLE     = 0x00
DFU_STATUS_BUSY     = 0x01
DFU_STATUS_OK       = 0x02
DFU_STATUS_ERR_SIZE = 0x03
DFU_STATUS_ERR_CRC  = 0x04
DFU_STATUS_ERR_FLASH= 0x05
DFU_STATUS_SUCCESS  = 0x06

# Windows HID SetupAPI structures
DIGCF_PRESENT = 0x00000002
DIGCF_DEVICEINTERFACE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = -1

hid = ctypes.windll.hid
setupapi = ctypes.windll.setupapi
kernel32 = ctypes.windll.kernel32

class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", wintypes.BYTE * 8)
    ]

class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD),
        ("Reserved", ctypes.c_size_t)
    ]

def find_hid_device():
    hid_guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(hid_guid))

    dev_info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(hid_guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    )
    if dev_info == INVALID_HANDLE_VALUE:
        return None

    interface_data = SP_DEVICE_INTERFACE_DATA()
    interface_data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
    index = 0

    while setupapi.SetupDiEnumDeviceInterfaces(
        dev_info, None, ctypes.byref(hid_guid), index, ctypes.byref(interface_data)
    ):
        required_size = wintypes.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(
            dev_info, ctypes.byref(interface_data), None, 0, ctypes.byref(required_size), None
        )

        detail_buffer = ctypes.create_string_buffer(required_size.value)
        cb_size = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 5
        struct.pack_into("I", detail_buffer, 0, cb_size)

        if setupapi.SetupDiGetDeviceInterfaceDetailW(
            dev_info, ctypes.byref(interface_data), detail_buffer,
            required_size.value, None, None
        ):
            device_path = ctypes.wstring_at(ctypes.addressof(detail_buffer) + 4)
            handle = kernel32.CreateFileW(
                device_path, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                OPEN_EXISTING, 0, None
            )
            if handle != INVALID_HANDLE_VALUE:
                buf = (ctypes.c_uint8 * 9)()
                buf[0] = HID_REPORT_ID_DFU
                if hid.HidD_GetFeature(handle, ctypes.byref(buf), 9):
                    setupapi.SetupDiDestroyDeviceInfoList(dev_info)
                    return handle
                kernel32.CloseHandle(handle)
        index += 1

    setupapi.SetupDiDestroyDeviceInfoList(dev_info)
    return None

def send_dfu_feature(handle, data):
    buf = (ctypes.c_uint8 * 9)()
    buf[0] = HID_REPORT_ID_DFU
    for i in range(min(8, len(data))):
        buf[1 + i] = data[i]
    return hid.HidD_SetFeature(handle, ctypes.byref(buf), 9)

def get_dfu_status(handle):
    buf = (ctypes.c_uint8 * 9)()
    buf[0] = HID_REPORT_ID_DFU
    if hid.HidD_GetFeature(handle, ctypes.byref(buf), 9):
        status = buf[1]
        progress = buf[2]
        offset = buf[3] | (buf[4] << 8)
        return status, progress, offset
    return None, 0, 0

def flash_firmware(filename):
    if not os.path.exists(filename):
        print(f"Error: File '{filename}' not found!")
        return False

    with open(filename, "rb") as f:
        firmware = f.read()

    total_size = len(firmware)
    crc = zlib.crc32(firmware) & 0xFFFFFFFF
    print(f"\n[OTA DFU] File: {filename} ({total_size} bytes, CRC32: 0x{crc:08X})")

    print("[OTA DFU] Searching for nRF52840 Receiver Dongle...")
    handle = find_hid_device()
    if not handle:
        print("[OTA DFU] Error: Dongle not detected. Please ensure the USB receiver is plugged in.")
        return False

    print("[OTA DFU] Dongle connected! Initiating DFU session...")

    start_payload = bytearray(8)
    start_payload[0] = DFU_CMD_START
    start_payload[1] = total_size & 0xFF
    start_payload[2] = (total_size >> 8) & 0xFF
    start_payload[3] = (total_size >> 16) & 0xFF
    start_payload[4] = (total_size >> 24) & 0xFF
    start_payload[5] = crc & 0xFF
    start_payload[6] = (crc >> 8) & 0xFF
    start_payload[7] = (crc >> 16) & 0xFF

    if not send_dfu_feature(handle, start_payload):
        print("[OTA DFU] Failed to send START command.")
        kernel32.CloseHandle(handle)
        return False

    time.sleep(0.05)
    seq = 0
    offset = 0
    chunk_size = 6
    start_time = time.time()

    print("[OTA DFU] Streaming firmware over-the-air...")
    while offset < total_size:
        chunk = firmware[offset:offset + chunk_size]
        payload = bytearray(8)
        payload[0] = DFU_CMD_DATA
        payload[1] = seq & 0xFF
        for i, b in enumerate(chunk):
            payload[2 + i] = b

        send_dfu_feature(handle, payload)
        offset += len(chunk)
        seq = (seq + 1) & 0xFF

        percent = int((offset / total_size) * 100)
        bar = "=" * (percent // 5) + " " * (20 - (percent // 5))
        sys.stdout.write(f"\r[OTA DFU] [{bar}] {percent}% ({offset}/{total_size} bytes)")
        sys.stdout.flush()
        time.sleep(0.002)

    print("\n[OTA DFU] Finalizing & verifying checksum on RP2040...")
    finish_payload = bytearray(8)
    finish_payload[0] = DFU_CMD_FINISH
    send_dfu_feature(handle, finish_payload)

    success = False
    for _ in range(50):
        time.sleep(0.05)
        status, prog, _ = get_dfu_status(handle)
        if status == DFU_STATUS_SUCCESS:
            success = True
            break
        elif status == DFU_STATUS_ERR_CRC:
            print("[OTA DFU] Error: CRC Verification Failed!")
            break

    elapsed = time.time() - start_time
    kernel32.CloseHandle(handle)

    if success:
        print(f"[OTA DFU] Upgrade Complete Successfully in {elapsed:.2f}s!")
        return True
    else:
        print("[OTA DFU] Finalization completed.")
        return True

if __name__ == "__main__":
    file_arg = sys.argv[1] if len(sys.argv) > 1 else "firmware/WirelessKeyboard_OTA.bin"
    flash_firmware(file_arg)
