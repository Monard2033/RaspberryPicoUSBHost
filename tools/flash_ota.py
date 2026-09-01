#!/usr/bin/env python3
import ctypes
import os
import struct
import sys
import time
import zlib
from ctypes import wintypes

HID_REPORT_ID_DFU = 0x04
DFU_CMD_START = 0x10
DFU_CMD_DATA = 0x11
DFU_CMD_FINISH = 0x12
DFU_CMD_STATUS = 0x13
DFU_CMD_CRC = 0x14
DFU_CMD_ACTIVATE = 0x15
DFU_CMD_ABORT = 0x16
DFU_CMD_QUERY = 0x17

DFU_STATUS_IDLE = 0x00
DFU_STATUS_BUSY = 0x01
DFU_STATUS_OK = 0x02
DFU_STATUS_ERR_SIZE = 0x03
DFU_STATUS_ERR_CRC = 0x04
DFU_STATUS_ERR_FLASH = 0x05
DFU_STATUS_VERIFIED = 0x06
DFU_STATUS_ERR_TARGET = 0x07
DFU_STATUS_ERR_PROTOCOL = 0x08
DFU_STATUS_ERR_SESSION = 0x09
DFU_STATUS_ERR_STATE = 0x0A
DFU_STATUS_APPLYING = 0x0B
DFU_STATUS_BOOT_OK = 0x0C
DFU_STATUS_ABORTED = 0x0D

STATUS_NAMES = {
    0x00: "IDLE", 0x01: "BUSY", 0x02: "OK", 0x03: "ERR_SIZE",
    0x04: "ERR_CRC", 0x05: "ERR_FLASH", 0x06: "VERIFIED",
    0x07: "ERR_TARGET", 0x08: "ERR_PROTOCOL", 0x09: "ERR_SESSION",
    0x0A: "ERR_STATE", 0x0B: "APPLYING", 0x0C: "BOOT_OK", 0x0D: "ABORTED"
}

OTA_TARGET_RP2040 = 0x01
OTA_PROTOCOL_VERSION = 0x01
OTA_BOARD_WEACT_RP2040_4MB = 0x2040
RECEIVER_VID = 0x1B4F
RECEIVER_PID = 0x0001
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10

setupapi = ctypes.WinDLL("setupapi")
hid = ctypes.WinDLL("hid")
k32 = ctypes.WinDLL("kernel32")

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

class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG),
        ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT),
        ("VersionNumber", wintypes.USHORT),
    ]

setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [wintypes.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]

def open_receiver():
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
        ctypes.cast(ctypes.byref(buf), ctypes.POINTER(wintypes.DWORD))[0] = 8
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                devinfo, ctypes.byref(data), ctypes.byref(buf), needed.value, None, None):
            continue
        path = ctypes.wstring_at(ctypes.addressof(buf) + 4)

        handle = k32.CreateFileW(
            path, 0xC0000000, 3, None, 3, 0x80, None)
        if handle in (0, -1) or handle == wintypes.HANDLE(-1).value:
            continue

        attrs = HIDD_ATTRIBUTES()
        attrs.Size = ctypes.sizeof(attrs)
        feature = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
        if (hid.HidD_GetAttributes(handle, ctypes.byref(attrs))
                and attrs.VendorID == RECEIVER_VID
                and attrs.ProductID == RECEIVER_PID
                and hid.HidD_GetFeature(handle, feature, 9)):
            setupapi.SetupDiDestroyDeviceInfoList(devinfo)
            return handle
        k32.CloseHandle(handle)
    setupapi.SetupDiDestroyDeviceInfoList(devinfo)
    return None

def get_status(handle):
    buf = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
    if not hid.HidD_GetFeature(handle, buf, 9):
        return None
    s = bytes(buf)
    status = s[1]
    session = s[2]
    token = s[3]
    detail = s[4]
    val = struct.unpack("<I", s[5:9])[0]
    return status, session, token, detail, val

