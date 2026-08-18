#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dbt.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"WirelessKeyboardTrayHiddenWindow";
constexpr wchar_t kAppName[] = L"Wireless Keyboard Battery";
constexpr wchar_t kRunValueName[] = L"WirelessKeyboardTray";
constexpr wchar_t kSingleInstanceName[] =
    L"Local\\WirelessKeyboardTray_6C0B547D_406C_4AF7_A3CE_A28A17D15543";

constexpr USHORT kReceiverVid = 0x1B4F;
constexpr USHORT kReceiverPid = 0x0001;
constexpr USAGE kBatteryUsagePage = 0xFF00;
constexpr USAGE kBatteryUsage = 0x0001;
constexpr uint8_t kBatteryReportId = 3;
constexpr size_t kBatteryPayloadLength = 8;
constexpr size_t kBatteryReportLength = 1 + kBatteryPayloadLength;
constexpr DWORD kPollIntervalMs = 30000;
constexpr DWORD kTimerToleranceMs = 5000;
constexpr uint16_t kStaleAfterSeconds = 120;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kRefreshMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;

constexpr UINT kMenuRefresh = 1001;
constexpr UINT kMenuAutostart = 1002;
constexpr UINT kMenuExit = 1003;
constexpr LONG kTrayMenuLiftPx = 23;

enum class Availability {
    Offline,
    WaitingForTelemetry,
    Live,
    Stale,
    ReadError,
};

struct BatterySnapshot {
    Availability availability = Availability::Offline;
    uint8_t percentage = 0;
    uint8_t batteryState = 4;
    uint16_t millivolts = 0;
    uint8_t sequence = 0;
    uint8_t flags = 0;
    uint16_t ageSeconds = 0;
    DWORD error = ERROR_SUCCESS;
};

HINSTANCE gInstance = nullptr;
HWND gWindow = nullptr;
HANDLE gSingleInstance = nullptr;
HANDLE gStopEvent = nullptr;
HANDLE gRefreshEvent = nullptr;
HANDLE gPollTimer = nullptr;
HANDLE gWorkerThread = nullptr;
HDEVNOTIFY gDeviceNotification = nullptr;
NOTIFYICONDATAW gNotifyIcon{};
HICON gCurrentIcon = nullptr;
UINT gTaskbarCreatedMessage = 0;
volatile LONG gDeviceEpoch = 0;
BatterySnapshot gLastSnapshot{};

