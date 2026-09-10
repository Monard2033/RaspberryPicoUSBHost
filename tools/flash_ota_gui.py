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
    QComboBox,
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
# Localization (EN default, RU, RO)
# ---------------------------------------------------------------------------
LANGS = {
    "EN": {
        "title": "WirelessKeyboard OTA Flasher",
        "subtitle": "2.4 GHz radio firmware updater for the RP2040",
        "card_file": "1. Select Firmware File (.wkota)",
        "file_placeholder": "Choose a compiled firmware .wkota file...",
        "btn_browse": "📁 Browse...",
        "meta_none": "No file selected",
        "meta_ok": "✓ Target: {target} ({board}) | Size: {size} B | CRC32: 0x{crc:08X}",
        "meta_err": "❌ Package error: {err}",
        "file_selected_log": "File selected: {name} (CRC32: 0x{crc:08X})",
        "card_dongle_lbl": "",
        "dongle_check": "⏳ Checking for USB Dongle...",
        "dongle_ok": "🟢 USB Receiver Dongle Connected (VID 1B4F, PID 0001)",
        "dongle_off": "🔴 USB Dongle Disconnected — plug the USB Dongle into the PC",
        "btn_refresh": "🔄 Refresh",
        "card_prog": "2. Update Progress & Status",
        "lbl_stats": "Ready for transfer",
        "btn_flash": "⚡ Start OTA Update",
        "log_title": "Event Log (Live)",
        "dlg_open_title": "Select the OTA Firmware Package",
        "dlg_open_filter": "WirelessKeyboard OTA (*.wkota);;All files (*.*)",
        "warn_title": "Warning",
        "warn_body": "Please choose a valid .wkota file!",
        "err_title": "Update Error",
        "err_body": "The update failed:\n\n{msg}",
        "ok_title": "Success",
        "ok_body": "{msg}",
        "st_read": "Reading firmware package...",
        "st_connect": "Connecting to USB Dongle...",
        "st_negotiate": "Negotiating session with RP2040...",
        "st_stream": "High-speed streaming in progress...",
        "st_verify": "Verifying flash image integrity...",
        "st_apply": "Applying update and resetting RP2040...",
        "st_done": "Update finished successfully!",
        "st_init": "Initiating transfer...",
        "log_load": "📂 Loading package: {path}",
        "log_valid": "✓ Valid package: {size} bytes | CRC32: 0x{crc:08X}",
        "log_dongle_ok": "✓ USB Receiver Dongle connected successfully.",
        "log_session": "📡 Session #{session} started. Waking RP2040 via ESB radio...",
        "log_start_ack": "✓ START response received from RP2040.",
        "log_crc_ack": "✓ Parameters and hardware target confirmed.",
        "log_stream_begin": "⚡ Starting flash page streaming (~2,000 B/s)...",
        "log_cancel": "Update stopped by user.",
        "log_resend": "[RESEND] no ACK progress 2s; device accepted {val} B, re-sending {a}..{b}",
        "log_crc_ok": "✓ CRC32 confirmed by RP2040: 0x{crc:08X} (100% match)",
        "log_activate": "🔄 ACTIVATE command sent -> RP2040 swaps Slot 0 and reboots...",
        "log_done": "🎉 OTA UPDATE COMPLETED! The keyboard rebooted into the new firmware.",
        "ok_body_full": "The OTA update completed successfully!\nThe keyboard rebooted with the new firmware.",
        "err_packet": "The Receiver Dongle (VID 1B4F, PID 0001) was not found!",
        "err_dev": "RP2040 reported error {name} (detail={detail}, val={val})",
        "err_page_timeout": "Timeout waiting for page commit at offset {offset}",
        "err_crc": "CRC mismatch: expected 0x{exp:08X}, device reported 0x{got:08X}",
        "prog_transfer": "Transfer: {pct}% ({done} / {total} B) | Speed: {speed} B/s",
        "prog_done": "✅ Update Finished Successfully (100%)!",
        "err_prefix": "Error: {msg}",
        "log_error": "❌ ERROR: {msg}",
        "err_stop": "The update was stopped by the user.",
        "lang_lbl": "🌐",
    },
    "RU": {
        "title": "WirelessKeyboard OTA Flasher",
        "subtitle": "Обновление прошивки по радио 2.4 ГГц для RP2040",
        "card_file": "1. Выберите файл прошивки (.wkota)",
        "file_placeholder": "Выберите скомпилированный файл прошивки .wkota...",
        "btn_browse": "📁 Обзор...",
        "meta_none": "Файл не выбран",
        "meta_ok": "✓ Цель: {target} ({board}) | Размер: {size} Б | CRC32: 0x{crc:08X}",
        "meta_err": "❌ Ошибка пакета: {err}",
        "file_selected_log": "Файл выбран: {name} (CRC32: 0x{crc:08X})",
        "card_dongle_lbl": "",
        "dongle_check": "⏳ Поиск USB-донгла...",
        "dongle_ok": "🟢 USB-донгл приёмника подключён (VID 1B4F, PID 0001)",
        "dongle_off": "🔴 USB-донгл отключён — подключите USB-донгл к ПК",
        "btn_refresh": "🔄 Обновить",
        "card_prog": "2. Прогресс обновления и состояние",
        "lbl_stats": "Готов к передаче",
        "btn_flash": "⚡ Начать OTA-обновление",
        "log_title": "Журнал событий (Live)",
        "dlg_open_title": "Выберите пакет прошивки OTA",
        "dlg_open_filter": "WirelessKeyboard OTA (*.wkota);;Все файлы (*.*)",
        "warn_title": "Внимание",
        "warn_body": "Пожалуйста, выберите корректный файл .wkota!",
        "err_title": "Ошибка обновления",
        "err_body": "Обновление не удалось:\n\n{msg}",
        "ok_title": "Успех",
        "ok_body": "{msg}",
        "st_read": "Чтение пакета прошивки...",
        "st_connect": "Подключение к USB-донглу...",
        "st_negotiate": "Согласование сеанса с RP2040...",
        "st_stream": "Идёт потоковая передача...",
        "st_verify": "Проверка целостности образа во флеш-памяти...",
        "st_apply": "Применение обновления и перезагрузка RP2040...",
        "st_done": "Обновление успешно завершено!",
        "st_init": "Начало передачи...",
        "log_load": "📂 Загрузка пакета: {path}",
        "log_valid": "✓ Пакет корректен: {size} байт | CRC32: 0x{crc:08X}",
        "log_dongle_ok": "✓ USB-донгл приёмника успешно подключён.",
        "log_session": "📡 Сеанс #{session} начат. Пробуждение RP2040 по радио ESB...",
        "log_start_ack": "✓ Получен ответ START от RP2040.",
        "log_crc_ack": "✓ Параметры и аппаратная цель подтверждены.",
        "log_stream_begin": "⚡ Начало потоковой передачи страниц флеш (~2 000 Б/с)...",
        "log_cancel": "Обновление остановлено пользователем.",
        "log_resend": "[RESEND] нет прогресса ACK 2 с; устройство приняло {val} Б, повторная отправка {a}..{b}",
        "log_crc_ok": "✓ CRC32 подтверждено RP2040: 0x{crc:08X} (совпадение 100%)",
        "log_activate": "🔄 Команда ACTIVATE отправлена -> RP2040 меняет слоты и перезагружается...",
        "log_done": "🎉 OTA-ОБНОВЛЕНИЕ ЗАВЕРШЕНО! Клавиатура перезагрузилась с новой прошивкой.",
        "ok_body_full": "OTA-обновление успешно завершено!\nКлавиатура перезагрузилась с новой прошивкой.",
        "err_packet": "Донгл-приёмник (VID 1B4F, PID 0001) не найден!",
        "err_dev": "RP2040 сообщил об ошибке {name} (detail={detail}, val={val})",
        "err_page_timeout": "Таймаут подтверждения страницы по смещению {offset}",
        "err_crc": "Несовпадение CRC: ожидалось 0x{exp:08X}, устройство сообщило 0x{got:08X}",
        "prog_transfer": "Передача: {pct}% ({done} / {total} Б) | Скорость: {speed} Б/с",
        "prog_done": "✅ Обновление успешно завершено (100%)!",
        "err_prefix": "Ошибка: {msg}",
        "log_error": "❌ ОШИБКА: {msg}",
        "err_stop": "Обновление остановлено пользователем.",
        "lang_lbl": "🌐",
    },
    "RO": {
        "title": "WirelessKeyboard OTA Flasher",
        "subtitle": "Utilitar de actualizare firmware prin radio 2.4GHz către RP2040",
        "card_file": "1. Selectare Fișier Firmware (.wkota)",
        "file_placeholder": "Alegeți fișierul .wkota cu firmware-ul compilat...",
        "btn_browse": "📁 Răsfoiește...",
        "meta_none": "Niciun fișier selectat",
        "meta_ok": "✓ Țintă: {target} ({board}) | Dimensiune: {size} B | CRC32: 0x{crc:08X}",
        "meta_err": "❌ Eroare pachet: {err}",
        "file_selected_log": "Fișier selectat: {name} (CRC32: 0x{crc:08X})",
        "card_dongle_lbl": "",
        "dongle_check": "⏳ Se verifică prezența Dongle-ului USB...",
        "dongle_ok": "🟢 Dongle USB Receiver Conectat (VID 1B4F, PID 0001)",
        "dongle_off": "🔴 Dongle USB Deconectat — Conectați Dongle-ul USB în PC",
        "btn_refresh": "🔄 Reîmprospătează",
        "card_prog": "2. Progres Actualizare & Stare",
        "lbl_stats": "Pregătit pentru transfer",
        "btn_flash": "⚡ Pornește Actualizarea OTA",
        "log_title": "Jurnal Evenimente (Live Logs)",
        "dlg_open_title": "Selectați Pachetul Firmware OTA",
        "dlg_open_filter": "WirelessKeyboard OTA (*.wkota);;Toate fișierele (*.*)",
        "warn_title": "Atenție",
        "warn_body": "Vă rugăm să alegeți un fișier .wkota valid!",
        "err_title": "Eroare Actualizare",
        "err_body": "Actualizarea a eșuat:\n\n{msg}",
        "ok_title": "Succes",
        "ok_body": "{msg}",
        "st_read": "Se citește pachetul firmware...",
        "st_connect": "Se conectează la Dongle USB...",
        "st_negotiate": "Se negociază sesiunea cu RP2040...",
        "st_stream": "Streaming de mare viteză în curs...",
        "st_verify": "Se verifică integritatea imaginii flash...",
        "st_apply": "Se aplică actualizarea și se resetează RP2040...",
        "st_done": "Actualizare finalizată cu succes!",
        "st_init": "Inițiere transfer...",
        "log_load": "📂 Încărcare pachet: {path}",
        "log_valid": "✓ Pachet valid: {size} octeți | CRC32: 0x{crc:08X}",
        "log_dongle_ok": "✓ Dongle USB Receiver conectat cu succes.",
        "log_session": "📡 Sesiune inițiată #{session}. Se trezește RP2040 prin radio ESB...",
        "log_start_ack": "✓ Răspuns START primit de la RP2040.",
        "log_crc_ack": "✓ Verificare parametri și țintă hardware confirmată.",
        "log_stream_begin": "⚡ Începe streaming-ul paginilor flash (~2.000 B/s)...",
        "log_cancel": "Actualizarea a fost oprită de utilizator.",
        "log_resend": "[RESEND] no ACK progress 2s; device accepted {val} B, re-sending {a}..{b}",
        "log_crc_ok": "✓ CRC32 Confirmat de RP2040: 0x{crc:08X} (Potrivire 100%)",
        "log_activate": "🔄 Comandă ACTIVATE trimisă -> RP2040 execută swap-ul Slot 0 și repornește...",
        "log_done": "🎉 ACTUALIZARE FINALIZATĂ CU SUCCES! Tastatura a repornit în noul firmware.",
        "ok_body_full": "Actualizarea OTA a fost finalizată cu succes!\nTastatura a repornit cu noul firmware.",
        "err_packet": "Dongle-ul Receiver (VID 1B4F, PID 0001) nu a fost găsit!",
        "err_dev": "RP2040 a raportat eroare {name} (detail={detail}, val={val})",
        "err_page_timeout": "Timeout la confirmarea paginii la offset-ul {offset}",
        "err_crc": "CRC mismatch: expected 0x{exp:08X}, device reported 0x{got:08X}",
        "prog_transfer": "Transfer: {pct}% ({done} / {total} B) | Viteză: {speed} B/s",
        "prog_done": "✅ Actualizare Finalizată cu Succes (100%)!",
        "err_prefix": "Eroare: {msg}",
        "log_error": "❌ EROARE: {msg}",
        "err_stop": "Actualizarea a fost oprită de utilizator.",
        "lang_lbl": "🌐",
    },
}

