"""Native checks compiling selected functions extracted from WirelessKeyboard.c."""
from pathlib import Path
import os, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
GCC = ROOT / "tools" / "w64devkit" / "bin" / "gcc.exe"
SOURCE = (ROOT / "WirelessKeyboard.c").read_text(encoding="utf-8")

def extract(signature, text=SOURCE):
    try:
        start = text.index("static " + signature)
    except ValueError:
        start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{": depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0: return text[start:pos + 1]
    raise AssertionError(signature)

decode_helper = extract("bool keyboard_boot_report_has_error")
read_bits = extract("uint16_t read_report_bits")
decode = extract("bool keyboard_decode_report")
item_value = extract("uint32_t hid_item_value")
layout_parser = extract("void parse_keyboard_input_layout")
builder = extract("void keyboard_led_build_output_report")
task = extract("void keyboard_led_task")
complete = extract("void tuh_hid_set_report_complete_cb")
host = (ROOT / "Pico-PIO-USB" / "src" / "pio_usb_host.c").read_text(encoding="utf-8")
helper = extract("bool usb_in_packet_fits", host)
assert "static bool usb_in_packet_fits" in host
assert "!usb_in_packet_fits(ep, (uint16_t)receive_len)" in host
assert "++ep->rx_oversize_count" in host
assert "pio_usb_ll_transfer_complete(ep, PIO_USB_INTS_ENDPOINT_ERROR_BITS);" in host
assert "hid_receive_arm_or_defer(dev_addr, instance);" in SOURCE
assert "if (len == keyboard_led_output_report_len)" in SOURCE

c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define KBD_REPORT_LEN 8
#define MAX_LED_OUTPUT_REPORT_LEN 16
typedef struct { uint16_t size, total_len, actual_len; } endpoint_t;
typedef struct { uint8_t report_id, usage; uint16_t usage_page; } tuh_hid_report_info_t;
struct keyboard_input_layout {
  uint8_t report_id;
  uint16_t modifier_bit_offset;
  uint8_t modifier_bit_size;
  uint16_t key_bitmap_bit_offset, key_bitmap_bit_count;
  bool has_modifier, has_key_bitmap;
};
struct hid_instance_state {
  struct keyboard_input_layout keyboard_layout;
  bool has_keyboard_layout;
};
#define HID_USAGE_PAGE_DESKTOP 0x01
#define HID_USAGE_DESKTOP_KEYBOARD 0x06
#define HID_USAGE_PAGE_KEYBOARD 0x07
static uint8_t keyboard_input_report_id;
static uint8_t keyboard_led_output_report_id;
static uint8_t keyboard_led_output_report_len;
static uint16_t keyboard_led_output_bit_offsets[3];
static uint8_t keyboard_led_tx_report[MAX_LED_OUTPUT_REPORT_LEN];
static uint8_t keyboard_led_tx_state;
static uint8_t kbd_dev_addr=1, kbd_instance=2;
static uint8_t kbd_is_mounted=1;
static uint8_t submit_result=1;
static uint32_t now_ms, keyboard_last_report_ms;
static unsigned receive_rearm_calls;
static unsigned submit_calls;
static uint8_t submitted_states[8];
static int board_millis(void) { return (int)now_ms; }
static int keyboard_report_is_released(void) { return 0; }
static int tuh_hid_set_report(uint8_t d,uint8_t i,uint8_t id,uint8_t type,void *p,uint16_t n) {
  (void)d;(void)i;(void)id;(void)type;(void)n;
  if (submit_calls < sizeof(submitted_states)) submitted_states[submit_calls] = ((uint8_t *)p)[0];
  ++submit_calls;
  return submit_result;
}
static void hid_receive_arm_or_defer(uint8_t d,uint8_t i) { (void)d;(void)i; ++receive_rearm_calls; }
#define KEYBOARD_LED_SYNC_ENABLED 1
#define HID_REPORT_TYPE_OUTPUT 2
static uint8_t keyboard_target_led_state, keyboard_rendered_led_state;
static uint8_t keyboard_led_update_pending, keyboard_led_transfer_active;
'''
c += item_value + "\n" + layout_parser + "\n" + read_bits + "\n" + decode_helper + "\n" + decode + "\n" + builder + "\n" + task + "\n" + complete + "\n" + helper + r'''
int main(void) {
  uint8_t out[8], boot[8]={0,0,4,5,6,7,0,0};
  struct hid_instance_state boot_state={0};
  tuh_hid_report_info_t boot_info={0, 6, 1};
  assert(keyboard_decode_report(&boot_state, &boot_info, boot,8,out));
  assert(out[2]==4 && out[5]==7);
  struct hid_instance_state nkro_state={0};
  nkro_state.has_keyboard_layout=true;
  nkro_state.keyboard_layout.report_id=1;
  nkro_state.keyboard_layout.modifier_bit_offset=0;
  nkro_state.keyboard_layout.modifier_bit_size=8;
  nkro_state.keyboard_layout.key_bitmap_bit_offset=8;
  nkro_state.keyboard_layout.key_bitmap_bit_count=152;
  nkro_state.keyboard_layout.has_modifier=true;
  nkro_state.keyboard_layout.has_key_bitmap=true;
  tuh_hid_report_info_t nkro_info={1, 6, 1};
  uint8_t nkro[20]={0}; nkro[1]=(uint8_t)(1u << (0x04u & 7u));
  assert(keyboard_decode_report(&nkro_state, &nkro_info, nkro, sizeof(nkro), out));
  assert(out[0]==0 && out[2]==4);
  /* Full 133-byte descriptor captured from the Sonix diagnostic log. */
  static uint8_t const desc[133] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0,
    0x25,1,0x75,1,0x95,8,0x81,2,0x05,7,0x19,0,0x29,0x97,0x15,0,
    0x25,1,0x75,1,0x96,0x98,0,0x81,2,0xc0,0x05,1,0x09,0x80,0xa1,1,
    0x85,2,0x19,0,0x29,0xb7,0x15,0,0x26,0xb7,0,0x95,1,0x75,8,0x81,0,
    0xc0,0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x1a,0,0,0x2a,0x3c,2,
    0x15,0,0x26,0x3c,2,0x75,0x10,0x95,1,0x81,0,0xc0,0x06,0x33,0xff,
    0x0a,4,0x0a,0xa1,1,0x85,7,0x19,1,0x29,0x3f,0x15,0,0x26,0xff,0,
    0x75,8,0x95,0x3f,0x81,0,0x19,1,0x29,0x3f,0x15,0,0x26,0xff,0,
    0x75,8,0x95,0x3f,0xb1,2,0xc0
  };
  struct hid_instance_state parsed={0};
  parse_keyboard_input_layout(&parsed, desc, sizeof(desc));
  assert(parsed.has_keyboard_layout);
  assert(parsed.keyboard_layout.report_id==1);
  assert(parsed.keyboard_layout.modifier_bit_offset==0);
  assert(parsed.keyboard_layout.key_bitmap_bit_offset==8);
  assert(parsed.keyboard_layout.key_bitmap_bit_count==152);
  uint8_t captured[20]={0};
  captured[1]=(uint8_t)(1u << (0x07u & 7u)); /* D */
  captured[2]=(uint8_t)((1u << 0) | (1u << 1) | (1u << 2)); /* EFG */
  assert(keyboard_decode_report(&parsed, &nkro_info, captured, sizeof(captured), out));
  assert(out[2]==7 && out[3]==8 && out[4]==9 && out[5]==10);
  memset(captured, 0, sizeof(captured));
  assert(keyboard_decode_report(&parsed, &nkro_info, captured, sizeof(captured), out));
  assert(out[0]==0 && out[2]==0);
  keyboard_led_output_report_id=0; keyboard_led_output_report_len=1;
  keyboard_led_output_bit_offsets[0]=0; keyboard_led_output_bit_offsets[1]=1;
  keyboard_led_output_bit_offsets[2]=2; keyboard_led_build_output_report(1);
  assert(keyboard_led_tx_report[0]==1 && keyboard_led_tx_state==1);
  keyboard_led_output_report_id=5; keyboard_led_output_report_len=2;
  keyboard_led_output_bit_offsets[0]=0; keyboard_led_output_bit_offsets[1]=1;
  keyboard_led_output_bit_offsets[2]=2; keyboard_led_build_output_report(3);
  assert(keyboard_led_tx_report[0]==5 && keyboard_led_tx_report[1]==3);
  /* Exercise the actual task and completion callback: submit rejection does
   * not become in-flight; zero-length completion stays pending and re-arms
   * through the deferred helper; success renders the transmitted state. */
  keyboard_led_output_report_id=0; keyboard_led_output_report_len=1;
  keyboard_led_output_bit_offsets[0]=0; keyboard_led_output_bit_offsets[1]=1;
  keyboard_led_output_bit_offsets[2]=2; keyboard_rendered_led_state=0xff;
  keyboard_target_led_state=1; keyboard_led_transfer_active=0;
  now_ms=1; keyboard_last_report_ms=0; submit_calls=0;
  submit_result=0; keyboard_led_task();
  assert(!keyboard_led_transfer_active && keyboard_rendered_led_state==0xff && submit_calls==1);
  /* A rejected submit retries on the next loop at the same time, even while a
   * key is held and before any 100 ms interval has elapsed. */
  submit_result=1; keyboard_led_task(); assert(keyboard_led_transfer_active && submit_calls==2);
  keyboard_target_led_state=3;
  tuh_hid_set_report_complete_cb(1,2,0,HID_REPORT_TYPE_OUTPUT,0);
  assert(keyboard_rendered_led_state==0xff && keyboard_led_update_pending && receive_rearm_calls==1);
  keyboard_led_task(); assert(keyboard_led_transfer_active && submit_calls==3);
  assert(submitted_states[0]==1 && submitted_states[1]==1 && submitted_states[2]==3);
  tuh_hid_set_report_complete_cb(1,2,0,HID_REPORT_TYPE_OUTPUT,1);
  assert(keyboard_rendered_led_state==3 && !keyboard_led_update_pending);
  endpoint_t ep={8,8,0}; assert(usb_in_packet_fits(&ep,8));
  assert(!usb_in_packet_fits(&ep,9)); ep.actual_len=4;
  assert(!usb_in_packet_fits(&ep,5)); ep.actual_len=9;
  assert(!usb_in_packet_fits(&ep,0));
  puts("actual extracted Sonix HID functions: PASS"); return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="sonix-led-test-") as td:
    src, exe = Path(td)/"test.c", Path(td)/"test.exe"
    src.write_text(c, encoding="utf-8")
    env = os.environ.copy(); env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([str(GCC), "-std=c11", "-O2", str(src), "-o", str(exe)], check=True, cwd=ROOT, env=env)
    subprocess.run([str(exe)], check=True, cwd=ROOT)
