# Wireless Keyboard Battery Tray

A lightweight, portable, native Win32 system tray application for Windows 10 and Windows 11 that reads live battery telemetry from the custom wireless keyboard's USB Receiver Dongle.

Built as a single standalone executable with **zero external dependencies** — requires no .NET Runtime, Python, Electron, HIDAPI DLL, background service, or administrator privileges.

---

## Key Features & UI Capabilities

- 🎨 **Modern Windows 11 / 10 UI**:
  - Immersive Dark Mode styling with DWM rounded corners (`DWMWA_WINDOW_CORNER_PREFERENCE`).
  - Native Per-Monitor v2 High-DPI awareness (`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`) for crisp text on 4K and multi-monitor setups.
- 🔋 **Comprehensive Telemetry Metrics**:
  - **Battery Percentage**: Real-time calculated capacity (0% to 100%).
  - **Cell Voltage**: Raw physical Li-Ion cell voltage with millivolt precision (e.g. `4.076 V`).
  - **Power State**: Real-time detection of *Discharging*, *Charging (CC/CV)*, *Full*, or *Idle*.
  - **Telemetry Liveness**: Live age tracking (e.g. `Telemetry: LIVE, age 2 s` or `STALE, age > 120 s`).
  - **Sequence Tracking**: Packet sequence counter for verifying continuous wireless delivery.
- 🚥 **Dynamic Visual Status Icons**:
  - 🟢 **Green**: Healthy battery ($\ge 50\%$).
  - 🟡 **Yellow**: Medium battery ($20\% - 49\%$).
  - 🔴 **Red**: Low battery ($< 20\%$).
  - 🔵 **Blue**: Actively charging.
  - ⚪ **Gray**: Keyboard offline / waiting for telemetry.
- ⚡ **Ultra-Low Resource Footprint**:
  - Efficient 5-second polling interval (`kPollIntervalMs = 5000`) via waitable coalescable timers.
  - Consumes **0% CPU** at idle and $< 2\text{ MB}$ private working memory.
  - Instant Plug-and-Play USB arrival/removal notifications (`WM_DEVICECHANGE`).

---

## Release Artifacts

Located in [`dist/`](dist/):

| Artifact | Type | Size | SHA-256 Checksum |
| :--- | :---: | :---: | :--- |
| [`dist/WirelessKeyboardTray.exe`](dist/WirelessKeyboardTray.exe) | Portable Executable | 256,000 bytes | `FC95A617EFC6302E4301D497A014A6AD586087750E2B94B2F181EE8EAA8A709A` |
| [`dist/WirelessKeyboardTray.zip`](dist/WirelessKeyboardTray.zip) | ZIP Archive | 110,111 bytes | `3F9117A6211362E1E35D4E0F0002742F83639747FAE8BAE6B50AF81E7749962C` |

---

## Portable Usage

1. Copy `WirelessKeyboardTray.exe` to any folder of your choice.
2. Double-click to launch. The battery icon will appear in the Windows System Notification Area (Tray).
3. **Hover** over the icon for a quick tooltip preview.
4. **Left-click or Right-click** to open the interactive detail popup:
   - **Refresh now**: Triggers an immediate hardware telemetry read.
   - **Start with Windows**: Toggles automatic startup on Windows login (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
   - **Exit**: Cleanly shuts down the tray process.

---

## Technical HID Contract

The application communicates directly with the Receiver Dongle using standard Windows HID API (`HidD_GetFeature`):

- **Target Device**: `VID 0x1B4F` / `PID 0x0001`
- **Target Collection**: Top-Level `Usage Page 0xFF00` / `Usage 0x0001`
- **Feature Report ID**: `3` (9-byte total buffer: 1 ID byte + 8 payload bytes)

### Report Structure:
```text
byte 0: Report ID = 0x03
byte 1: Battery percentage (0..100)
byte 2: Power state (0: Idle, 1: Charging, 2: Discharging, 3: Full, 4: Unknown)
byte 3: Millivolts low byte
byte 4: Millivolts high byte (uint16_t mV)
byte 5: Telemetry packet sequence counter
byte 6: Flags (bit 0: valid telemetry)
byte 7: Telemetry age in seconds (low byte)
byte 8: Telemetry age in seconds (high byte)
```

> [!NOTE]
> Telemetry is read directly from the Receiver Dongle's RAM cache. Querying the tray app emits no radio traffic, does not interrupt keyboard typing, and does not wake the keyboard from deep sleep.

---

## Build from Source

Requirements: MinGW-w64 (`g++` and `windres`).

From PowerShell:
```powershell
.\build.ps1
```

The script compiles a fully static Unicode Win32 binary linking only core Windows system libraries (`hid.dll`, `setupapi.dll`, `dwmapi.dll`, `uxtheme.dll`, `shell32.dll`, `user32.dll`, `gdi32.dll`, `advapi32.dll`).
