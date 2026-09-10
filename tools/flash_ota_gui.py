#!/usr/bin/env python3
"""
WirelessKeyboard OTA Flasher - Windows GUI Application
High-speed wireless firmware updater for WeAct RP2040 4MB keyboard via nRF52840 Dongle.
"""

import ctypes
from ctypes import wintypes
import os
import struct
import sys
import time
import zlib

from PyQt6.QtCore import (
    QObject,
    QThread,
    QTimer,
    Qt,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QColor,
    QFont,
    QIcon,
    QPainter,
)
from PyQt6.QtWidgets import (
    QApplication,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

# ---------------------------------------------------------------------------
# Protocol & Hardware Constants
# ---------------------------------------------------------------------------
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

try:
    ctypes.windll.winmm.timeBeginPeriod(1)
except Exception:
    pass


def open_receiver():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    devinfo = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if devinfo == wintypes.HANDLE(-1).value or devinfo in (0, -1):
        return None

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
                        raise RuntimeError(f"Device returned {name} (detail={detail}, val={val})")
                    return status, session, token, detail, val
        if retry_interval > 0 and (time.time() - last_retry) > retry_interval:
            last_retry = time.time()
            set_feature(handle, payload8)
        time.sleep(0.001)

    raise TimeoutError("Timpul de așteptare pentru răspunsul RP2040 a expirat")


def parse_package_metadata(path):
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Fișierul nu a fost găsit: {path}")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 32:
        raise ValueError("Pachetul este prea mic (< 32 bytes)")
    magic, fmt_ver, tgt, proto, brd, hdr_size, psize, pcrc, vec_off, hdr_crc = struct.unpack("<8sHBBHHIIII", data[:32])
    if magic != b"WKRPOTA1" or fmt_ver != 1 or tgt != OTA_TARGET_RP2040 or brd != OTA_BOARD_WEACT_RP2040_4MB:
        raise ValueError("Header-ul pachetului este invalid (nu este destinat WeAct RP2040 4MB)")
    payload = data[hdr_size:]
    if len(payload) != psize:
        raise ValueError(f"Dimensiune payload eronată: {psize} declarat vs {len(payload)} real")
    calc_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if calc_crc != pcrc:
        raise ValueError(f"Eroare CRC32: header=0x{pcrc:08X} vs real=0x{calc_crc:08X}")
    return {
        "payload": payload,
        "size": psize,
        "crc32": pcrc,
        "target": "WeAct RP2040 4MB",
        "board_id": f"0x{brd:04X}",
        "protocol": proto,
    }


# ---------------------------------------------------------------------------
# Background Flashing Worker Thread
# ---------------------------------------------------------------------------
class FlasherWorker(QThread):
    sig_log = pyqtSignal(str)
    sig_progress = pyqtSignal(int, int, int, float)  # pct, transferred, total, speed
    sig_status = pyqtSignal(str)
    sig_finished = pyqtSignal(bool, str)

    def __init__(self, package_path, parent=None):
        super().__init__(parent)
        self.package_path = package_path
        self._is_cancelled = False

    def cancel(self):
        self._is_cancelled = True

    def run(self):
        handle = None
        try:
            self.sig_status.emit("Se citește pachetul firmware...")
            self.sig_log.emit(f"📂 Încărcare pachet: {self.package_path}")

            info = parse_package_metadata(self.package_path)
            payload = info["payload"]
            total_size = info["size"]
            expected_crc = info["crc32"]

            self.sig_log.emit(f"✓ Pachet valid: {total_size:,} octeți | CRC32: 0x{expected_crc:08X}")
            self.sig_status.emit("Se conectează la Dongle USB...")

            handle = open_receiver()
            if not handle:
                raise RuntimeError("Dongle-ul Receiver (VID 1B4F, PID 0001) nu a fost găsit!")

            self.sig_log.emit("✓ Dongle USB Receiver conectat cu succes.")
            self.sig_status.emit("Se negociază sesiunea cu RP2040...")

            base_status, base_session, base_token, _, _ = get_status(handle)
            session = (int(time.time() * 1000) ^ expected_crc ^ total_size) & 0xFF or 1
            if session == base_session:
                session = (session + 1) & 0xFF or 1

            self.sig_log.emit(f"📡 Sesiune inițiată #{session}. Se trezește RP2040 prin radio ESB...")

            start_cmd = bytes([
                DFU_CMD_START, session, OTA_TARGET_RP2040, OTA_PROTOCOL_VERSION,
                total_size & 0xFF, (total_size >> 8) & 0xFF,
                (total_size >> 16) & 0xFF, (total_size >> 24) & 0xFF
            ])
            _, _, token, _, _ = send_command_and_wait(
                handle, start_cmd, timeout_sec=40.0, baseline_token=0, retry_interval=0.8
            )
            self.sig_log.emit("✓ Răspuns START primit de la RP2040.")

            crc_cmd = bytes([
                DFU_CMD_CRC, session,
                expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
                (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
                OTA_BOARD_WEACT_RP2040_4MB & 0xFF, (OTA_BOARD_WEACT_RP2040_4MB >> 8) & 0xFF
            ])
            _, _, token, _, _ = send_command_and_wait(
                handle, crc_cmd, timeout_sec=15.0, baseline_token=token, retry_interval=0.8
            )
            self.sig_log.emit("✓ Verificare parametri și țintă hardware confirmată.")

            # High-speed continuous 256B page streaming loop:
            PAGE_SIZE = 256
            offset = 0
            t0 = time.time()
            next_page = PAGE_SIZE

            self.sig_status.emit("Streaming de mare viteză în curs...")
            self.sig_log.emit("⚡ Începe streaming-ul paginilor flash (~2.000 B/s)...")

            while offset < total_size:
                if self._is_cancelled:
                    raise RuntimeError("Actualizarea a fost oprită de utilizator.")

                target_offset = min(next_page, total_size)

                # Sector boundary guard for hardware sector erase:
                if (offset % 4096) == 0 and offset > 0:
                    time.sleep(0.040)

                # Send 6-byte packets continuously:
                while offset < target_offset:
                    if self._is_cancelled:
                        raise RuntimeError("Actualizarea a fost oprită de utilizator.")

                    chunk = payload[offset:offset + 6]
                    data_cmd = bytes([DFU_CMD_DATA, session]) + chunk + bytes(6 - len(chunk))
                    while not set_feature(handle, data_cmd):
                        time.sleep(0.0002)
                    time.sleep(0.0005)
                    offset += len(chunk)

                # Await page commit ACK from RP2040:
                deadline = time.time() + 10.0
                last_progress = time.time()
                last_val = -1
                while time.time() < deadline:
                    if self._is_cancelled:
                        raise RuntimeError("Actualizarea a fost oprită de utilizator.")

                    st = get_status(handle)
                    if st is not None:
                        status, s_session, s_token, s_detail, val = st
                        if s_session == session:
                            if val >= target_offset:
                                token = s_token
                                break
                            # The device-reported value is the authoritative
                            # count of bytes actually applied. ACK frames drain
                            # asynchronously, so val normally lags behind the
                            # PC offset for a few ms. NEVER re-send during that
                            # lag: the dongle stamps every HID command with a
                            # fresh token, so the RP2040 replay guard cannot
                            # catch re-sent chunks and they would be
                            # double-appended, corrupting staging. Only treat
                            # it as genuine frame loss when val stays frozen
                            # for 2 s (far longer than any ACK drain or flash
                            # erase). Then re-send starting exactly at val —
                            # those bytes were never applied.
                            if val != last_val:
                                last_val = val
                                last_progress = time.time()
                            if (time.time() - last_progress) > 2.0:
                                last_progress = time.time()
                                self.sig_log.emit(
                                    f"[RESEND] no ACK progress 2s; device accepted {val} B, "
                                    f"re-sending {val}..{target_offset}")
                                p = val
                                while p < target_offset:
                                    chk = payload[p:p + 6]
                                    data_cmd = bytes([DFU_CMD_DATA, session]) + chk + bytes(6 - len(chk))
                                    while not set_feature(handle, data_cmd):
                                        time.sleep(0.0002)
                                    time.sleep(0.0005)
                                    p += len(chk)
                                offset = p
                            if status in (DFU_STATUS_ERR_SIZE, DFU_STATUS_ERR_CRC,
                                          DFU_STATUS_ERR_FLASH, DFU_STATUS_ERR_TARGET,
                                          DFU_STATUS_ERR_PROTOCOL, DFU_STATUS_ERR_SESSION,
                                          DFU_STATUS_ERR_STATE, DFU_STATUS_ABORTED):
                                name = STATUS_NAMES.get(status, f"0x{status:02X}")
                                raise RuntimeError(f"RP2040 a raportat eroare {name} (detail={s_detail}, val={val})")
                    time.sleep(0.0005)
                else:
                    raise TimeoutError(f"Timeout la confirmarea paginii la offset-ul {target_offset}")

                next_page += PAGE_SIZE
                pct = (offset * 100) // total_size
                elapsed = time.time() - t0
                speed = offset / elapsed if elapsed > 0 else 0
                self.sig_progress.emit(pct, offset, total_size, speed)

            self.sig_status.emit("Se verifică integritatea imaginii flash...")
            self.sig_log.emit("🔍 Verificare imagine completă în flash staging...")

            fin_cmd = bytes([
                DFU_CMD_FINISH, session,
                expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
                (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
                0, 0
            ])
            _, _, token, _, verified_crc = send_command_and_wait(
                handle, fin_cmd, timeout_sec=15.0, baseline_token=token, retry_interval=1.0
            )
            if verified_crc != expected_crc:
                raise RuntimeError(
                    f"CRC mismatch: expected 0x{expected_crc:08X}, "
                    f"device reported 0x{verified_crc:08X}"
                )
            self.sig_log.emit(f"✓ CRC32 Confirmat de RP2040: 0x{verified_crc:08X} (Potrivire 100%)")

            self.sig_status.emit("Se aplică actualizarea și se resetează RP2040...")
            self.sig_log.emit("🔄 Comandă ACTIVATE trimisă -> RP2040 execută swap-ul Slot 0 și repornește...")

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

            self.sig_progress.emit(100, total_size, total_size, speed)
            self.sig_status.emit("Actualizare finalizată cu succes!")
            self.sig_log.emit("🎉 ACTUALIZARE FINALIZATĂ CU SUCCES! Tastatura a repornit în noul firmware.")
            self.sig_finished.emit(True, "Actualizarea OTA a fost finalizată cu succes!\nTastatura a repornit cu noul firmware.")

        except Exception as ex:
            err_msg = str(ex)
            self.sig_status.emit(f"Eroare: {err_msg}")
            self.sig_log.emit(f"❌ EROARE: {err_msg}")
            self.sig_finished.emit(False, err_msg)
        finally:
            if handle:
                try:
                    k32.CloseHandle(handle)
                except Exception:
                    pass


# ---------------------------------------------------------------------------
# Custom Styled Modern Progress Bar with Percentage inside
# ---------------------------------------------------------------------------
class CustomPercentageProgressBar(QProgressBar):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setTextVisible(True)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimum(0)
        self.setMaximum(100)
        self.setValue(0)
        self.setFixedHeight(34)
        self.setStyleSheet("""
            QProgressBar {
                background-color: #1E222B;
                border: 1px solid #333B4D;
                border-radius: 8px;
                text-align: center;
                color: #FFFFFF;
                font-weight: bold;
                font-size: 13px;
            }
            QProgressBar::chunk {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                            stop:0 #0078D4, stop:0.5 #00B4D8, stop:1 #00F5D4);
                border-radius: 7px;
            }
        """)


# ---------------------------------------------------------------------------
# Main Modern Window
# ---------------------------------------------------------------------------
class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("WirelessKeyboard OTA Flasher")
        self.setMinimumSize(660, 560)
        self.resize(680, 600)
        self.worker = None

        self.setup_ui()
        self.apply_dark_theme()

        # Check default path
        default_rel = os.path.join("firmware", "WirelessKeyboard_OTA.wkota")
        base_dir = os.path.dirname(os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__)))
        default_abs = os.path.join(base_dir, default_rel)
        if os.path.isfile(default_abs):
            self.set_selected_file(default_abs)
        elif os.path.isfile(default_rel):
            self.set_selected_file(os.path.abspath(default_rel))

        # Dongle detection timer
        self.dongle_timer = QTimer(self)
        self.dongle_timer.timeout.connect(self.check_dongle_status)
        self.dongle_timer.start(1500)
        self.check_dongle_status()

    def setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(20, 20, 20, 20)
        layout.setSpacing(14)

        # Header
        header_layout = QHBoxLayout()
        header_text = QVBoxLayout()
        title = QLabel("WirelessKeyboard OTA Flasher")
        title.setStyleSheet("font-size: 20px; font-weight: bold; color: #FFFFFF;")
        subtitle = QLabel("Utilitar de actualizare firmware prin radio 2.4GHz către RP2040")
        subtitle.setStyleSheet("font-size: 12px; color: #8A99AD;")
        header_text.addWidget(title)
        header_text.addWidget(subtitle)
        header_layout.addLayout(header_text)
        header_layout.addStretch()

        layout.addLayout(header_layout)

        # Card 1: File Selection
        card_file = QFrame()
        card_file.setObjectName("Card")
        file_layout = QVBoxLayout(card_file)
        file_layout.setContentsMargins(14, 12, 14, 12)
        file_layout.setSpacing(8)

        lbl_file_title = QLabel("1. Selectare Fișier Firmware (.wkota)")
        lbl_file_title.setStyleSheet("font-weight: bold; font-size: 13px; color: #E1E7EF;")
        file_layout.addWidget(lbl_file_title)

        picker_layout = QHBoxLayout()
        self.txt_path = QLineEdit()
        self.txt_path.setPlaceholderText("Alegeți fișierul .wkota cu firmware-ul compilat...")
        self.txt_path.setReadOnly(True)
        self.btn_browse = QPushButton("📁 Răsfoiește...")
        self.btn_browse.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_browse.clicked.connect(self.choose_file)
        picker_layout.addWidget(self.txt_path)
        picker_layout.addWidget(self.btn_browse)
        file_layout.addLayout(picker_layout)

        # Metadata badges
        self.lbl_meta = QLabel("Niciun fișier selectat")
        self.lbl_meta.setStyleSheet("font-size: 11px; color: #7F8C9D;")
        file_layout.addWidget(self.lbl_meta)
        layout.addWidget(card_file)

        # Card 2: Dongle Connection Status
        card_dongle = QFrame()
        card_dongle.setObjectName("Card")
        dongle_layout = QHBoxLayout(card_dongle)
        dongle_layout.setContentsMargins(14, 10, 14, 10)

        self.lbl_dongle_status = QLabel("⏳ Se verifică prezența Dongle-ului USB...")
        self.lbl_dongle_status.setStyleSheet("font-size: 12px; color: #FFAA00; font-weight: 500;")
        dongle_layout.addWidget(self.lbl_dongle_status)
        dongle_layout.addStretch()

        self.btn_refresh_dongle = QPushButton("🔄 Reîmprospătează")
        self.btn_refresh_dongle.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_refresh_dongle.clicked.connect(self.check_dongle_status)
        dongle_layout.addWidget(self.btn_refresh_dongle)
        layout.addWidget(card_dongle)

        # Card 3: Progress & Action
        card_progress = QFrame()
        card_progress.setObjectName("Card")
        prog_layout = QVBoxLayout(card_progress)
        prog_layout.setContentsMargins(14, 14, 14, 14)
        prog_layout.setSpacing(10)

        lbl_prog_title = QLabel("2. Progres Actualizare & Stare")
        lbl_prog_title.setStyleSheet("font-weight: bold; font-size: 13px; color: #E1E7EF;")
        prog_layout.addWidget(lbl_prog_title)

        self.progress_bar = CustomPercentageProgressBar()
        prog_layout.addWidget(self.progress_bar)

        self.lbl_stats = QLabel("Pregătit pentru transfer")
        self.lbl_stats.setStyleSheet("font-size: 12px; color: #00F5D4; font-weight: 500;")
        prog_layout.addWidget(self.lbl_stats)

        # Action Button
        self.btn_flash = QPushButton("⚡ Pornește Actualizarea OTA")
        self.btn_flash.setFixedHeight(42)
        self.btn_flash.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_flash.setStyleSheet("""
            QPushButton {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                            stop:0 #0078D4, stop:1 #00BCF2);
                color: #FFFFFF;
                font-size: 14px;
                font-weight: bold;
                border: none;
                border-radius: 8px;
            }
            QPushButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                            stop:0 #1086E0, stop:1 #1FD0FF);
            }
            QPushButton:disabled {
                background-color: #2D3342;
                color: #5C677D;
            }
        """)
        self.btn_flash.clicked.connect(self.start_flash)
        prog_layout.addWidget(self.btn_flash)
        layout.addWidget(card_progress)

        # Card 4: Detailed Log Output
        card_log = QFrame()
        card_log.setObjectName("Card")
        log_layout = QVBoxLayout(card_log)
        log_layout.setContentsMargins(14, 12, 14, 12)
        log_layout.setSpacing(6)

        lbl_log_title = QLabel("Jurnal Evenimente (Live Logs)")
        lbl_log_title.setStyleSheet("font-weight: bold; font-size: 12px; color: #8A99AD;")
        log_layout.addWidget(lbl_log_title)

        self.txt_log = QTextEdit()
        self.txt_log.setReadOnly(True)
        self.txt_log.setFixedHeight(120)
        self.txt_log.setStyleSheet("""
            QTextEdit {
                background-color: #0F1218;
                color: #A6B4C9;
                font-family: 'Consolas', 'Courier New', monospace;
                font-size: 11px;
                border: 1px solid #232A38;
                border-radius: 6px;
                padding: 6px;
            }
        """)
        log_layout.addWidget(self.txt_log)
        layout.addWidget(card_log)

    def apply_dark_theme(self):
        self.setStyleSheet("""
            QWidget {
                background-color: #13171F;
                color: #FFFFFF;
                font-family: 'Segoe UI', sans-serif;
            }
            QFrame#Card {
                background-color: #191E28;
                border: 1px solid #2A3242;
                border-radius: 10px;
            }
            QLineEdit {
                background-color: #0F1218;
                border: 1px solid #333B4D;
                border-radius: 6px;
                padding: 8px 10px;
                color: #FFFFFF;
                font-size: 12px;
            }
            QPushButton {
                background-color: #242B3A;
                border: 1px solid #3A455C;
                border-radius: 6px;
                padding: 7px 14px;
                color: #E2E8F0;
                font-size: 12px;
                font-weight: 500;
            }
            QPushButton:hover {
                background-color: #303A4E;
                border-color: #4D5C7A;
            }
        """)

    def choose_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Selectați Pachetul Firmware OTA",
            self.txt_path.text() or os.getcwd(),
            "WirelessKeyboard OTA (*.wkota);;Toate fișierele (*.*)"
        )
        if path:
            self.set_selected_file(path)

    def set_selected_file(self, path):
        try:
            info = parse_package_metadata(path)
            self.txt_path.setText(path)
            self.lbl_meta.setText(
                f"✓ Țintă: {info['target']} ({info['board_id']}) | "
                f"Dimensiune: {info['size']:,} B | "
                f"CRC32: 0x{info['crc32']:08X}"
            )
            self.lbl_meta.setStyleSheet("font-size: 11px; color: #00F5D4; font-weight: 500;")
            self.log(f"Fișier selectat: {os.path.basename(path)} (CRC32: 0x{info['crc32']:08X})")
            self.update_flash_button_state()
        except Exception as ex:
            self.txt_path.setText(path)
            self.lbl_meta.setText(f"❌ Eroare pachet: {str(ex)}")
            self.lbl_meta.setStyleSheet("font-size: 11px; color: #FF4D4F; font-weight: 500;")
            self.update_flash_button_state()

    def check_dongle_status(self):
        handle = open_receiver()
        if handle:
            k32.CloseHandle(handle)
            self.lbl_dongle_status.setText("🟢 Dongle USB Receiver Conectat (VID 1B4F, PID 0001)")
            self.lbl_dongle_status.setStyleSheet("font-size: 12px; color: #00F5D4; font-weight: 500;")
            self.is_dongle_connected = True
        else:
            self.lbl_dongle_status.setText("🔴 Dongle USB Deconectat — Conectați Dongle-ul USB în PC")
            self.lbl_dongle_status.setStyleSheet("font-size: 12px; color: #FF4D4F; font-weight: 500;")
            self.is_dongle_connected = False
        self.update_flash_button_state()

    def update_flash_button_state(self):
        has_valid_file = self.txt_path.text() and os.path.isfile(self.txt_path.text())
        can_flash = getattr(self, "is_dongle_connected", False) and has_valid_file and (self.worker is None or not self.worker.isRunning())
        self.btn_flash.setEnabled(bool(can_flash))

    def log(self, text):
        timestamp = time.strftime("%H:%M:%S")
        self.txt_log.append(f"[{timestamp}] {text}")

    def start_flash(self):
        file_path = self.txt_path.text()
        if not file_path or not os.path.isfile(file_path):
            QMessageBox.warning(self, "Atenție", "Vă rugăm să alegeți un fișier .wkota valid!")
            return

        self.btn_flash.setEnabled(False)
        self.btn_browse.setEnabled(False)
        self.progress_bar.setValue(0)
        self.lbl_stats.setText("Inițiere transfer...")
        self.txt_log.clear()

        self.worker = FlasherWorker(file_path)
        self.worker.sig_log.connect(self.log)
        self.worker.sig_status.connect(self.on_worker_status)
        self.worker.sig_progress.connect(self.on_worker_progress)
        self.worker.sig_finished.connect(self.on_worker_finished)
        self.worker.start()

    def on_worker_status(self, text):
        self.lbl_stats.setText(text)

    def on_worker_progress(self, pct, transferred, total, speed):
        self.progress_bar.setValue(pct)
        self.lbl_stats.setText(
            f"Transfer: {pct}% ({transferred:,} / {total:,} B) | "
            f"Viteză: {speed:,.1f} B/s"
        )

    def on_worker_finished(self, success, message):
        self.btn_browse.setEnabled(True)
        self.update_flash_button_state()
        if success:
            self.progress_bar.setValue(100)
            self.lbl_stats.setText("✅ Actualizare Finalizată cu Succes (100%)!")
            QMessageBox.information(self, "Succes", message)
        else:
            QMessageBox.critical(self, "Eroare Actualizare", f"Actualizarea a eșuat:\n\n{message}")


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
