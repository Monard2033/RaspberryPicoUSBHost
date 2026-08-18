# Wireless battery telemetry handoff

Status: RP2040 -> Transmitter -> Receiver firmware transport/cache is
implemented on branch `codex/ultra-fast-input-reliability`. Receiver exposes a
vendor-defined HID Input/Feature report (ID `3`) for a no-driver reader. A
Windows HIDAPI tray executable, native Windows battery UI validation, and
hardware latency/radio-power validation remain TODO.

## Objective

Expose the RP2040 battery measurement through the wireless link and Receiver
without compromising keyboard latency, ordered multimedia transitions, or the
five-minute nRF52840 System OFF behavior.

The implemented path is:

```text
RP2040 GP28 ADC -> SPI -> nRF52840 Transmitter -> ESB -> Receiver cache
                                                        |-> standard HID battery usage, if Windows proves compatible
                                                        `-> vendor HID report -> Windows tray application
```

The RP2040 remains the measurement authority. The Receiver stores the latest
record. A Windows application reads only the Receiver cache, so frequent tray
polling must not create additional radio traffic.

## Required scheduling policy

RP2040 continues sampling locally once per second. Wireless telemetry is one
complete current-state packet, normally once every 30 seconds, and may be sent
only when all of the following are true:

```text
now - last_battery_tx_ms >= 30000
radio_power_state == RADIO_AWAKE
time since last changed HID input >= 50 ms
no Keyboard transition or retry is pending
no Consumer transition or retry is pending
no wake queue or urgent SPI/ESB work is pending
```

Recommended constants:

```c
#define BATTERY_TELEMETRY_PERIOD_MS 30000u
#define BATTERY_HID_QUIET_GUARD_MS     50u
```

When the 30-second deadline occurs during typing, keep one `battery_tx_pending`
flag and the latest measurement only. Send it in the first quiet window; never
replay missed intervals or queue historical samples. Start the next 30-second
period from the successful transmission time.

Battery telemetry is not user activity. It must never call
`radio_note_activity()`, update `radio_last_activity_ms`, wake a sleeping
Transmitter, or postpone the five-minute System OFF deadline.

Strict priority is:

```text
1. Keyboard press/release
2. Consumer Control press/release
3. Keyboard/Consumer reliability retry
4. System OFF and wake control
5. Battery telemetry
```

At the five-minute boundary, System OFF wins. Drop/defer any pending battery
send, retain the newest measurement locally, and send it only after the next
human-triggered wake and a 50 ms quiet window.

## Packet contract

The versioned implementation uses `LINK_TYPE_BATTERY = 0x04`. It keeps the
fixed 8-byte data area and uses integer values only:

```text
data[0]    percentage, 0..100
data[1]    battery state enum
data[2:3]  measured battery millivolts, little-endian
data[4]    monotonically incrementing battery sequence
data[5]    flags
data[6:7]  reserved, transmit as zero
```

Recommended state enum:

```c
BATTERY_STATE_IDLE = 0,
BATTERY_STATE_CHARGING = 1,
BATTERY_STATE_DISCHARGING = 2,
BATTERY_STATE_FULL = 3,
BATTERY_STATE_UNKNOWN = 4,
```

Recommended flags:

```text
bit 0: measurement valid
bit 1: material voltage step observed
bit 2: optional about-to-sleep indication
bit 3: external power suspected
bits 4..7: reserved
```

Use the RP2040 filtered battery millivolts and percentage already maintained by
the local battery task. Do not transmit floats or raw ADC samples. A sequence
change lets the Receiver distinguish a fresh update from an ESB duplicate.

This protocol extension is version `0x03`; RP2040, Transmitter and Receiver
must be built/flashed as a matched set. All three implementations reject
malformed percentage, state, length or reserved values.

## RP2040 implementation

The implementation is a non-blocking state machine; it does not send from the
ADC task and does not put sleep calls in TinyUSB callbacks:

```c
battery_sample_once_per_second();

if (now - last_battery_tx_ms >= BATTERY_TELEMETRY_PERIOD_MS) {
    battery_tx_pending = true;
}

