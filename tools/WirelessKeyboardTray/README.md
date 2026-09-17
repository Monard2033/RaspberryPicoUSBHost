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
  - **Discharge Countdown (ETA)**: Estimated time left until 0 %, fitted on the
    millivolt decay over a sliding window (median-filtered, block-median endpoints).
    Shown in the tooltip and popup while the pack is discharging, e.g.
    `3 h 25 min left (est.)`, plus the measured rate in `%/h` and `mV/h`.
    See *Discharge ETA* below for the limits.
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
| [`dist/WirelessKeyboardTray.exe`](dist/WirelessKeyboardTray.exe) | Portable Executable | 221,696 bytes | `18FEF353E08C90157719EB2FAC0E0E7C726684F9BDDBD9778F15F18BF395FEB1` |
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

## Discharge ETA (time left until 0 %)

The popup and tooltip show an estimated time-to-empty while the pack is discharging.
It is computed **entirely inside the tray process** from the existing cache fields; no
firmware change, no radio traffic and no extra HID reads are involved.

### Algorithm

1. Every poll (5 s) a `(timestamp, percentage, millivolts)` sample is appended to a
   ring buffer (~85 min of history) **only** when the telemetry is `LIVE` and the
   reported state is `Discharging`.
2. The rate is fitted on the **millivolt decay**, not on the percentage: 1 % of the
   firmware scale is only `kMvPerPercent = 11.4 mV`, so the millivolt signal has
   ~11x finer resolution. The window spans up to the last 60 min.
3. Voltage is a *state*, not a flow, so the slope is measured against artefacts
   rather than from raw endpoints:
   - a rolling **median filter** (5 samples, 25 s) rejects the short sag/spike a
     keypress, radio burst or LED pulse puts on the cell;
   - the rate is the difference between the **median of the oldest block** and the
     **median of the newest block** (4 samples each), over the time between the two
     block centres. This is what stops a single load step from being read as a huge
     fake discharge rate.
4. The last 9 rate readings are reduced with a **median**, and the countdown is
   extrapolated from the *median* of the newest block — not from the raw
   instantaneous reading — down to the firmware's 0 % reference
   (`BATT_MIN_MV = 3050 mV`).

A rate is only reported once the window spans at least 10 min and the fitted decay is
at least 10 mV (~0.9 %), so the first estimate appears after roughly 10-15 min of real
discharging rather than immediately.

History is discarded on a reported charge cycle, on a sampling gap longer than 10 min
(PC or keyboard asleep — a charge cycle may fit inside), and when the measured voltage
has risen more than 25 mV above the stored median, which catches a charge cycle that
happened while the app was not sampling. Shorter gaps are kept: the timestamps preserve
the real elapsed time, and the cell keeps discharging across them. A stale/offline
Receiver freezes the last estimate for display.

### Honest limits

> [!IMPORTANT]
> The countdown is an **estimate**, not a measurement. The firmware derives the
> percentage from a 1 mV-resolution voltage divider and no current sensor exists in
> the hardware, so real consumed capacity (mAh) is not available. On the flat part of
> the Li-Ion curve a 1 % step may mean very different amounts of time.
>
> Practical consequences:
> - Nothing is shown (popup prints `Time left: estimating...`) until ~10-15 min of
>   real discharging has accumulated.
> - Countdown is hidden while charging or full, and during the 8 s boot indication.
> - Accuracy is best in the 90 %..20 % band; the last few percent and the
>   voltage-fallback path are the least reliable.
> - The history lives in RAM only: restarting the app (or rebooting) restarts the
>   estimate from scratch.

---

## Build from Source

Requirements: MinGW-w64 (`g++` and `windres`).

From PowerShell:
```powershell
.\build.ps1
```

An alternative compiler path that needs no system-wide install is the `ziglang`
Python wheel (clang + bundled mingw-w64):

```powershell
pip install --target .buildtools ziglang
.buildtools\ziglang\zig.exe c++ -std=c++17 -Os -s `
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE `
  -target x86_64-windows-gnu -municode -static -static-libgcc -static-libstdc++ `
  -Wl,--gc-sections -Wl,--subsystem,windows -o build\WirelessKeyboardTray.exe `
  src\main.cpp dist\resources.o `
  -lhid -lsetupapi -lshell32 -luser32 -lgdi32 -ladvapi32 -ldwmapi -luxtheme
```

`verify_estimator.py` replicates the ETA estimator in Python and exercises it with
synthetic discharge profiles (flat 6 h pack, heavy 90 min load, quantisation noise,
charge-cycle reset, sampling-gap reset). It validates the algorithm only; it does not
compile the C++.

The script compiles a fully static Unicode Win32 binary linking only core Windows system libraries (`hid.dll`, `setupapi.dll`, `dwmapi.dll`, `uxtheme.dll`, `shell32.dll`, `user32.dll`, `gdi32.dll`, `advapi32.dll`).
