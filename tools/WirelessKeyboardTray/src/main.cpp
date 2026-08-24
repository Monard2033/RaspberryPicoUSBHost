#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <dwmapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"WirelessKeyboardTrayHiddenWindow";
constexpr wchar_t kPopupWindowClass[] = L"WirelessKeyboardTrayModernPopup";
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
constexpr DWORD kPollIntervalMs = 5000;
constexpr DWORD kTimerToleranceMs = 5000;
constexpr uint16_t kStaleAfterSeconds = 120;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kRefreshMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;

constexpr int kItemRefresh = 0;
constexpr int kItemAutostart = 1;
constexpr int kItemExit = 2;

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

struct PopupColors {
    COLORREF bg;
    COLORREF border;
    COLORREF title;
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF separator;
    COLORREF itemHoverBg;
    COLORREF itemPressedBg;
    COLORREF checkColor;
};

struct PopupLayout {
    int width = 236;
    int height = 0;
    RECT titleRect{};
    std::vector<std::pair<std::wstring, COLORREF>> telemetryLines;
    int sep1Y = 0;
    RECT refreshRect{};
    RECT autostartRect{};
    int sep2Y = 0;
    RECT exitRect{};
};

HINSTANCE gInstance = nullptr;
HWND gWindow = nullptr;
HWND gPopupWindow = nullptr;
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

bool gIsRefreshing = false;
int gHoverItem = -1;
int gPressedItem = -1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
void EnableHighDpiAwareness()
{
    HMODULE const user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        typedef BOOL(WINAPI * SetProcessDpiAwarenessContextProc)(
            DPI_AWARENESS_CONTEXT);
        auto const setDpiContext =
            reinterpret_cast<SetProcessDpiAwarenessContextProc>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setDpiContext != nullptr) {
            setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }

        typedef BOOL(WINAPI * SetProcessDPIAwareProc)();
        auto const setDpiAware = reinterpret_cast<SetProcessDPIAwareProc>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (setDpiAware != nullptr) {
            setDpiAware();
        }
    }
}

UINT GetWindowDpi(HWND window)
{
    if (window != nullptr) {
        HMODULE const user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            typedef UINT(WINAPI * GetDpiForWindowProc)(HWND);
            auto const getDpiForWindow =
                reinterpret_cast<GetDpiForWindowProc>(
                    GetProcAddress(user32, "GetDpiForWindow"));
            if (getDpiForWindow != nullptr) {
                UINT const dpi = getDpiForWindow(window);
                if (dpi != 0) {
                    return dpi;
                }
            }
        }
    }
    HDC const screen = GetDC(nullptr);
    int const dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}
#pragma GCC diagnostic pop

int ScaleDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

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

bool IsSystemDarkMode()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        DWORD type = REG_DWORD;
        if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value),
                             &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return value == 0;
        }
        RegCloseKey(key);
    }
    return false;
}

PopupColors GetCurrentColors()
{
    bool const isDark = IsSystemDarkMode();
    PopupColors c{};
    if (isDark) {
        c.bg = RGB(44, 44, 44);
        c.border = RGB(65, 65, 65);
        c.title = RGB(190, 190, 190);
        c.textPrimary = RGB(255, 255, 255);
        c.textSecondary = RGB(180, 180, 180);
        c.separator = RGB(60, 60, 60);
        c.itemHoverBg = RGB(58, 58, 58);
        c.itemPressedBg = RGB(70, 70, 70);
        c.checkColor = RGB(255, 255, 255);
    } else {
        c.bg = RGB(242, 242, 242);
        c.border = RGB(214, 214, 214);
        c.title = RGB(55, 55, 55);
        c.textPrimary = RGB(20, 20, 20);
        c.textSecondary = RGB(85, 85, 85);
        c.separator = RGB(222, 222, 222);
        c.itemHoverBg = RGB(226, 226, 226);
        c.itemPressedBg = RGB(212, 212, 212);
        c.checkColor = RGB(20, 20, 20);
    }
    return c;
}

struct Vec2 {
    float x, y;
};

inline float Length(Vec2 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}

inline float ClampFloat(float v, float minVal, float maxVal)
{
    return std::max(minVal, std::min(maxVal, v));
}

inline float SdfRoundedBox(Vec2 p, Vec2 b, float r)
{
    Vec2 q = {std::abs(p.x) - b.x + r, std::abs(p.y) - b.y + r};
    Vec2 m = {std::max(q.x, 0.0f), std::max(q.y, 0.0f)};
    return std::min(std::max(q.x, q.y), 0.0f) + Length(m) - r;
}