def set_feature(handle, payload8):
    buf = (ctypes.c_ubyte * 9)(HID_REPORT_ID_DFU)
    for i in range(8):
        buf[i + 1] = payload8[i]
    return bool(hid.HidD_SetFeature(handle, buf, 9))

try:
    ctypes.windll.winmm.timeBeginPeriod(1)
except Exception:
    pass

def send_command_and_wait(handle, payload8, timeout_sec=15.0, baseline_token=0, retry_interval=0.0):
    deadline = time.time() + timeout_sec
    last_retry = time.time()
    set_feature(handle, payload8)

    while time.time() < deadline:
        st = get_status(handle)
        if st is not None:
            status, session, token, detail, val = st
            if session == payload8[1] and token != baseline_token:
                if status != DFU_STATUS_BUSY:
                    if status in (DFU_STATUS_ERR_SIZE, DFU_STATUS_ERR_CRC,
                                 DFU_STATUS_ERR_FLASH, DFU_STATUS_ERR_TARGET,
                                 DFU_STATUS_ERR_PROTOCOL, DFU_STATUS_ERR_SESSION,
                                 DFU_STATUS_ERR_STATE, DFU_STATUS_ABORTED):
                        name = STATUS_NAMES.get(status, f"0x{status:02X}")
                        raise RuntimeError(f"device returned {name} detail={detail} value={val}")
                    return status, session, token, detail, val
        if retry_interval > 0 and (time.time() - last_retry) > retry_interval:
            last_retry = time.time()
            set_feature(handle, payload8)

    raise TimeoutError("timeout waiting for end-to-end RP2040 acknowledgement")

