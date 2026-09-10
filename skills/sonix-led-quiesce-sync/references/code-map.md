Key symbols in WirelessKeyboard.c (RP2040 host firmware):

- `spi_process_ack()` — core 1; parses reverse-ACK lock state; publish point for `keyboard_led_desired_state` + `keyboard_led_sync_dirty`.
- `keyboard_led_task()` — core 0 main loop; quiesce/hold-off decision, SET_REPORT issue, timeout/backoff fallbacks.
- `tuh_hid_report_received_cb()` — re-arm point; quiesce flag skips exactly one re-arm for the keyboard instance.
- `tuh_hid_set_report_complete_cb()` — re-arms EP1 after the control transfer completes; clears in-flight/held flags.
- `tuh_hid_receive_ready(dev, inst)` — true when no interrupt-IN transfer is armed (bus quiet).
- `hid_receive_arm_or_defer()` — single re-arm helper; forward-declare before the sync task.
- `tuh_hid_umount_cb()` / `keyboard_led_reset()` — reset all sync flags.

Fallback constants: KEYBOARD_LED_SYNC_TIMEOUT_MS 100, KEYBOARD_LED_SYNC_RETRY_MS 500.
Build: cmake --build build-release-ninja --target WirelessKeyboard with pico-sdk cmake/ninja on PATH.
Live verify: Receiver tools/read_input_trace.py shows ESB_RX + HID_SUBMIT_OK rc=0 under multi-key input.