inline float SdfSegment(Vec2 p, Vec2 a, Vec2 b)
{
    Vec2 pa = {p.x - a.x, p.y - a.y};
    Vec2 ba = {b.x - a.x, b.y - a.y};
    float h = ClampFloat((pa.x * ba.x + pa.y * ba.y) / (ba.x * ba.x + ba.y * ba.y), 0.0f, 1.0f);
    Vec2 d = {pa.x - ba.x * h, pa.y - ba.y * h};
    return Length(d);
}

inline float SdfCircle(Vec2 p, float r)
{
    return Length(p) - r;
}

inline float SdfRing(Vec2 p, float r, float thickness)
{
    return std::abs(Length(p) - r) - thickness * 0.5f;
}

struct Color4f {
    float r, g, b, a;
};

inline uint32_t ToArgb(Color4f c)
{
    uint8_t a = static_cast<uint8_t>(ClampFloat(c.a * 255.0f + 0.5f, 0.0f, 255.0f));
    uint8_t r = static_cast<uint8_t>(ClampFloat(c.r * 255.0f + 0.5f, 0.0f, 255.0f));
    uint8_t g = static_cast<uint8_t>(ClampFloat(c.g * 255.0f + 0.5f, 0.0f, 255.0f));
    uint8_t b = static_cast<uint8_t>(ClampFloat(c.b * 255.0f + 0.5f, 0.0f, 255.0f));
    return (static_cast<uint32_t>(a) << 24U) |
           (static_cast<uint32_t>(r) << 16U) |
           (static_cast<uint32_t>(g) << 8U) |
           static_cast<uint32_t>(b);
}