std::wstring GetExecutablePath()
{
    std::vector<wchar_t> buffer(512);

    for (;;) {
        DWORD const length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsAutostartEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG const result = RegQueryValueExW(key, kRunValueName, nullptr, &type,
                                         nullptr, &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ && bytes > sizeof(wchar_t);
}

bool SetAutostartEnabled(bool enabled)
{
    HKEY key = nullptr;
    LONG const openResult = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (openResult != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(openResult));
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        std::wstring const path = GetExecutablePath();
        if (path.empty()) {
            RegCloseKey(key);
            return false;
        }
        std::wstring const command = L"\"" + path + L"\" --startup";
        result = RegSetValueExW(
            key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<BYTE const *>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(result));
        return false;
    }
    return true;
}

uint32_t Argb(COLORREF color)
{
    return 0xFF000000U |
           (static_cast<uint32_t>(GetRValue(color)) << 16U) |
           (static_cast<uint32_t>(GetGValue(color)) << 8U) |
           static_cast<uint32_t>(GetBValue(color));
}

HICON CreateBatteryIcon(BatterySnapshot const &snapshot)
{
    constexpr int size = 16;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void *bits = nullptr;
    HDC const screen = GetDC(nullptr);
    HBITMAP const colorBitmap = CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO const *>(&header), DIB_RGB_COLORS,
        &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (colorBitmap == nullptr || bits == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    auto *pixels = static_cast<uint32_t *>(bits);
    std::fill(pixels, pixels + size * size, 0U);

    COLORREF color = RGB(130, 130, 130);
    if (snapshot.availability == Availability::Live ||
        snapshot.availability == Availability::Stale) {
        if (snapshot.batteryState == 1) {
            color = RGB(45, 170, 255);
        } else if (snapshot.percentage >= 50) {
            color = RGB(45, 200, 90);
        } else if (snapshot.percentage >= 20) {
            color = RGB(245, 175, 35);
        } else {
            color = RGB(235, 65, 65);
        }
    }
    uint32_t const pixel = Argb(color);

    auto putPixel = [pixels](int x, int y, uint32_t value) {
        if (x >= 0 && x < size && y >= 0 && y < size) {
            pixels[y * size + x] = value;
        }
    };

    for (int x = 1; x <= 12; ++x) {
        putPixel(x, 3, pixel);
        putPixel(x, 12, pixel);
    }
    for (int y = 3; y <= 12; ++y) {
        putPixel(1, y, pixel);
        putPixel(12, y, pixel);
    }
    for (int y = 6; y <= 9; ++y) {
        putPixel(13, y, pixel);
        putPixel(14, y, pixel);
    }

    if (snapshot.availability == Availability::Live ||
        snapshot.availability == Availability::Stale) {
        int const fillColumns = std::clamp(
            (static_cast<int>(snapshot.percentage) * 9 + 99) / 100, 0, 9);
        for (int x = 3; x < 3 + fillColumns; ++x) {
            for (int y = 5; y <= 10; ++y) {
                putPixel(x, y, pixel);
            }
        }
        if (snapshot.availability == Availability::Stale) {
            uint32_t const stalePixel = Argb(RGB(120, 120, 120));
            for (int i = 0; i < 7; ++i) {
                putPixel(4 + i, 5 + i, stalePixel);
            }
        }
    } else {
        for (int i = 0; i < 7; ++i) {
            putPixel(4 + i, 5 + i, pixel);
            putPixel(10 - i, 5 + i, pixel);
        }
    }

    std::array<BYTE, size * 2> maskBits{};
    HBITMAP const maskBitmap = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON const icon = CreateIconIndirect(&iconInfo);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

wchar_t const *BatteryStateName(uint8_t state)
{
    switch (state) {
    case 0:
        return L"Idle";
    case 1:
        return L"Charging";
    case 2:
        return L"Discharging";
    case 3:
        return L"Full";
    default:
        return L"Unknown";
    }
}

std::wstring BuildTooltip(BatterySnapshot const &snapshot)
{
    wchar_t text[128]{};
    switch (snapshot.availability) {
    case Availability::Offline:
        return L"Wireless Keyboard: Receiver offline";
    case Availability::WaitingForTelemetry:
        return L"Wireless Keyboard: waiting for battery telemetry";
    case Availability::ReadError:
        std::swprintf(text, std::size(text),
                      L"Wireless Keyboard: HID read error %lu",
                      static_cast<unsigned long>(snapshot.error));
        return text;
    case Availability::Live:
    case Availability::Stale:
        std::swprintf(
            text, std::size(text), L"Keyboard %u%% | %.3f V | %ls | %ls | age %us",
            static_cast<unsigned>(snapshot.percentage),
            static_cast<double>(snapshot.millivolts) / 1000.0,
            BatteryStateName(snapshot.batteryState),
            snapshot.availability == Availability::Live ? L"LIVE" : L"STALE",
            static_cast<unsigned>(snapshot.ageSeconds));
        return text;
    }
    return kAppName;
}

void CopyTooltip(std::wstring const &tooltip)
{
    std::wcsncpy(gNotifyIcon.szTip, tooltip.c_str(),
                 std::size(gNotifyIcon.szTip) - 1);
    gNotifyIcon.szTip[std::size(gNotifyIcon.szTip) - 1] = L'\0';
}

void UpdateTray(BatterySnapshot const &snapshot)
{
    HICON const nextIcon = CreateBatteryIcon(snapshot);
    HICON const previousIcon = gCurrentIcon;
    gCurrentIcon = nextIcon;
    gNotifyIcon.hIcon = gCurrentIcon;
    CopyTooltip(BuildTooltip(snapshot));
    gNotifyIcon.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &gNotifyIcon);

    if (previousIcon != nullptr && previousIcon != gCurrentIcon &&
        previousIcon != LoadIconW(nullptr, IDI_APPLICATION)) {
        DestroyIcon(previousIcon);
    }
    gLastSnapshot = snapshot;
}

void AddTrayIcon()
{
    if (gCurrentIcon != nullptr &&
        gCurrentIcon != LoadIconW(nullptr, IDI_APPLICATION)) {
        DestroyIcon(gCurrentIcon);
    }
    gNotifyIcon = {};
    gNotifyIcon.cbSize = sizeof(gNotifyIcon);
    gNotifyIcon.hWnd = gWindow;
    gNotifyIcon.uID = kTrayIconId;
    gNotifyIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    gNotifyIcon.uCallbackMessage = kTrayCallbackMessage;
    gCurrentIcon = CreateBatteryIcon(gLastSnapshot);
    gNotifyIcon.hIcon = gCurrentIcon;
    CopyTooltip(BuildTooltip(gLastSnapshot));
    Shell_NotifyIconW(NIM_ADD, &gNotifyIcon);

    gNotifyIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &gNotifyIcon);
}

void RemoveTrayIcon()
{
    if (gNotifyIcon.hWnd != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &gNotifyIcon);
        gNotifyIcon.hWnd = nullptr;
    }
    if (gCurrentIcon != nullptr &&
        gCurrentIcon != LoadIconW(nullptr, IDI_APPLICATION)) {
        DestroyIcon(gCurrentIcon);
    }
    gCurrentIcon = nullptr;
}

