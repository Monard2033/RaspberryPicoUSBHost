# Wireless Keyboard Battery Tray

Minimal native Windows tray reader for the Receiver's cached Battery Feature
report. It is a single portable executable and requires no custom driver, .NET
runtime, HIDAPI DLL, installer, administrator rights, network connection, or
background service.

## Runtime behavior

- Identifies only USB HID `VID 0x1B4F / PID 0x0001` and top-level collection
  `Usage Page 0xFF00 / Usage 0x01`.
- Reads Feature report ID `3` (`ID byte + 8 payload bytes`) with the built-in
  Windows HID API.
- Never opens or reads the Keyboard and Consumer Control collections.
- Uses a hidden non-activating window for tray and Plug-and-Play messages.
- Uses a low-priority worker and a coalescable 30-second waitable timer with a
  5-second tolerance. There is no busy loop and no high-resolution timer.
- Refreshes immediately on Receiver arrival/removal and when the user chooses
  `Refresh now` or left-clicks the tray icon.
- Reads only Receiver RAM cache. A refresh emits no ESB packet, does not wake
  the keyboard, and does not postpone System OFF.
- Shows no automatic balloons, overlays, global hooks, Raw Input listeners, or
  focus-stealing windows. The detail/action popup appears only after user
  interaction.
- Anchors the context menu with its configured lift above the pointer and
  dispatches only the
  command explicitly returned by Windows, preventing an accidental Exit on the
  right-button release inside the notification overflow panel.
- Runs below normal process priority; the HID worker runs at the lowest thread
  priority.

The tooltip explicitly requests the Windows standard hover UI and shows
percentage, voltage, charge state, LIVE/STALE state, and the age of the last
wireless Battery packet. Left-click and right-click both open a native popup
that shows Battery percentage, voltage, charge state, telemetry state/age and
sequence above the action commands. The icon is green/yellow/red according to
level, blue while charging, and gray while offline or waiting for telemetry.

## Portable usage

1. Copy `dist\WirelessKeyboardTray.exe` to any writable folder.
2. Run it. No installation or elevation is required.
3. Hover for the compact status. Left-click or right-click for the complete
   Battery details and `Refresh now`, `Start with Windows`, or `Exit`.

`Start with Windows` adds one per-user value named `WirelessKeyboardTray` under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. Turning the option off
removes that value. If the portable EXE is moved, disable and re-enable startup
from its new location.

## Why USB cannot install it silently

The Receiver is a HID device and has no filesystem from which Windows could run
an application. Adding USB Mass Storage would consume firmware/USB resources
and still would not provide reliable AutoRun: current Windows security policy
often disables execution from removable media. Silent software deployment also
requires trust, signing, and/or administrator or organization policy. The safe
consumer workflow is a portable EXE copied once, optionally enabled at logon.

## Feature report contract

Windows passes a 9-byte buffer to `HidD_GetFeature`:

```text
byte 0  report ID = 3
byte 1  percentage, 0..100
byte 2  state: 0 idle, 1 charging, 2 discharging, 3 full, 4 unknown
byte 3  millivolts low byte
byte 4  millivolts high byte
byte 5  Battery sequence
byte 6  flags; bit 0 means valid
byte 7  cache age seconds low byte
byte 8  cache age seconds high byte
```

The app reports STALE after 120 seconds. Current firmware does not expose an
authoritative sleeping flag, so the app intentionally does not guess SLEEPING;
an old cache is labeled STALE rather than OFFLINE.

## Build

From PowerShell:

```powershell
.\build.ps1
```

The script builds a static Unicode GUI executable with MinGW-w64 and links only
Windows system libraries: HID, SetupAPI, Shell32, User32, GDI32, and Advapi32.
The committed `dist\WirelessKeyboardTray.exe` is the portable release artifact.

Current release:

```text
size      244224 bytes
SHA-256   03FFDA3D498A36DE80DBB9B95F7481700F9DB4BFC89AE598EA0A481EABAD66E1
```

Offline idle smoke test on Windows 11: zero additional CPU milliseconds during
a measured 10-second post-startup interval, about 2 MB private memory, one
active instance, below-normal process priority, and clean `WM_CLOSE` shutdown.
This is a software smoke test; a real Battery read remains a hardware test after
the corrected protocol `0x03` Receiver is flashed and re-enumerated.