HICON CreateBatteryIcon(BatterySnapshot const &snapshot)
{
    int const size = std::max(16, GetSystemMetrics(SM_CXSMICON));
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

    bool const isOffline = (snapshot.availability == Availability::Offline ||
                            snapshot.availability == Availability::WaitingForTelemetry ||
                            snapshot.availability == Availability::ReadError);
    bool const isStale = (snapshot.availability == Availability::Stale);
    bool const isCharging = (snapshot.availability == Availability::Live && snapshot.batteryState == 1);
    bool const isFull = (snapshot.availability == Availability::Live &&
                         (snapshot.batteryState == 3 || snapshot.percentage >= 100));

    Color4f mainColor;
    if (isOffline || isStale) {
        mainColor = {0.55f, 0.55f, 0.58f, 1.0f}; // Muted gray
    } else if (isCharging) {
        mainColor = {0.15f, 0.65f, 1.0f, 1.0f};  // Electric blue #26A6FF
    } else if (snapshot.percentage >= 50) {
        mainColor = {0.18f, 0.80f, 0.38f, 1.0f}; // Vibrant green #2ECC71
    } else if (snapshot.percentage >= 20) {
        mainColor = {1.0f, 0.60f, 0.0f, 1.0f};   // Amber / Orange #FF9900
    } else {
        mainColor = {1.0f, 0.25f, 0.22f, 1.0f};  // Alert Red #FF3F38
    }

    float const scale = static_cast<float>(size) / 16.0f;
    int const ss = 4; // 4x4 subpixel anti-aliasing

    Vec2 const bodyCenter = {8.0f, 9.0f};
    Vec2 const bodyHalfExtents = {5.7f, 6.4f};
    float const bodyRadius = 2.4f;

    float const capTop = 0.3f;
    float const capBottom = 2.6f;
    Vec2 const capCenter = {8.0f, (capTop + capBottom) * 0.5f};
    Vec2 const capHalfExtents = {2.4f, (capBottom - capTop) * 0.5f};
    float const capRadius = 0.7f;

    Vec2 const innerCenter = {8.0f, 9.0f};
    Vec2 const innerHalfExtents = {4.5f, 5.2f};
    float const innerRadius = 1.5f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float accumR = 0.0f;
            float accumG = 0.0f;
            float accumB = 0.0f;
            float accumA = 0.0f;

            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    float const px = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / ss) / scale;
                    float const py = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / ss) / scale;

                    Vec2 const bp = {px - bodyCenter.x, py - bodyCenter.y};
                    Vec2 const cp = {px - capCenter.x, py - capCenter.y};

                    float const dBody = SdfRoundedBox(bp, bodyHalfExtents, bodyRadius);
                    float const dCap = SdfRoundedBox(cp, capHalfExtents, capRadius);
                    float const dShellOuter = std::min(dBody, dCap);

                    Vec2 const ip = {px - innerCenter.x, py - innerCenter.y};
                    float const dInner = SdfRoundedBox(ip, innerHalfExtents, innerRadius);

                    bool const isBorder = (dShellOuter <= 0.0f) && (dInner >= 0.0f);

                    float const fillFrac = ClampFloat(static_cast<float>(snapshot.percentage) / 100.0f, 0.0f, 1.0f);
                    float const fillTopY = 14.2f - fillFrac * 10.4f;

                    bool isFilled = false;
                    if (!isOffline && !isStale) {
                        if (dInner <= 0.0f && (isFull || py >= fillTopY)) {
                            isFilled = true;
                        }
                    }

                    bool isSymbol = false;
                    Color4f symbolCol = {1.0f, 1.0f, 1.0f, 1.0f};

                    if (isCharging) {
                        float const lx = px - 8.0f;
                        float const ly = py - 8.9f;
                        Vec2 poly[6] = {
                            {0.7f, -4.5f},
                            {-2.4f, 0.35f},
                            {-0.35f, 0.35f},
                            {-0.7f, 4.5f},
                            {2.4f, -0.35f},
                            {0.35f, -0.35f}
                        };
                        bool inside = false;
                        for (int i = 0, j = 5; i < 6; j = i++) {
                            if (((poly[i].y > ly) != (poly[j].y > ly)) &&
                                (lx < (poly[j].x - poly[i].x) * (ly - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) {
                                inside = !inside;
                            }
                        }
                        if (inside) {
                            isSymbol = true;
                            symbolCol = {1.0f, 1.0f, 1.0f, 1.0f};
                        }
                    } else if (isStale) {
                        Vec2 const clockP = {px - 8.0f, py - 8.9f};
                        float const ring = SdfRing(clockP, 2.9f, 0.95f);
                        float const hand1 = SdfSegment(clockP, {0.0f, 0.0f}, {0.0f, -1.9f}) - 0.50f;
                        float const hand2 = SdfSegment(clockP, {0.0f, 0.0f}, {1.7f, 0.0f}) - 0.50f;
                        float const clockDist = std::min(ring, std::min(hand1, hand2));
                        if (clockDist <= 0.0f) {
                            isSymbol = true;
                            symbolCol = {0.90f, 0.90f, 0.93f, 1.0f};
                        }
                    } else if (isOffline) {
                        Vec2 const exP = {px - 8.0f, py - 8.9f};
                        float const bar = SdfSegment(exP, {0.0f, -2.8f}, {0.0f, 0.7f}) - 0.60f;
                        float const dot = SdfCircle({exP.x, exP.y - 2.6f}, 0.60f);
                        if (bar <= 0.0f || dot <= 0.0f) {
                            isSymbol = true;
                            symbolCol = {0.85f, 0.85f, 0.88f, 1.0f};
                        }
                    }

                    Color4f sampleColor = {0.0f, 0.0f, 0.0f, 0.0f};
                    if (isSymbol) {
                        sampleColor = symbolCol;
                    } else if (isFilled) {
                        sampleColor = mainColor;
                    } else if (isBorder) {
                        sampleColor = mainColor;
                    }

                    accumR += sampleColor.r * sampleColor.a;
                    accumG += sampleColor.g * sampleColor.a;
                    accumB += sampleColor.b * sampleColor.a;
                    accumA += sampleColor.a;
                }
            }

            float const total = static_cast<float>(ss * ss);
            float const a = accumA / total;
            if (a > 0.001f) {
                Color4f const finalCol = {accumR / accumA, accumG / accumA, accumB / accumA, a};
                pixels[y * size + x] = ToArgb(finalCol);
            }
        }
    }

    std::vector<BYTE> maskBits(size * ((size + 31) / 32 * 4), 0);
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
    case 2:
        return L"Discharging";
    case 1:
        return L"Charging";
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