if (battery_tx_pending &&
    radio_power_state == RADIO_AWAKE &&
    urgent_input_paths_idle() &&
    now - radio_last_activity_ms >= BATTERY_HID_QUIET_GUARD_MS) {
    queue_latest_battery_state();
}
```

The 30-second deadline keeps one pending latest state. The local SPI scheduler
marks it accepted only after it owns the one-slot Battery frame. Battery does
not use the Keyboard 250 us duplicate slot and cannot displace an ordered
Consumer edge.

## Transmitter implementation

Battery is implemented as low-priority latest-state telemetry, not as an input
transition, using a separate one-slot pending state rather than placing it in
front of Keyboard/Consumer frames in the urgent queue.

- Always drain urgent Keyboard/Consumer work first.
- Send Battery only when urgent work is empty.
- Use ESB acknowledgement, but do not enter an aggressive 8 ms retry loop.
- On failure, retain only the newest Battery state and retry in a later idle
  opportunity; input traffic may preempt it at any time.
- Battery success/failure does not alter `keyboard_delivery_pending` or held-key
  keepalive timing.
- A Battery frame must never be interpreted as activity that prevents System
  OFF.

## Receiver cache and USB exposure

The implementation does not insert Battery into the Keyboard/Consumer HID
transition queue. It validates the frame in the ESB handler and updates a
dedicated cache safely:

```text
percentage
millivolts
state
sequence
valid flag
Receiver uptime at last wireless update
optional sleeping/online state
```

The cache survives ordinary duplicate packets and remains readable while the
keyboard-side Transmitter is sleeping. USB reads return immediately from this
cache and never request a live radio transaction.

Investigate a standards-based USB HID battery usage first, but verify on the
target Windows version whether it is actually shown in native device UI.
Bluetooth-style battery display must not be assumed for arbitrary USB HID.

The reliable fallback is implemented as a vendor-defined HID Feature/Input
report with report ID `3`. It exposes percentage, state, millivolts, battery
sequence, flags and age in seconds. Receiver remains HID-only: UART, Serial,
CDC and a debug COM port stay disabled, so a reader requires no custom kernel
driver.

The Windows control-transfer buffer is nine bytes: byte `0` is report ID `3`
and bytes `1..8` are the cached Battery payload. Receiver must return all nine
bytes from its GET_REPORT callback; returning only the payload shifts every
field and makes a valid cached value appear malformed to `HidD_GetFeature`.

## Windows tray application

The portable implementation is in `tools/WirelessKeyboardTray`. It uses the
built-in Windows HID/SetupAPI interfaces directly, avoiding a HIDAPI DLL and
any external runtime. It:

- identifies VID/PID `1B4F:0001` and only vendor collection
  `Usage Page 0xFF00 / Usage 0x01`;
- reads the local Receiver cache once per 30 seconds, with a five-second timer
  tolerance, or immediately after a Plug-and-Play/user refresh event;
- displays percentage, voltage, charging state and cache age in its tooltip;
- uses changing tray icons and an optional per-user `Start with Windows` entry;
- distinguishes `LIVE`, `STALE`, waiting-for-telemetry and `OFFLINE`;
- performs no writes or radio wake requests merely to refresh the UI;
- uses no high-resolution timer, input hook, overlay, network connection,
  background service, custom driver or administrator permission.

Suggested meanings:

```text
LIVE      recent battery packet while keyboard radio is active
SLEEPING  last known value plus an explicit/derived System OFF state
STALE     no recent update and sleep was not confirmed
OFFLINE   Receiver has not observed the keyboard for a long policy interval
```

An optional best-effort `about-to-sleep` flag may be sent before System OFF,
but it must not delay sleep if it cannot be acknowledged. A cached value from
up to roughly 30 seconds before sleep is acceptable and should be displayed as
the last known value, not immediately treated as invalid.

## Implementation order

1. [x] Define/version `LINK_TYPE_BATTERY`, payload validation and shared constants.
2. [x] Add RP2040 pending/timing logic without touching the 1 kHz HID callback.
3. [x] Add low-priority Transmitter scheduling and ESB acknowledgement handling.
4. [x] Add Receiver cache and freshness tracking, separate from input queues.
5. [ ] Prototype the standard HID battery report and test native Windows behavior.
6. [x] Add the vendor-defined HID report as a stable application API.
7. [x] Build the portable native Windows tray application.
8. [ ] Build and flash all three firmware images as a matched set; measure input
   latency, RF traffic, sleep entry, wake behavior and current consumption.

## Acceptance criteria

- Battery is sampled locally every second and normally transmitted no more than
  once per 30 seconds while awake.
- No Battery transmission occurs during active/pending Keyboard or Consumer
  work, or within 50 ms of changed HID input.
- Continuous typing postpones telemetry and results in one latest-state packet,
  not a backlog.
- Battery traffic never resets the five-minute inactivity timer, wakes System
  OFF, delays sleep, or changes the four-blue-blink indication.
- Normal Keyboard remains capable of one changed USB input report per 1 ms and
  Consumer press/release ordering remains intact.
- Receiver cache remains readable while the keyboard sleeps and reports age/
  freshness correctly.
- Tray polling creates zero additional radio packets.
- Malformed or stale Battery frames cannot alter Keyboard/Consumer state.
- Hardware tests confirm negligible latency impact and no material autonomy
  regression; compile success alone is not hardware validation.

## Related reverse-channel implementation

Lock LED synchronization is implemented as a separate reverse-direction
protocol:

```text
Windows HID Output -> Receiver -> ESB ACK payload -> Transmitter
-> SPI MISO (P0.08 to RP2040 GP8) -> RP2040 HID SET_REPORT -> SONIX
```

The ACK carries LED bits, a validity flag and a Receiver boot epoch so an
8-bit sequence restart after reconnect/wake cannot be mistaken for an old
state.

It may share protocol-version work with Battery, but Battery remains a forward,
low-priority telemetry stream. Do not couple tray polling to the reverse lock
channel and do not let either feature interfere with urgent input delivery.
