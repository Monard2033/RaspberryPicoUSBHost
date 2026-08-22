#!/usr/bin/env python3
"""Diagnostic probe for the strict OTA reverse chain.

Sends one harmless QUERY command and watches the DFU Feature report for a
session/token echo. Tests the full path PC -> Receiver -> ESB ACK payload ->
Transmitter -> SPI MISO -> RP2040 -> DFU_STATUS -> back to the PC, without
touching flash. A long window (80 s) also covers two OTA radio-discovery
cycles in case the keyboard's nRF is in System OFF.
"""

import ctypes
import sys
import time
from ctypes import wintypes

HID_REPORT_ID_DFU = 0x04
DFU_CMD_QUERY = 0x17
OTA_TARGET_RP2040 = 1
OTA_PROTOCOL_VERSION = 1
BOARD = 0x2040

RECEIVER_VID = 0x1B4F
RECEIVER_PID = 0x0001

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10

import ctypes.wintypes as wt

setupapi = ctypes.WinDLL("setupapi")
hid = ctypes.WinDLL("hid")


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD),
        ("Reserved", ctypes.c_void_p),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("DevicePath", ctypes.c_wchar * 1)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG),
        ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT),
        ("VersionNumber", wintypes.USHORT),
    ]


def find_receiver():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    devinfo = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if devinfo == wintypes.HANDLE(-1).value or devinfo in (0, -1):
        raise RuntimeError("SetupDiGetClassDevsW failed")

    index = 0
    while True:
        data = SP_DEVICE_INTERFACE_DATA()
        data.cbSize = ctypes.sizeof(data)
        if not setupapi.SetupDiEnumDeviceInterfaces(
                devinfo, None, ctypes.byref(guid), index, ctypes.byref(data)):
            break
        index += 1

        needed = wintypes.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(
            devinfo, ctypes.byref(data), None, 0, ctypes.byref(needed), None)
        if needed.value == 0:
            continue
        buf = ctypes.create_unicode_buffer(needed.value)
        detail = SP_DEVICE_INTERFACE_DETAIL_DATA_W.from_buffer(buf)
        detail.cbSize = 8  # x64 ABI quirk: cbSize is DWORD+padding worth 8
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                devinfo, ctypes.byref(data), buf, needed.value, None, None):
            continue
        path = ctypes.wstring_at(
            ctypes.addressof(buf) + 8,  # skip cbSize field
            (needed.value - 8) // 2)

        handle = ctypes.windll.kernel32.CreateFileW(
            path, 0xC0000000, 3, None, 3, 0x80, None)  # RW, share RW, OPEN_EXISTING
        if handle in (0, -1) or handle == wintypes.HANDLE(-1).value:
            continue

        attrs = HIDD_ATTRIBUTES()
        attrs.Size = ctypes.sizeof(attrs)
        feature = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
        if (hid.HidD_GetAttributes(handle, ctypes.byref(attrs))
                and attrs.VendorID == RECEIVER_VID
                and attrs.ProductID == RECEIVER_PID
                and hid.HidD_GetFeature(handle, feature, 9)):
            return handle
        ctypes.windll.kernel32.CloseHandle(handle)
    setupapi.SetupDiDestroyDeviceInfoList(devinfo)
    return None


def get_status(handle):
    buf = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
    if not hid.HidD_GetFeature(handle, buf, 9):
        return None
    return bytes(buf)


def set_feature(handle, payload8):
    buf = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
    for i in range(8):
        buf[i + 1] = payload8[i]
    return bool(hid.HidD_SetFeature(handle, buf, 9))


def main():
    handle = find_receiver()
    if not handle:
        print("PROBE FAILED: Receiver dongle (VID 1B4F/PID 0001, DFU report) not found")
        return 2

    print("Receiver opened.")
    base = get_status(handle)
    print(f"Baseline report: {base.hex(' ')}")

    session = int(time.time()) & 0xFF or 1
    query = bytes([DFU_CMD_QUERY, session, OTA_TARGET_RP2040,
                   OTA_PROTOCOL_VERSION,
                   BOARD & 0xFF, BOARD >> 8, 0, 0])
    print(f"Sending QUERY session={session}: {query.hex(' ')}")
    if not set_feature(handle, query):
        print("SetFeature rejected")
        return 2

    deadline = time.time() + 80
    last = None
    while time.time() < deadline:
        s = get_status(handle)
        if s and s != last:
            last = s
            print(f"[{time.time() - (deadline - 80):5.1f}s] report: {s.hex(' ')}")
            if s[2] == session and s[1] in (0x0C, 0x00) and s[3] != 0:
                print(f"ECHO OK: status=0x{s[1]:02X} token={s[3]} "
                      f"value={int.from_bytes(s[5:9], 'little')}")
                break
        if s and s[2] == session and s[3] != 0:
            print(f"ECHO OK: status=0x{s[1]:02X} token={s[3]} "
                  f"detail={s[4]} value={int.from_bytes(s[5:9], 'little')}")
            break
        time.sleep(0.25)

    if not (last and last[2] == session):
        print("NO ECHO: the RP2040 never confirmed the QUERY end-to-end.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