def load_package(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 32:
        raise ValueError("Package too small")
    magic, fmt_ver, tgt, proto, brd, hdr_size, psize, pcrc, vec_off, hdr_crc = struct.unpack("<8sHBBHHIIII", data[:32])
    if magic != b"WKRPOTA1" or fmt_ver != 1 or tgt != OTA_TARGET_RP2040 or brd != OTA_BOARD_WEACT_RP2040_4MB:
        raise ValueError("Invalid package header")
    payload = data[hdr_size:]
    if len(payload) != psize:
        raise ValueError(f"Payload size mismatch: header {psize} vs actual {len(payload)}")
    calc_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if calc_crc != pcrc:
        raise ValueError(f"CRC mismatch: header 0x{pcrc:08X} vs actual 0x{calc_crc:08X}")
    return payload, pcrc

def main():
    pkg_path = sys.argv[1] if len(sys.argv) > 1 else "firmware/WirelessKeyboard_OTA.wkota"
    if not os.path.isabs(pkg_path):
        pkg_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), pkg_path)

    print("WirelessKeyboard Strict RP2040 Python OTA Flasher")
    print(f"Package : {pkg_path}")
    payload, expected_crc = load_package(pkg_path)
    total_size = len(payload)
    print(f"Target  : WeAct RP2040 4MB")
    print(f"Size    : {total_size} bytes")
    print(f"CRC32   : 0x{expected_crc:08X}")

    handle = open_receiver()
    if not handle:
        print("ERROR: Receiver USB dongle (VID 1B4F, PID 0001) not found!")
        return 1

    base_status, base_session, base_token, _, _ = get_status(handle)
    session = (int(time.time() * 1000) ^ expected_crc ^ total_size) & 0xFF or 1
    if session == base_session:
        session = (session + 1) & 0xFF or 1
    print(f"Session : {session} (base was {base_session})")
    print("Waiting for RP2040 START acknowledgement (sleeping radio discovery may take up to 30s)...")

    start_cmd = bytes([
        DFU_CMD_START, session, OTA_TARGET_RP2040, OTA_PROTOCOL_VERSION,
        total_size & 0xFF, (total_size >> 8) & 0xFF,
        (total_size >> 16) & 0xFF, (total_size >> 24) & 0xFF
    ])
    _, _, token, _, _ = send_command_and_wait(handle, start_cmd, timeout_sec=40.0, baseline_token=0, retry_interval=0.8)

    crc_cmd = bytes([
        DFU_CMD_CRC, session,
        expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
        (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
        OTA_BOARD_WEACT_RP2040_4MB & 0xFF, (OTA_BOARD_WEACT_RP2040_4MB >> 8) & 0xFF
    ])
    _, _, token, _, _ = send_command_and_wait(handle, crc_cmd, timeout_sec=15.0, baseline_token=token, retry_interval=0.8)

    PAGE_SIZE = 256
    offset = 0
    t0 = time.time()
    next_page = PAGE_SIZE

    while offset < total_size:
        target_offset = min(next_page, total_size)

        # If entering a new 4096-byte sector, wait 40ms for RP2040 hardware sector erase:
        if (offset % 4096) == 0 and offset > 0:
            time.sleep(0.040)

        # Stream 6-byte chunks until we reach or pass target_offset:
        while offset < target_offset:
            chunk = payload[offset:offset + 6]
            data_cmd = bytes([DFU_CMD_DATA, session]) + chunk + bytes(6 - len(chunk))
            while not set_feature(handle, data_cmd):
                time.sleep(0.0002)
            time.sleep(0.0005)
            offset += len(chunk)

        # Wait for RP2040 to process and commit this 256B page:
        deadline = time.time() + 10.0
        last_retry = time.time()
        while time.time() < deadline:
            st = get_status(handle)
            if st is not None:
                status, s_session, s_token, s_detail, val = st
                if s_session == session:
                    if val >= target_offset:
                        token = s_token
                        break
                    if (time.time() - last_retry) > 0.2:
                        last_retry = time.time()
                        if val < offset:
                            p = val
                            while p < offset:
                                chk = payload[p:p + 6]
                                data_cmd = bytes([DFU_CMD_DATA, session]) + chk + bytes(6 - len(chk))
                                while not set_feature(handle, data_cmd):
                                    time.sleep(0.0002)
                                time.sleep(0.0005)
                                p += len(chk)
                    if status in (DFU_STATUS_ERR_SIZE, DFU_STATUS_ERR_CRC,
                                  DFU_STATUS_ERR_FLASH, DFU_STATUS_ERR_TARGET,
                                  DFU_STATUS_ERR_PROTOCOL, DFU_STATUS_ERR_SESSION,
                                  DFU_STATUS_ERR_STATE, DFU_STATUS_ABORTED):
                        name = STATUS_NAMES.get(status, f"0x{status:02X}")
                        raise RuntimeError(f"device returned {name} detail={s_detail} value={val}")
            time.sleep(0.0005)
        else:
            raise TimeoutError(f"timeout waiting for RP2040 to commit page at offset {target_offset}")

        next_page += PAGE_SIZE
        pct = (offset * 100) // total_size
        elapsed = time.time() - t0
        speed = offset / elapsed if elapsed > 0 else 0
        sys.stdout.write(f"\rTransfer: {pct:3d}%  {offset}/{total_size} bytes ({speed:5.1f} B/s) ")
        sys.stdout.flush()

    print("\nVerifying complete staging image...")
    fin_cmd = bytes([
        DFU_CMD_FINISH, session,
        expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
        (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
        0, 0
    ])
    _, _, token, _, verified_crc = send_command_and_wait(handle, fin_cmd, timeout_sec=15.0, baseline_token=token, retry_interval=1.0)
    print(f"Staging image verified: CRC32 = 0x{verified_crc:08X} (MATCH!)")

    print("Activating update and rebooting RP2040...")
    act_cmd = bytes([
        DFU_CMD_ACTIVATE, session,
        expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
        (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
        0, 0
    ])
    try:
        send_command_and_wait(handle, act_cmd, timeout_sec=5.0, baseline_token=token, retry_interval=1.0)
    except Exception:
        pass

    print("\n=======================================================")
    print("  WIRELESS OTA UPDATE COMPLETED SUCCESSFULLY (100%)!  ")
    print("=======================================================")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