bool IsBatteryCollection(HANDLE handle)
{
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(handle, &attributes) ||
        attributes.VendorID != kReceiverVid ||
        attributes.ProductID != kReceiverPid) {
        return false;
    }

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(handle, &preparsed)) {
        return false;
    }

    HIDP_CAPS caps{};
    NTSTATUS const status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    return status == HIDP_STATUS_SUCCESS &&
           caps.UsagePage == kBatteryUsagePage &&
           caps.Usage == kBatteryUsage &&
           caps.FeatureReportByteLength >= kBatteryReportLength;
}

HANDLE OpenBatteryCollection()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO const devices = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index,
                                         &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredBytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0,
                                         &requiredBytes, nullptr);
        if (requiredBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<BYTE> detailBuffer(requiredBytes);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(
            detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(
                devices, &interfaceData, detail, requiredBytes, nullptr,
                nullptr)) {
            continue;
        }

        HANDLE inspect = CreateFileW(
            detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (inspect == INVALID_HANDLE_VALUE) {
            continue;
        }
        bool const matches = IsBatteryCollection(inspect);
        CloseHandle(inspect);
        if (!matches) {
            continue;
        }

        result = CreateFileW(
            detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (result == INVALID_HANDLE_VALUE) {
            result = CreateFileW(
                detail->DevicePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (result != INVALID_HANDLE_VALUE) {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

BatterySnapshot ReadBattery(HANDLE handle)
{
    BatterySnapshot snapshot{};
    std::array<uint8_t, kBatteryReportLength> report{};
    report[0] = kBatteryReportId;

    if (!HidD_GetFeature(handle, report.data(),
                         static_cast<ULONG>(report.size()))) {
        snapshot.availability = Availability::ReadError;
        snapshot.error = GetLastError();
        return snapshot;
    }

    if (report[1] > 100U || report[2] > 4U) {
        snapshot.availability = Availability::ReadError;
        snapshot.error = ERROR_INVALID_DATA;
        return snapshot;
    }

    snapshot.percentage = report[1];
    snapshot.batteryState = report[2];
    snapshot.millivolts = static_cast<uint16_t>(report[3]) |
                          (static_cast<uint16_t>(report[4]) << 8U);
    snapshot.sequence = report[5];
    snapshot.flags = report[6];
    snapshot.ageSeconds = static_cast<uint16_t>(report[7]) |
                          (static_cast<uint16_t>(report[8]) << 8U);

    if ((snapshot.flags & 0x01U) == 0) {
        snapshot.availability = Availability::WaitingForTelemetry;
    } else if (snapshot.ageSeconds > kStaleAfterSeconds) {
        snapshot.availability = Availability::Stale;
    } else {
        snapshot.availability = Availability::Live;
    }
    return snapshot;
}

void PostSnapshot(BatterySnapshot const &snapshot)
{
    auto *copy = new (std::nothrow) BatterySnapshot(snapshot);
    if (copy == nullptr) {
        return;
    }
    if (!PostMessageW(gWindow, kStatusMessage, 0,
                      reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

DWORD WINAPI WorkerMain(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    HANDLE battery = INVALID_HANDLE_VALUE;
    LONG observedEpoch = -1;
    HANDLE waits[] = {gStopEvent, gRefreshEvent, gPollTimer};

    for (;;) {
        DWORD const waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(waits)), waits, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult < WAIT_OBJECT_0 ||
            waitResult >= WAIT_OBJECT_0 + std::size(waits)) {
            continue;
        }

        LONG const currentEpoch = InterlockedCompareExchange(
            &gDeviceEpoch, 0, 0);
        if (currentEpoch != observedEpoch) {
            if (battery != INVALID_HANDLE_VALUE) {
                CloseHandle(battery);
                battery = INVALID_HANDLE_VALUE;
            }
            observedEpoch = currentEpoch;
        }

        if (battery == INVALID_HANDLE_VALUE) {
            battery = OpenBatteryCollection();
        }
        if (battery == INVALID_HANDLE_VALUE) {
            PostSnapshot(BatterySnapshot{});
            continue;
        }

        BatterySnapshot const snapshot = ReadBattery(battery);
        PostSnapshot(snapshot);
        if (snapshot.availability == Availability::ReadError) {
            CloseHandle(battery);
            battery = INVALID_HANDLE_VALUE;
        }
    }

    if (battery != INVALID_HANDLE_VALUE) {
        CloseHandle(battery);
    }
    return 0;
}

void RequestRefresh()
{
    if (gRefreshEvent != nullptr) {
        SetEvent(gRefreshEvent);
    }
}

void AppendBatteryDetails(HMENU menu, BatterySnapshot const &snapshot)
{
    constexpr UINT kDetailFlags = MF_STRING | MF_DISABLED;
    wchar_t text[128]{};

    AppendMenuW(menu, kDetailFlags, 0, L"Wireless Keyboard Battery");

    switch (snapshot.availability) {
    case Availability::Offline:
        AppendMenuW(menu, kDetailFlags, 0, L"Receiver: offline");
        break;
    case Availability::WaitingForTelemetry:
        AppendMenuW(menu, kDetailFlags, 0,
                    L"Waiting for battery telemetry...");
        break;
    case Availability::ReadError:
        std::swprintf(text, std::size(text), L"HID read error: %lu",
                      static_cast<unsigned long>(snapshot.error));
        AppendMenuW(menu, kDetailFlags, 0, text);
        break;
    case Availability::Live:
    case Availability::Stale:
        std::swprintf(text, std::size(text), L"Battery: %u%%",
                      static_cast<unsigned>(snapshot.percentage));
        AppendMenuW(menu, kDetailFlags, 0, text);
        std::swprintf(text, std::size(text), L"Voltage: %.3f V",
                      static_cast<double>(snapshot.millivolts) / 1000.0);
        AppendMenuW(menu, kDetailFlags, 0, text);
        std::swprintf(text, std::size(text), L"State: %ls",
                      BatteryStateName(snapshot.batteryState));
        AppendMenuW(menu, kDetailFlags, 0, text);
        std::swprintf(text, std::size(text), L"Telemetry: %ls, age %u s",
                      snapshot.availability == Availability::Live ?
                          L"LIVE" : L"STALE",
                      static_cast<unsigned>(snapshot.ageSeconds));
        AppendMenuW(menu, kDetailFlags, 0, text);
        std::swprintf(text, std::size(text), L"Sequence: %u",
                      static_cast<unsigned>(snapshot.sequence));
        AppendMenuW(menu, kDetailFlags, 0, text);
        break;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

void ShowTrayMenu()
{
    HMENU const menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendBatteryDetails(menu, gLastSnapshot);
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Refresh now");
    AppendMenuW(menu, MF_STRING |
                          (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    /* The notification overflow panel is topmost and can cover the lower
     * menu rows. Lift the anchor above the click so Exit is neither
     * hidden by the tray nor directly under the released mouse button. */
    cursor.y -= kTrayMenuLiftPx;
    SetForegroundWindow(gWindow);
    UINT const command = TrackPopupMenuEx(
        menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN |
                  TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x, cursor.y, gWindow, nullptr);
    PostMessageW(gWindow, WM_NULL, 0, 0);
    Shell_NotifyIconW(NIM_SETFOCUS, &gNotifyIcon);
    DestroyMenu(menu);
    if (command != 0) {
        SendMessageW(gWindow, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

void RegisterForHidNotifications()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = hidGuid;
    gDeviceNotification = RegisterDeviceNotificationW(
        gWindow, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void StopWorker()
{
    if (gStopEvent != nullptr) {
        SetEvent(gStopEvent);
    }
    if (gWorkerThread != nullptr) {
        WaitForSingleObject(gWorkerThread, 2000);
        CloseHandle(gWorkerThread);
        gWorkerThread = nullptr;
    }
    if (gPollTimer != nullptr) {
        CancelWaitableTimer(gPollTimer);
        CloseHandle(gPollTimer);
        gPollTimer = nullptr;
    }
    if (gRefreshEvent != nullptr) {
        CloseHandle(gRefreshEvent);
        gRefreshEvent = nullptr;
    }
    if (gStopEvent != nullptr) {
        CloseHandle(gStopEvent);
        gStopEvent = nullptr;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam)
{
    if (message == gTaskbarCreatedMessage && message != 0) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
    case kTrayCallbackMessage: {
        UINT const event = LOWORD(lParam);
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            ShowTrayMenu();
        } else if (event == NIN_SELECT || event == NIN_KEYSELECT ||
                   event == WM_LBUTTONUP) {
            RequestRefresh();
            ShowTrayMenu();
        }
        return 0;
    }
    case kStatusMessage: {
        auto *snapshot = reinterpret_cast<BatterySnapshot *>(lParam);
        if (snapshot != nullptr) {
            UpdateTray(*snapshot);
            delete snapshot;
        }
        return 0;
    }
    case kRefreshMessage:
        RequestRefresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuRefresh:
            RequestRefresh();
            return 0;
        case kMenuAutostart: {
            bool const enable = !IsAutostartEnabled();
            if (!SetAutostartEnabled(enable)) {
                MessageBoxW(window,
                            L"Windows could not update the current user's "
                            L"startup entry.",
                            kAppName, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case kMenuExit:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE ||
            wParam == DBT_DEVNODES_CHANGED) {
            InterlockedIncrement(&gDeviceEpoch);
            RequestRefresh();
        }
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_DESTROY:
        if (gDeviceNotification != nullptr) {
            UnregisterDeviceNotification(gDeviceNotification);
            gDeviceNotification = nullptr;
        }
        RemoveTrayIcon();
        StopWorker();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool StartWorker()
{
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gRefreshEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    gPollTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (gStopEvent == nullptr || gRefreshEvent == nullptr ||
        gPollTimer == nullptr) {
        return false;
    }

    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -static_cast<LONGLONG>(kPollIntervalMs) * 10000LL;
    if (!SetWaitableTimerEx(gPollTimer, &dueTime,
                            static_cast<LONG>(kPollIntervalMs), nullptr,
                            nullptr, nullptr, kTimerToleranceMs)) {
        return false;
    }

    gWorkerThread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    return gWorkerThread != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    gInstance = instance;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    gSingleInstance = CreateMutexW(nullptr, FALSE, kSingleInstanceName);
    if (gSingleInstance == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND const existing = FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, kRefreshMessage, 0, 0);
        }
        CloseHandle(gSingleInstance);
        return 0;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(gSingleInstance);
        return 2;
    }

    gWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, kAppName,
        WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, 1, 1, nullptr, nullptr,
        instance, nullptr);
    if (gWindow == nullptr) {
        CloseHandle(gSingleInstance);
        return 3;
    }

    gTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterForHidNotifications();
    AddTrayIcon();
    if (!StartWorker()) {
        MessageBoxW(gWindow, L"The low-priority battery worker could not start.",
                    kAppName, MB_OK | MB_ICONERROR);
        DestroyWindow(gWindow);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (gSingleInstance != nullptr) {
        CloseHandle(gSingleInstance);
    }
    return static_cast<int>(message.wParam);
}