PopupLayout ComputeLayout(BatterySnapshot const &snapshot,
                         PopupColors const &colors, UINT dpi)
{
    PopupLayout l{};
    l.width = ScaleDpi(236, dpi);
    int y = ScaleDpi(10, dpi);
    int const sidePad = ScaleDpi(16, dpi);
    int const lineHeight = ScaleDpi(20, dpi);
    int const itemHeight = ScaleDpi(30, dpi);
    int const sideMargin = ScaleDpi(5, dpi);

    l.titleRect = {sidePad, y, l.width - sidePad, y + ScaleDpi(18, dpi)};
    y += ScaleDpi(20, dpi);

    wchar_t text[128]{};
    switch (snapshot.availability) {
    case Availability::Offline:
        l.telemetryLines.push_back({L"Receiver: offline", colors.textPrimary});
        break;
    case Availability::WaitingForTelemetry:
        l.telemetryLines.push_back(
            {L"Waiting for battery telemetry...", colors.textPrimary});
        break;
    case Availability::ReadError:
        std::swprintf(text, std::size(text), L"HID read error: %lu",
                      static_cast<unsigned long>(snapshot.error));
        l.telemetryLines.push_back({text, colors.textPrimary});
        break;
    case Availability::Live:
    case Availability::Stale:
        std::swprintf(text, std::size(text), L"Battery: %u%%",
                      static_cast<unsigned>(snapshot.percentage));
        l.telemetryLines.push_back({text, colors.textPrimary});
        std::swprintf(text, std::size(text), L"Voltage: %.3f V",
                      static_cast<double>(snapshot.millivolts) / 1000.0);
        l.telemetryLines.push_back({text, colors.textPrimary});
        std::swprintf(text, std::size(text), L"State: %ls",
                      BatteryStateName(snapshot.batteryState));
        l.telemetryLines.push_back({text, colors.textPrimary});
        std::swprintf(text, std::size(text), L"Telemetry: %ls, age %u s",
                      snapshot.availability == Availability::Live ?
                          L"LIVE" : L"STALE",
                      static_cast<unsigned>(snapshot.ageSeconds));
        l.telemetryLines.push_back({text, colors.textSecondary});
        std::swprintf(text, std::size(text), L"Sequence: %u",
                      static_cast<unsigned>(snapshot.sequence));
        l.telemetryLines.push_back({text, colors.textSecondary});
        break;
    }

    y += static_cast<int>(l.telemetryLines.size()) * lineHeight +
         ScaleDpi(6, dpi);

    l.sep1Y = y;
    y += ScaleDpi(6, dpi);

    l.refreshRect = {sideMargin, y, l.width - sideMargin, y + itemHeight};
    y += itemHeight + ScaleDpi(2, dpi);

    l.autostartRect = {sideMargin, y, l.width - sideMargin, y + itemHeight};
    y += itemHeight + ScaleDpi(4, dpi);

    l.sep2Y = y;
    y += ScaleDpi(5, dpi);

    l.exitRect = {sideMargin, y, l.width - sideMargin, y + itemHeight};
    y += itemHeight + ScaleDpi(6, dpi);

    l.height = y;
    return l;
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

    gLastSnapshot = snapshot;
    gIsRefreshing = false;

    if (gPopupWindow != nullptr && IsWindowVisible(gPopupWindow)) {
        UINT const dpi = GetWindowDpi(gPopupWindow);
        PopupColors const colors = GetCurrentColors();
        PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);
        RECT currentRect{};
        GetWindowRect(gPopupWindow, &currentRect);
        int const diff = layout.height - (currentRect.bottom - currentRect.top);
        int const newY = currentRect.top - diff;
        SetWindowPos(gPopupWindow, HWND_TOPMOST, currentRect.left, newY,
                     layout.width, layout.height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        InvalidateRect(gPopupWindow, nullptr, TRUE);
        UpdateWindow(gPopupWindow);
    }

    if (previousIcon != nullptr && previousIcon != gCurrentIcon &&
        previousIcon != LoadIconW(nullptr, IDI_APPLICATION)) {
        DestroyIcon(previousIcon);
    }
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

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // User clicked Refresh now: trigger immediate on-demand battery poll
            std::array<uint8_t, kBatteryReportLength> pollReq{};
            pollReq[0] = kBatteryReportId;
            pollReq[1] = 0x01;
            HidD_SetFeature(battery, pollReq.data(), static_cast<ULONG>(pollReq.size()));
            Sleep(150);
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

void ShowModernPopup()
{
    UINT const dpi = GetWindowDpi(gPopupWindow != nullptr ? gPopupWindow : gWindow);
    PopupColors const colors = GetCurrentColors();
    PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);

    if (gPopupWindow == nullptr) {
        gPopupWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupWindowClass, kAppName,
            WS_POPUP, 0, 0, layout.width, layout.height, gWindow,
            nullptr, gInstance, nullptr);
        if (gPopupWindow == nullptr) return;

        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(gPopupWindow, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &corner, sizeof(corner));
    }

    BOOL isDark = IsSystemDarkMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(gPopupWindow, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &isDark, sizeof(isDark));

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR const monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    RECT const work = mi.rcWork;

    LONG x = std::clamp(cursor.x - layout.width / 2, work.left,
                        work.right - layout.width);
    LONG y = cursor.y - ScaleDpi(kTrayMenuLiftPx, dpi) - layout.height;
    y = std::clamp(y, work.top, work.bottom - layout.height);

    SetWindowPos(gPopupWindow, HWND_TOPMOST, x, y, layout.width, layout.height,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(gPopupWindow);
    SetFocus(gPopupWindow);
    InvalidateRect(gPopupWindow, nullptr, TRUE);
}

