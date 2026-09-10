---
name: "sonix-led-quiesce-sync"
description: "Sync SN32 keyboard LEDs via SET_REPORT on a quiesced RP2040 PIO-USB host bus without freezing 1000 Hz input; use for LED sync work or SN32 input freezes."
---

# Sonix LED Sync (SHELVED)

Status: the quiesced-SET_REPORT LED sync was implemented per a former version of this skill, live-tested, and REVERTED by the operator (2026-09-10). Do not re-implement it without first reproducing the failure in isolation. Trigger phrases: "beculețul fizic", "LED sync", "SET_REPORT freeze", "SN32 not responding", "sync numlock with Windows".

## Proven constraints (all live-tested on the SN32 + RP2040 PIO-USB host)

- The SN32 has no interrupt-OUT endpoint; SET_REPORT(Output) on EP0 is the only USB channel that changes its physical LEDs. A correctly parsed LED output report DOES light the LEDs — addressing is not the problem.
- The Windows lock state already arrives at the RP2040 through the radio reverse-ACK path (`spi_process_ack`), so knowing the state needs no USB traffic; only rendering it on the physical LEDs needs SET_REPORT.
- **Wedge rule (empirical): after any SET_REPORT — even seconds away from typing — the keyboard accepts <= 3 simultaneous keys but freezes on >= 4-key rollover bursts.** Bus quiescing does not prevent it: a variant that held off EP 0x81 for a full report interval before every SET_REPORT still froze at >= 4 keys. The SN32 exits every control transfer with EP 0x81 in a corrupted state (suspected data-toggle desync); an explicit host-side `pio_usb_host_endpoint_reset_toggle` after completion did NOT cure it.
- **One-shot at boot does not dodge the wedge either (2026-09-10, third variant):** a single SET_REPORT(NumLock ON) fired 300 ms after mount, gated on all-keys-released + 250 ms quiet window + host-side EP1 toggle re-alignment after completion, was OTA-installed and still wedged EP1 on >= 4 simultaneous keys. Conclusion: SET_REPORT(Output) poisons the SN32's interrupt-IN pipe regardless of timing, gating, or toggle re-alignment — one-shot "boot kick" included.
- Deferring the initial arming of EP 0x81 (boot SET_REPORT handshake before the first IN poll) wedges the SN32 harder; only a physical power-cycle of the keyboard recovers it. Never delay the first arm of the interrupt-IN endpoint.
- SN32 wedges look identical to firmware bugs: always power-cycle the keyboard before re-testing, and expect a wedged keyboard to also silence the radio path (it feeds the radio chain), which masquerades as a broken OTA link.

## If LED sync is attempted again (open TODO, uncommitted experiments removed)

1. Reproduce the wedge in isolation first: flash a minimal build that sends exactly one SET_REPORT at idle, then test >= 4-key rollover minutes later. If it wedges (expected per current evidence), no sync policy on the RP2040 can fix it — the fix would have to address the SN32's EP1 state after control transfers (e.g. SET_PROTOCOL/SET_IDLE probes, bus reset via port power toggle, or accepting a partially-off EP0 policy).

## LED "stopped working" diagnosis (2026-09-10, post-revert)

- After the revert, the operator reported the NumLock LED never lights again — not at boot, not on a physical NumLock press, not after cold boot/power-cycle. The firmware diff bit-for-bit against the known-good state showed ZERO changes on the LED/USB path, so firmware was never the cause.
- Mechanism identified in TinyUSB (hid_host.c, `process_set_config`): on every HID mount the host automatically sends **SET_PROTOCOL(BOOT)** on EP0 when the interface has BOOT subclass and `tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT)` is set. The SN32 manages its own LEDs only in BOOT protocol; in REPORT protocol LEDs are host-driven and the physical toggle key stops lighting them. Any previous control-transfer damage (SET_REPORT experiments) can leave the SN32 stuck in that host-driven state, which survives short power-cycles.
- Diagnostic sequence for "LED stopped working" on this rig: (1) bit-diff the firmware against the known-good commit to rule out code changes; (2) connect the keyboard directly to a normal PC USB port — the PC's enumeration (SET_PROTOCOL + SET_REPORT + LED output) resets the SN32's LED management state; (3) then reconnect to the RP2040. Do not chase firmware for this symptom.
- Corrections to earlier session conclusions: `keyboard_led_state` in WirelessKeyboard.c HEAD is write-only (never read, never rendered) — the RP2040 never lights the physical LED; the "LED worked at boot" observation was the SN32's own power-on default plus its local toggle, not host-driven output.
2. The radio reverse-ACK already delivers the Windows lock state; any future renderer must not touch EP0 (e.g. render on an RP2040-side indicator) unless constraint above is beaten.
3. Recovery path used successfully this session: full revert of LED-sync code to the known-good image. The physical LED then follows SN32's own local state (power-on default + local toggle on the physical key), which the operator accepted as sufficient.

## Related but separate

- OTA slot-swap bootsel fixes are covered by the rp2040-ota-slot-swap skill; the 2026-09-10 session fixed a watchdog_update()-in-XIP crash there. That session also found the OTA flasher tools report false-positive CRC matches (stale PyInstaller exe) and the radio path corrupts staging during simultaneous keyboard typing — separate follow-up, not LED sync.