CURRENT_LANG = "EN"


def tr(key, **kw):
    table = LANGS.get(CURRENT_LANG, LANGS["EN"])
    text = table.get(key) or LANGS["EN"].get(key) or key
    return text.format(**kw) if kw else text


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
            self.sig_status.emit(tr("st_read"))
            self.sig_log.emit(tr("log_load", path=self.package_path))

            info = parse_package_metadata(self.package_path)
            payload = info["payload"]
            total_size = info["size"]
            expected_crc = info["crc32"]

            self.sig_log.emit(tr("log_valid", size=f"{total_size:,}", crc=expected_crc))
            self.sig_status.emit(tr("st_connect"))

            handle = open_receiver()
            if not handle:
                raise RuntimeError(tr("err_packet"))

            self.sig_log.emit(tr("log_dongle_ok"))
            self.sig_status.emit(tr("st_negotiate"))

            base_status, base_session, base_token, _, _ = get_status(handle)
            session = (int(time.time() * 1000) ^ expected_crc ^ total_size) & 0xFF or 1
            if session == base_session:
                session = (session + 1) & 0xFF or 1

            self.sig_log.emit(tr("log_session", session=session))

            start_cmd = bytes([
                DFU_CMD_START, session, OTA_TARGET_RP2040, OTA_PROTOCOL_VERSION,
                total_size & 0xFF, (total_size >> 8) & 0xFF,
                (total_size >> 16) & 0xFF, (total_size >> 24) & 0xFF
            ])
            _, _, token, _, _ = send_command_and_wait(
                handle, start_cmd, timeout_sec=40.0, baseline_token=0, retry_interval=0.8
            )
            self.sig_log.emit(tr("log_start_ack"))

            crc_cmd = bytes([
                DFU_CMD_CRC, session,
                expected_crc & 0xFF, (expected_crc >> 8) & 0xFF,
                (expected_crc >> 16) & 0xFF, (expected_crc >> 24) & 0xFF,
                OTA_BOARD_WEACT_RP2040_4MB & 0xFF, (OTA_BOARD_WEACT_RP2040_4MB >> 8) & 0xFF
            ])
            _, _, token, _, _ = send_command_and_wait(
                handle, crc_cmd, timeout_sec=15.0, baseline_token=token, retry_interval=0.8
            )
            self.sig_log.emit(tr("log_crc_ack"))

            # High-speed continuous 256B page streaming loop:
            PAGE_SIZE = 256
            CHUNK_SIZE = 5
            offset = 0
            t0 = time.time()
            next_page = PAGE_SIZE

            self.sig_status.emit(tr("st_stream"))
            self.sig_log.emit(tr("log_stream_begin"))

            while offset < total_size:
                if self._is_cancelled:
                    raise RuntimeError("Actualizarea a fost oprită de utilizator.")

                target_offset = min(next_page, total_size)

                # Sector boundary guard for hardware sector erase:
                if (offset % 4096) == 0 and offset > 0:
                    time.sleep(0.040)

                # Send 5-byte packets with a sequence byte in data[7]; the
                # RP2040 accepts only the exact next sequence (mod 256) and
                # drops duplicates/out-of-order chunks idempotently:
                while offset < target_offset:
                    if self._is_cancelled:
                        raise RuntimeError(tr("err_stop"))

                    chunk = payload[offset:offset + CHUNK_SIZE]
                    seq = ((offset // CHUNK_SIZE) + 1) & 0xFF
                    data_cmd = (bytes([DFU_CMD_DATA, session]) + chunk +
                                bytes(5 - len(chunk)) + bytes([seq]))
                    while not set_feature(handle, data_cmd):
                        time.sleep(0.0002)
                    time.sleep(0.0024)
                    offset += len(chunk)

                # Await page commit ACK from RP2040:
                deadline = time.time() + 10.0
                last_progress = time.time()
                last_val = -1
                while time.time() < deadline:
                    if self._is_cancelled:
                        raise RuntimeError(tr("err_stop"))

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
                                self.sig_log.emit(tr(
                                    "log_resend", val=val, a=val, b=target_offset))
                                # Re-send exactly from the device count;
                                # sequences derive from the byte offset so
                                # re-sent chunks carry the expected next
                                # sequences — device drops duplicates.
                                p = val
                                while p < target_offset:
                                    chk = payload[p:p + CHUNK_SIZE]
                                    seq = ((p // CHUNK_SIZE) + 1) & 0xFF
                                    data_cmd = (bytes([DFU_CMD_DATA, session]) + chk +
                                                bytes(5 - len(chk)) + bytes([seq]))
                                    while not set_feature(handle, data_cmd):
                                        time.sleep(0.0002)
                                    time.sleep(0.0024)
                                    p += len(chk)
                                offset = p
                            if status in (DFU_STATUS_ERR_SIZE, DFU_STATUS_ERR_CRC,
                                          DFU_STATUS_ERR_FLASH, DFU_STATUS_ERR_TARGET,
                                          DFU_STATUS_ERR_PROTOCOL, DFU_STATUS_ERR_SESSION,
                                          DFU_STATUS_ERR_STATE, DFU_STATUS_ABORTED):
                                name = STATUS_NAMES.get(status, f"0x{status:02X}")
                                raise RuntimeError(tr("err_dev", name=name, detail=s_detail, val=val))
                    time.sleep(0.0024)
                else:
                    raise TimeoutError(tr("err_page_timeout", offset=target_offset))

                next_page += PAGE_SIZE
                pct = (offset * 100) // total_size
                elapsed = time.time() - t0
                speed = offset / elapsed if elapsed > 0 else 0
                self.sig_progress.emit(pct, offset, total_size, speed)

            self.sig_status.emit(tr("st_verify"))
            self.sig_log.emit("🔍 Verifying complete staging image...")

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
            self.sig_log.emit(tr("log_crc_ok", crc=verified_crc))

            self.sig_status.emit(tr("st_apply"))
            self.sig_log.emit(tr("log_activate"))

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
            self.sig_status.emit(tr("st_done"))
            self.sig_log.emit(tr("log_done"))
            self.sig_finished.emit(True, tr("ok_body_full"))

        except Exception as ex:
            err_msg = str(ex)
            self.sig_status.emit(tr("err_prefix", msg=err_msg))
            self.sig_log.emit(tr("log_error", msg=err_msg))
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
        self.setWindowTitle(tr("title"))
        self.setMinimumSize(660, 700)
        self.resize(680, 760)
        self.worker = None

        self.setup_ui()
        self.apply_dark_theme()

        # No default .wkota pre-selection: the user picks the package
        # explicitly (also avoids frozen-app CWD surprises).

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
        title = QLabel(tr("title"))
        title.setStyleSheet("font-size: 20px; font-weight: bold; color: #FFFFFF;")
        subtitle = QLabel(tr("subtitle"))
        subtitle.setStyleSheet("font-size: 12px; color: #8A99AD;")
        header_text.addWidget(title)
        header_text.addWidget(subtitle)
        header_layout.addLayout(header_text)
        header_layout.addStretch()

        # Language selector (EN default)
        self.cmb_lang = QComboBox()
        self.cmb_lang.addItems(["EN", "RU", "RO"])
        self.cmb_lang.setCurrentText("EN")
        self.cmb_lang.setFixedWidth(70)
        self.cmb_lang.currentTextChanged.connect(self.on_language_changed)
        header_layout.addWidget(QLabel(tr("lang_lbl")))
        header_layout.addWidget(self.cmb_lang)

        layout.addLayout(header_layout)

        # Card 1: File Selection
        card_file = QFrame()
        card_file.setObjectName("Card")
        file_layout = QVBoxLayout(card_file)
        file_layout.setContentsMargins(14, 12, 14, 12)
        file_layout.setSpacing(8)

        lbl_file_title = QLabel(tr("card_file"))
        lbl_file_title.setStyleSheet("font-weight: bold; font-size: 13px; color: #E1E7EF;")
        file_layout.addWidget(lbl_file_title)

        picker_layout = QHBoxLayout()
        self.txt_path = QLineEdit()
        self.txt_path.setPlaceholderText(tr("file_placeholder"))
        self.txt_path.setReadOnly(True)
        self.btn_browse = QPushButton(tr("btn_browse"))
        self.btn_browse.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_browse.clicked.connect(self.choose_file)
        picker_layout.addWidget(self.txt_path)
        picker_layout.addWidget(self.btn_browse)
        file_layout.addLayout(picker_layout)

        # Metadata badges
        self.lbl_meta = QLabel(tr("meta_none"))
        self.lbl_meta.setStyleSheet("font-size: 11px; color: #7F8C9D;")
        file_layout.addWidget(self.lbl_meta)
        layout.addWidget(card_file)

        # Card 2: Dongle Connection Status
        card_dongle = QFrame()
        card_dongle.setObjectName("Card")
        dongle_layout = QHBoxLayout(card_dongle)
        dongle_layout.setContentsMargins(14, 10, 14, 10)

        self.lbl_dongle_status = QLabel(tr("dongle_check"))
        self.lbl_dongle_status.setStyleSheet("font-size: 12px; color: #FFAA00; font-weight: 500;")
        dongle_layout.addWidget(self.lbl_dongle_status)
        dongle_layout.addStretch()

        self.btn_refresh_dongle = QPushButton(tr("btn_refresh"))
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

        lbl_prog_title = QLabel(tr("card_prog"))
        lbl_prog_title.setStyleSheet("font-weight: bold; font-size: 13px; color: #E1E7EF;")
        prog_layout.addWidget(lbl_prog_title)

        self.progress_bar = CustomPercentageProgressBar()
        prog_layout.addWidget(self.progress_bar)

        self.lbl_stats = QLabel(tr("lbl_stats"))
        self.lbl_stats.setStyleSheet("font-size: 12px; color: #00F5D4; font-weight: 500;")
        prog_layout.addWidget(self.lbl_stats)

        # Action Button
        self.btn_flash = QPushButton(tr("btn_flash"))
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

        lbl_log_title = QLabel(tr("log_title"))
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

    def on_language_changed(self, lang):
        global CURRENT_LANG
        CURRENT_LANG = lang
        self.setWindowTitle(tr("title"))
        self.txt_path.setPlaceholderText(tr("file_placeholder"))
        self.btn_browse.setText(tr("btn_browse"))
        self.btn_refresh_dongle.setText(tr("btn_refresh"))
        self.btn_flash.setText(tr("btn_flash"))
        if not self.txt_path.text():
            self.lbl_meta.setText(tr("meta_none"))
        self.check_dongle_status()

    def choose_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self,
            tr("dlg_open_title"),
            self.txt_path.text() or os.getcwd(),
            tr("dlg_open_filter")
        )
        if path:
            self.set_selected_file(path)

    def set_selected_file(self, path):
        try:
            info = parse_package_metadata(path)
            self.txt_path.setText(path)
            self.lbl_meta.setText(tr(
                "meta_ok", target=info['target'], board=info['board_id'],
                size=f"{info['size']:,}", crc=info['crc32']))
            self.lbl_meta.setStyleSheet("font-size: 11px; color: #00F5D4; font-weight: 500;")
            self.log(tr("file_selected_log", name=os.path.basename(path), crc=info['crc32']))
            self.update_flash_button_state()
        except Exception as ex:
            self.txt_path.setText(path)
            self.lbl_meta.setText(tr("meta_err", err=str(ex)))
            self.lbl_meta.setStyleSheet("font-size: 11px; color: #FF4D4F; font-weight: 500;")
            self.update_flash_button_state()

    def check_dongle_status(self):
        handle = open_receiver()
        if handle:
            k32.CloseHandle(handle)
            self.lbl_dongle_status.setText(tr("dongle_ok"))
            self.lbl_dongle_status.setStyleSheet("font-size: 12px; color: #00F5D4; font-weight: 500;")
            self.is_dongle_connected = True
        else:
            self.lbl_dongle_status.setText(tr("dongle_off"))
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
            QMessageBox.warning(self, tr("warn_title"), tr("warn_body"))
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
        self.lbl_stats.setText(tr(
            "prog_transfer", pct=pct, done=f"{transferred:,}",
            total=f"{total:,}", speed=f"{speed:,.1f}"))

    def on_worker_finished(self, success, message):
        self.btn_browse.setEnabled(True)
        self.update_flash_button_state()
        if success:
            self.progress_bar.setValue(100)
            self.lbl_stats.setText(tr("prog_done"))
            QMessageBox.information(self, tr("ok_title"), tr("ok_body", msg=message))
        else:
            QMessageBox.critical(self, tr("err_title"), tr("err_body", msg=message))


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