LRESULT CALLBACK PopupWindowProcedure(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        return 0;

    case WM_MOUSEMOVE: {
        int const x = GET_X_LPARAM(lParam);
        int const y = GET_Y_LPARAM(lParam);
        UINT const dpi = GetWindowDpi(window);
        PopupColors const colors = GetCurrentColors();
        PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);

        int hovered = -1;
        POINT pt = {x, y};
        if (PtInRect(&layout.refreshRect, pt)) hovered = kItemRefresh;
        else if (PtInRect(&layout.autostartRect, pt)) hovered = kItemAutostart;
        else if (PtInRect(&layout.exitRect, pt)) hovered = kItemExit;

        if (hovered != gHoverItem) {
            gHoverItem = hovered;
            InvalidateRect(window, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = window;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (gHoverItem != -1 || gPressedItem != -1) {
            gHoverItem = -1;
            gPressedItem = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int const x = GET_X_LPARAM(lParam);
        int const y = GET_Y_LPARAM(lParam);
        UINT const dpi = GetWindowDpi(window);
        PopupColors const colors = GetCurrentColors();
        PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);

        POINT pt = {x, y};
        if (PtInRect(&layout.refreshRect, pt)) gPressedItem = kItemRefresh;
        else if (PtInRect(&layout.autostartRect, pt)) gPressedItem = kItemAutostart;
        else if (PtInRect(&layout.exitRect, pt)) gPressedItem = kItemExit;
        else gPressedItem = -1;

        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        int const x = GET_X_LPARAM(lParam);
        int const y = GET_Y_LPARAM(lParam);
        UINT const dpi = GetWindowDpi(window);
        PopupColors const colors = GetCurrentColors();
        PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);

        int releasedItem = -1;
        POINT pt = {x, y};
        if (PtInRect(&layout.refreshRect, pt)) releasedItem = kItemRefresh;
        else if (PtInRect(&layout.autostartRect, pt)) releasedItem = kItemAutostart;
        else if (PtInRect(&layout.exitRect, pt)) releasedItem = kItemExit;

        if (releasedItem == gPressedItem && releasedItem != -1) {
            switch (releasedItem) {
            case kItemRefresh:
                gIsRefreshing = true;
                RequestRefresh();
                InvalidateRect(window, nullptr, FALSE);
                break;
            case kItemAutostart:
                SetAutostartEnabled(!IsAutostartEnabled());
                InvalidateRect(window, nullptr, FALSE);
                break;
            case kItemExit:
                ShowWindow(window, SW_HIDE);
                DestroyWindow(gWindow);
                break;
            }
        }
        gPressedItem = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(window, &ps);
        RECT rc;
        GetClientRect(window, &rc);

        UINT const dpi = GetWindowDpi(window);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        PopupColors const colors = GetCurrentColors();
        PopupLayout const layout = ComputeLayout(gLastSnapshot, colors, dpi);

        HBRUSH bgBrush = CreateSolidBrush(colors.bg);
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, colors.border);
        HGDIOBJ oldPen = SelectObject(memDC, borderPen);
        HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(memDC, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(borderPen);

        int const fontTitleHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        int const fontRegularHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        int const fontCheckHeight = -MulDiv(10, static_cast<int>(dpi), 72);

        HFONT fontTitle = CreateFontW(fontTitleHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT fontRegular = CreateFontW(fontRegularHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT fontCheck = CreateFontW(fontCheckHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        SetBkMode(memDC, TRANSPARENT);

        HGDIOBJ oldFont = SelectObject(memDC, fontTitle);
        SetTextColor(memDC, colors.title);
        RECT tr = layout.titleRect;
        DrawTextW(memDC, kAppName, -1, &tr,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        SelectObject(memDC, fontRegular);
        int const lineHeight = ScaleDpi(20, dpi);
        int const sidePad = ScaleDpi(16, dpi);
        int lineY = layout.titleRect.bottom + ScaleDpi(2, dpi);
        for (auto const &line : layout.telemetryLines) {
            RECT lr = {sidePad, lineY, layout.width - sidePad, lineY + lineHeight};
            SetTextColor(memDC, line.second);
            DrawTextW(memDC, line.first.c_str(), -1, &lr,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            lineY += lineHeight;
        }

        int const sepMargin = ScaleDpi(10, dpi);
        auto drawSep = [&](int sepY) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, colors.separator);
            HGDIOBJ prevPen = SelectObject(memDC, sepPen);
            MoveToEx(memDC, sepMargin, sepY, nullptr);
            LineTo(memDC, layout.width - sepMargin, sepY);
            SelectObject(memDC, prevPen);
            DeleteObject(sepPen);
        };

        drawSep(layout.sep1Y);

        int const cornerRadius = ScaleDpi(6, dpi);
        int const checkLeft = ScaleDpi(8, dpi);
        int const checkWidth = ScaleDpi(16, dpi);
        int const textLeft = ScaleDpi(28, dpi);
        int const textRightPad = ScaleDpi(8, dpi);

        auto drawItem = [&](int itemId, RECT const &itemRect,
                            wchar_t const *label, bool hasCheck,
                            bool isChecked) {
            bool const isHover = (gHoverItem == itemId);
            bool const isPress = (gPressedItem == itemId);

            if (isHover || isPress) {
                COLORREF itemBg = isPress ? colors.itemPressedBg :
                                            colors.itemHoverBg;
                HBRUSH itemBrush = CreateSolidBrush(itemBg);
                HPEN itemPen = CreatePen(PS_NULL, 0, itemBg);
                HGDIOBJ pPen = SelectObject(memDC, itemPen);
                HGDIOBJ pBrush = SelectObject(memDC, itemBrush);
                RoundRect(memDC, itemRect.left, itemRect.top, itemRect.right,
                          itemRect.bottom, cornerRadius, cornerRadius);
                SelectObject(memDC, pBrush);
                SelectObject(memDC, pPen);
                DeleteObject(itemPen);
                DeleteObject(itemBrush);
            }

            if (hasCheck && isChecked) {
                SelectObject(memDC, fontCheck);
                SetTextColor(memDC, colors.checkColor);
                RECT cr = {itemRect.left + checkLeft, itemRect.top,
                           itemRect.left + checkLeft + checkWidth,
                           itemRect.bottom};
                DrawTextW(memDC, L"\x2713", -1, &cr,
                          DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }

            SelectObject(memDC, fontRegular);
            SetTextColor(memDC, colors.textPrimary);
            RECT lr = {itemRect.left + textLeft, itemRect.top,
                       itemRect.right - textRightPad, itemRect.bottom};
            DrawTextW(memDC, label, -1, &lr,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        };

        drawItem(kItemRefresh, layout.refreshRect,
                 gIsRefreshing ? L"Refreshing..." : L"Refresh now", false,
                 false);
        drawItem(kItemAutostart, layout.autostartRect, L"Start with Windows",
                 true, IsAutostartEnabled());

        drawSep(layout.sep2Y);

        drawItem(kItemExit, layout.exitRect, L"Exit", false, false);

        SelectObject(memDC, oldFont);
        DeleteObject(fontTitle);
        DeleteObject(fontRegular);
        DeleteObject(fontCheck);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(window, &ps);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            ShowWindow(window, SW_HIDE);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(window, SW_HIDE);
        }
        return 0;

    case WM_DESTROY:
        gPopupWindow = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
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
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP ||
            event == NIN_SELECT || event == NIN_KEYSELECT ||
            event == WM_LBUTTONUP) {
            ShowModernPopup();
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
        case kItemRefresh:
            RequestRefresh();
            return 0;
        case kItemAutostart: {
            bool const enable = !IsAutostartEnabled();
            if (!SetAutostartEnabled(enable)) {
                MessageBoxW(window,
                            L"Windows could not update the current user's "
                            L"startup entry.",
                            kAppName, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case kItemExit:
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
    EnableHighDpiAwareness();

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

    WNDCLASSEXW popupClass{};
    popupClass.cbSize = sizeof(popupClass);
    popupClass.hInstance = instance;
    popupClass.lpfnWndProc = PopupWindowProcedure;
    popupClass.lpszClassName = kPopupWindowClass;
    popupClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    popupClass.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&popupClass)) {
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
