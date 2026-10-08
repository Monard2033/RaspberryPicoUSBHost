"""Native host tests for the adaptive HID polling policy.

The C under test is extracted from the checked-out firmware/library sources and
compiled with the host GCC.  This intentionally avoids a Python reimplementation
of the policy.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[1]
PYTHON = Path(r"C:\Users\Monard\.pico-sdk\python\3.13.7\python.exe")
GCC = ROOT / "tools" / "w64devkit" / "bin" / "gcc.exe"


def extract_function(text: str, name: str) -> str:
    match = re.search(r"(?:static\s+)?(?:bool|void|uint8_t)\s+" + name + r"\s*\([^)]*\)\s*\{", text)
    if not match:
        raise RuntimeError(f"cannot find {name}")
    start = match.start()
    brace = text.find("{", match.start())
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise RuntimeError(f"unbalanced function {name}")


def main() -> int:
    fw = (ROOT / "WirelessKeyboard.c").read_text(encoding="utf-8")
    host = (ROOT / "Pico-PIO-USB" / "src" / "pio_usb_host.c").read_text(encoding="utf-8")
    policy = "\n\n".join(extract_function(fw, n) for n in (
        "hid_poll_note_real_activity", "hid_poll_force_fast",
        "hid_poll_policy_task"))
    bulk = extract_function(host, "pio_usb_host_device_set_interrupt_poll_interval")
    source = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HID_ACTIVE_POLL_INTERVAL_MS 1u
#define HID_IDLE_POLL_INTERVAL_MS 8u
#define HID_INPUT_IDLE_TIMEOUT_MS 60000u
#define PIO_USB_ROOT_INDEX 0u
#define PIO_USB_EP_POOL_CNT 8u
#define EP_IN 0x80u
#define EP_ATTR_INTERRUPT 0x03u

typedef struct {
    uint8_t root_idx, dev_addr, ep_num, attr, interval, descriptor_interval;
    uint8_t interval_counter;
    uint16_t size;
} endpoint_t;
static endpoint_t endpoints[PIO_USB_EP_POOL_CNT];
#define PIO_USB_ENDPOINT(i) (&endpoints[(i)])
static uint32_t fake_ms;
static unsigned irq_saves, irq_restores;
static uint32_t board_millis(void) { return fake_ms; }
static uint32_t save_and_disable_interrupts(void) { ++irq_saves; return 0x55; }
static void restore_interrupts(uint32_t state) { assert(state == 0x55); ++irq_restores; }
static uint8_t pio_usb_host_device_set_interrupt_poll_interval(uint8_t, uint8_t, uint8_t);

static uint32_t hid_last_real_activity_ms;
static bool hid_poll_idle, hid_keyboard_held, hid_consumer_held;
static uint8_t kbd_dev_addr = 7;
static bool kbd_is_mounted;
static uint32_t hid_poll_fast_grace_until_ms;
static bool hid_poll_fast_grace_active;

''' + policy + r'''

static uint8_t pio_usb_host_device_set_interrupt_poll_interval(uint8_t root_idx,
                                                               uint8_t device_address,
                                                               uint8_t requested_interval) {
''' + bulk.partition("{")[2].rsplit("}", 1)[0] + r'''
}

static void clear_eps(void) { memset(endpoints, 0, sizeof endpoints); }
static endpoint_t *kbd_ep(uint8_t interval) {
    endpoints[0] = (endpoint_t){0, 7, EP_IN | 1, EP_ATTR_INTERRUPT, 0, interval, 0, 8};
    return &endpoints[0];
}
static void policy_setup(void) {
    kbd_is_mounted = true; hid_poll_idle = false;
    hid_keyboard_held = hid_consumer_held = false;
    hid_poll_fast_grace_active = false; hid_last_real_activity_ms = fake_ms;
}
static void test_policy(void) {
    clear_eps(); kbd_ep(1); policy_setup();
    fake_ms = 59999; hid_poll_policy_task(); assert(!hid_poll_idle);
    fake_ms = 60000; hid_poll_policy_task(); assert(hid_poll_idle && endpoints[0].interval == 8);
    hid_poll_note_real_activity(); assert(!hid_poll_idle && endpoints[0].interval == 1);

    hid_poll_idle = false; hid_keyboard_held = true; fake_ms += 60000; hid_poll_policy_task(); assert(!hid_poll_idle);
    hid_keyboard_held = false; hid_consumer_held = true; hid_poll_policy_task(); assert(!hid_poll_idle);
    hid_consumer_held = false; hid_poll_force_fast(); assert(!hid_poll_idle && hid_poll_fast_grace_active);
    fake_ms += 249; hid_poll_policy_task(); assert(!hid_poll_idle && hid_poll_fast_grace_active);
    fake_ms += 1; hid_poll_policy_task(); assert(!hid_poll_fast_grace_active && hid_poll_idle);

    /* A failed bulk callback cannot make policy claim the endpoint is idle. */
    clear_eps(); kbd_ep(16); policy_setup(); fake_ms += 60000;
    hid_poll_policy_task(); assert(hid_poll_idle && endpoints[0].interval == 16);
    clear_eps(); policy_setup(); fake_ms += 60000; hid_poll_policy_task(); assert(!hid_poll_idle);
}
static void test_bulk_filters_and_floors(void) {
    clear_eps();
    endpoints[0] = (endpoint_t){0, 7, EP_IN | 1, EP_ATTR_INTERRUPT, 3, 1, 9, 8};
    endpoints[1] = (endpoint_t){0, 7, EP_IN | 2, EP_ATTR_INTERRUPT, 3, 16, 9, 8};
    endpoints[2] = (endpoint_t){0, 7, 2, EP_ATTR_INTERRUPT, 3, 1, 9, 8};
    endpoints[3] = (endpoint_t){0, 7, EP_IN | 3, 2, 3, 1, 9, 8};
    endpoints[4] = (endpoint_t){1, 7, EP_IN | 4, EP_ATTR_INTERRUPT, 3, 1, 9, 8};
    endpoints[5] = (endpoint_t){0, 8, EP_IN | 5, EP_ATTR_INTERRUPT, 3, 1, 9, 8};
    endpoints[6] = (endpoint_t){0, 7, EP_IN | 6, EP_ATTR_INTERRUPT, 3, 1, 9, 0};
    assert(pio_usb_host_device_set_interrupt_poll_interval(0, 7, 1) == 2);
    assert(endpoints[0].interval == 1 && endpoints[0].interval_counter == 0);
    assert(endpoints[1].interval == 16 && endpoints[2].interval == 3);
    assert(endpoints[3].interval == 3 && endpoints[4].interval == 3 && endpoints[5].interval == 3);
    assert(pio_usb_host_device_set_interrupt_poll_interval(0, 7, 8) == 2);
    assert(endpoints[0].interval == 8 && endpoints[1].interval == 16);
}
static void test_wrap_and_timer_contract(void) {
    clear_eps(); kbd_ep(1); policy_setup();
    fake_ms = 0xfffffff0u; hid_last_real_activity_ms = 0xfffffff0u;
    fake_ms = 0x00000020u; hid_poll_policy_task(); assert(!hid_poll_idle);
    fake_ms = 0x0000ea60u; hid_poll_policy_task(); assert(hid_poll_idle);
    hid_poll_idle = false; fake_ms = 0xfffffff0u; hid_poll_force_fast();
    fake_ms = 0x000000e0u; hid_poll_policy_task(); assert(hid_poll_fast_grace_active);
    clear_eps(); fake_ms = 0x000000eeu; hid_poll_policy_task(); assert(!hid_poll_idle && !hid_poll_fast_grace_active);
    assert(HID_ACTIVE_POLL_INTERVAL_MS == 1u); /* USB SOF cadence is untouched. */
}
int main(void) { test_policy(); test_bulk_filters_and_floors(); test_wrap_and_timer_contract(); puts("adaptive HID native tests: PASS"); return 0; }
'''
    with tempfile.TemporaryDirectory(prefix="adaptive_hid_native_", dir=ROOT / "tools") as td:
        c = Path(td) / "harness.c"
        exe = Path(td) / "harness.exe"
        c.write_text(source, encoding="utf-8")
        env = os.environ.copy()
        env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
        build = subprocess.run([str(GCC), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], capture_output=True, text=True, env=env)
        if build.returncode:
            print(build.stderr, end="")
            return build.returncode
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        print(run.stdout, end="")
        print(run.stderr, end="")
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
