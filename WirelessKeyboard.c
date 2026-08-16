#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "host/usbh.h"
#include "pio_usb.h"

/*--------------------------------------------------------------------+
 *  Pin assignments
 *--------------------------------------------------------------------*/
#define USB_HOST_DP_PIN   4   // D+  (D- is forced to DP_PIN + 1 = GP5 by the lib)

#define SPI_PORT          spi0
#define SPI_BAUD_HZ       8000000
#define PIN_SPI_SCK       6    // SPI0 SCK  -> nRF P0.17
#define PIN_SPI_MOSI      7    // SPI0 TX   -> nRF P0.20
#define PIN_SPI_MISO      8    // unused (link is write-only); wire only -> nRF P0.08
#define PIN_SPI_CSN       9    // manual GPIO -> nRF P0.22 (SPIS CSN)

#define KBD_REPORT_LEN    8    // boot report: modifier, reserved, key1..key6
#define LINK_FRAME_LEN    12
#define LINK_MAGIC        0xA5
#define LINK_VERSION      0x02
#define LINK_TYPE_KEYBOARD 0x01
#define LINK_TYPE_CONSUMER 0x02
#define MAX_HID_REPORTS   8
#define MAX_CONSUMER_FIELDS 16
#define HID_KEY_A         0x04
#define HID_KEY_D         0x07
#define HID_KEY_S         0x16
#define HID_KEY_W         0x1A
#define HID_KEY_CAPS_LOCK   0x39
#define HID_KEY_SCROLL_LOCK 0x47
#define HID_KEY_NUM_LOCK    0x53

#define HID_LED_NUM_LOCK    0x01
#define HID_LED_CAPS_LOCK   0x02
#define HID_LED_SCROLL_LOCK 0x04

#define HID_USAGE_PAGE_CONSUMER_CONTROL 0x0C
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_CONSUMER_SCAN_NEXT    0x00B5
#define HID_USAGE_CONSUMER_SCAN_PREVIOUS 0x00B6
#define HID_USAGE_CONSUMER_PLAY_PAUSE   0x00CD
#define HID_USAGE_CONSUMER_MUTE         0x00E2
#define HID_USAGE_CONSUMER_VOLUME_UP    0x00E9
#define HID_USAGE_CONSUMER_VOLUME_DOWN  0x00EA

// --- Battery (read locally, RP2040 is the sole "brain" for this) --------
#define PIN_BATT_ADC      28   // GP28 = ADC2. Battery divider tap goes here.
#define BATT_ADC_INPUT    2    // adc_select_input() channel matching GP28
#define BATT_DIVIDER_RATIO 3   // Vbat--200k--node--100k--GND: node=Vbat/3
#define BATT_MIN_MV        3430
#define BATT_MAX_MV        4200
#define BATT_CHECK_MS      1000
#define BATT_BOOT_SHOW_MS  5000
#define BATT_PULSE_WINDOW_MS 2000
#define BATT_PULSE_PERIOD_MS 1000
#define BATT_CHARGE_RISE_MV  12
#define BATT_UNPLUG_DROP_MV  20
#define BATT_ONE_PERCENT_MV   8
#define BATT_TREND_SAMPLES    3

// --- RGB LED, driven locally by RP2040 PWM, 3 consecutive free pins -----
#define PIN_LED_R         10
#define PIN_LED_G         11
#define PIN_LED_B         12
#define LED_PWM_WRAP      255   // 8-bit duty resolution
#define LED_COMMON_ANODE  0     // 0 = common cathode to GND, 1 = common anode to 3V3

#ifndef SPI_LINK_TEST_MODE
#define SPI_LINK_TEST_MODE 0   /* 1 = send 0xAA+counter pattern every 1s (diagnostic only) */
#endif

#ifndef HOT_PATH_DEBUG
#define HOT_PATH_DEBUG    0   /* Per-report UART logging breaks 1 kHz operation. */
#endif

#ifndef PERIODIC_DEBUG
#define PERIODIC_DEBUG    0   /* Runtime UART summaries also pause USB servicing. */
#endif

#ifndef CONSUMER_DEBUG
#define CONSUMER_DEBUG    1   /* Descriptor/raw multimedia diagnostics. */
#endif

#ifndef NULL_MOVEMENT_ENABLED
#define NULL_MOVEMENT_ENABLED 1 /* Last-input-wins for A/D and W/S. */
#endif

/*--------------------------------------------------------------------+
 *  USB keyboard state
 *--------------------------------------------------------------------*/
static uint8_t           kbd_dev_addr = 0;
static uint8_t           kbd_instance = 0;
static bool              kbd_is_mounted = false;
static uint8_t           previous_physical_report[KBD_REPORT_LEN];
static uint8_t           previous_output_report[KBD_REPORT_LEN];
static uint8_t           active_ad_key;
static uint8_t           active_ws_key;
static bool              previous_physical_valid;
static bool              previous_output_valid;
static uint8_t           keyboard_input_report_id;
static bool              keyboard_uses_report_protocol;

struct link_input_frame {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t data[KBD_REPORT_LEN];
} __attribute__((packed));

_Static_assert(sizeof(struct link_input_frame) == LINK_FRAME_LEN,
               "SPI link frame must remain 12 bytes");

struct consumer_field {
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint16_t usage;
    bool is_array;
};

struct hid_instance_state {
    uint8_t dev_addr;
    uint8_t report_count;
    tuh_hid_report_info_t reports[MAX_HID_REPORTS];
    uint8_t consumer_field_count;
    struct consumer_field consumer_fields[MAX_CONSUMER_FIELDS];
    bool has_consumer;
};

static struct hid_instance_state hid_instances[CFG_TUH_HID];
static uint16_t previous_consumer_usage;
static bool previous_consumer_valid;

/* Locally simulated keyboard lock state. */
static uint8_t           keyboard_led_state;
static uint8_t           keyboard_led_tx_value;
static uint8_t           keyboard_lock_pressed;
static bool              keyboard_led_update_pending;
static bool              keyboard_led_transfer_active;
static uint32_t          keyboard_led_retry_after_ms;

static uint32_t total_hid_reports_received = 0;
static uint32_t total_spi_frames_sent      = 0;
static uint32_t total_output_reports_sent  = 0;

enum { BLINK_NOT_MOUNTED = 250, BLINK_MOUNTED = 1000, BLINK_SUSPENDED = 2500 };
static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
static uint8_t pending_descriptor_dev_addr;
static uint32_t descriptor_dump_after_ms;

/*--------------------------------------------------------------------+
 *  SPI master bridge to the nRF52840 (SPI slave). The fixed 12-byte frame
 *  carries either an 8-byte boot keyboard state or a normalized 16-bit
 *  Consumer Control usage. Battery and RGB data remain local-only.
 *--------------------------------------------------------------------*/
static void spi_master_init(void)
{
    spi_init(SPI_PORT, SPI_BAUD_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_SPI_MISO);
    gpio_set_dir(PIN_SPI_MISO, GPIO_IN);

    gpio_init(PIN_SPI_CSN);
    gpio_set_dir(PIN_SPI_CSN, GPIO_OUT);
    gpio_put(PIN_SPI_CSN, 1);

    printf("[SPI] Master init OK: %d Hz, SCK=GP%d MOSI=GP%d MISO=GP%d(unused) CSN=GP%d\n",
           SPI_BAUD_HZ, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_CSN);
#if SPI_LINK_TEST_MODE
    printf("[SPI] *** LINK TEST MODE ENABLED ***\n");
#endif
}

static void spi_send_input(uint8_t type, const uint8_t data[KBD_REPORT_LEN])
{
    static uint8_t sequence;
    struct link_input_frame const frame = {
        .magic = LINK_MAGIC,
        .version = LINK_VERSION,
        .type = type,
        .sequence = sequence++,
        .data = { data[0], data[1], data[2], data[3],
                  data[4], data[5], data[6], data[7] },
    };
#if HOT_PATH_DEBUG
    uint32_t const t0 = time_us_32();
#endif

    gpio_put(PIN_SPI_CSN, 0);
    sleep_us(1);
    spi_write_blocking(SPI_PORT, (uint8_t const *)&frame, sizeof(frame));
    sleep_us(1);
    gpio_put(PIN_SPI_CSN, 1);

    total_spi_frames_sent++;
#if HOT_PATH_DEBUG
    uint32_t const dt_us = time_us_32() - t0;
    printf("[SPI TX #%lu] took %lu us | type=%u seq=%u data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)total_spi_frames_sent, (unsigned long)dt_us,
           frame.type, frame.sequence,
           frame.data[0], frame.data[1], frame.data[2], frame.data[3],
           frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
#endif
}

#if SPI_LINK_TEST_MODE
static void spi_link_test_task(void)
{
    static absolute_time_t next_send;
    static uint8_t counter = 0;
    if (absolute_time_diff_us(get_absolute_time(), next_send) > 0) return;
    next_send = make_timeout_time_ms(1000);
    uint8_t test_data[KBD_REPORT_LEN] = {
        0xAA, counter, 0xAA, counter, 0xAA, counter, 0xAA, counter
    };
    counter++;
    spi_send_input(LINK_TYPE_KEYBOARD, test_data);
}
#endif

static bool consumer_usage_supported(uint16_t usage)
{
    switch (usage) {
    case HID_USAGE_CONSUMER_SCAN_NEXT:
    case HID_USAGE_CONSUMER_SCAN_PREVIOUS:
    case HID_USAGE_CONSUMER_PLAY_PAUSE:
    case HID_USAGE_CONSUMER_MUTE:
    case HID_USAGE_CONSUMER_VOLUME_UP:
    case HID_USAGE_CONSUMER_VOLUME_DOWN:
        return true;
    default:
        return false;
    }
}

static uint32_t hid_item_value(uint8_t const *data, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; ++i) value |= (uint32_t)data[i] << (8u * i);
    return value;
}

/* Extract enough HID descriptor information to normalize common Consumer
 * array and bitmap reports. Unknown/vendor items are skipped safely. */
static void parse_consumer_fields(struct hid_instance_state *state,
                                  uint8_t const *desc, uint16_t len)
{
    uint16_t usage_page = 0;
    uint8_t report_size = 0;
    uint8_t report_count = 0;
    uint8_t report_id = 0;
    uint16_t input_offsets[256] = { 0 };
    uint16_t usages[MAX_CONSUMER_FIELDS] = { 0 };
    uint8_t usage_count = 0;
    uint16_t usage_min = 0;
    bool usage_min_valid = false;

    for (uint16_t pos = 0; pos < len;) {
        uint8_t const prefix = desc[pos++];
        if (prefix == 0xFE) {
            if (pos + 2 > len) break;
            uint8_t const long_size = desc[pos++];
            pos++;
            pos = (uint16_t)((pos + long_size <= len) ? pos + long_size : len);
            continue;
        }

        uint8_t const size_code = prefix & 0x03;
        uint8_t const size = size_code == 3 ? 4 : size_code;
        uint8_t const type = (prefix >> 2) & 0x03;
        uint8_t const tag = (prefix >> 4) & 0x0F;
        if (pos + size > len) break;
        uint32_t const value = hid_item_value(desc + pos, size);
        pos += size;

        if (type == 1) { /* Global */
            if (tag == 0) usage_page = (uint16_t)value;
            else if (tag == 7) report_size = (uint8_t)value;
            else if (tag == 8) report_id = (uint8_t)value;
            else if (tag == 9) report_count = (uint8_t)value;
            continue;
        }
        if (type == 2) { /* Local */
            if (tag == 0 && usage_count < MAX_CONSUMER_FIELDS) {
                usages[usage_count++] = (uint16_t)value;
            } else if (tag == 1) {
                usage_min = (uint16_t)value;
                usage_min_valid = true;
            }
            continue;
        }
        if (type != 0) continue;

        if (tag == 8) { /* Input */
            bool const constant = (value & 0x01u) != 0;
            bool const variable = (value & 0x02u) != 0;
            uint16_t const base = input_offsets[report_id];

            if (!constant && usage_page == HID_USAGE_PAGE_CONSUMER_CONTROL &&
                report_size > 0 && report_size <= 16) {
                if (variable) {
                    for (uint8_t i = 0; i < report_count &&
                         state->consumer_field_count < MAX_CONSUMER_FIELDS; ++i) {
                        uint16_t usage = i < usage_count ? usages[i] :
                            (usage_min_valid ? (uint16_t)(usage_min + i) : 0);
                        if (!consumer_usage_supported(usage)) continue;
                        state->consumer_fields[state->consumer_field_count++] =
                            (struct consumer_field) {
                                .report_id = report_id,
                                .bit_offset = (uint16_t)(base + i * report_size),
                                .bit_size = report_size,
                                .usage = usage,
                                .is_array = false,
                            };
                    }
                } else if (state->consumer_field_count < MAX_CONSUMER_FIELDS) {
                    state->consumer_fields[state->consumer_field_count++] =
                        (struct consumer_field) {
                            .report_id = report_id,
                            .bit_offset = base,
                            .bit_size = report_size,
                            .usage = 0,
                            .is_array = true,
                        };
                }
            }
            input_offsets[report_id] =
                (uint16_t)(base + (uint16_t)report_size * report_count);
        }

        /* Local items apply only to the next Main item. */
        usage_count = 0;
        usage_min_valid = false;
    }

    state->has_consumer = state->consumer_field_count != 0;
}

static uint16_t read_report_bits(uint8_t const *data, uint16_t len,
                                 uint16_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 16 ||
        bit_offset + bit_size > (uint32_t)len * 8u) return 0;
    uint16_t value = 0;
    for (uint8_t i = 0; i < bit_size; ++i) {
        uint16_t const bit = (uint16_t)(bit_offset + i);
        if (data[bit / 8] & (1u << (bit % 8))) value |= (uint16_t)(1u << i);
    }
    return value;
}

static uint16_t decode_consumer_usage(struct hid_instance_state const *state,
                                      uint8_t const *report, uint16_t len)
{
    uint8_t report_id = 0;
    bool has_report_ids = false;
    for (uint8_t i = 0; i < state->report_count; ++i) {
        if (state->reports[i].report_id != 0) { has_report_ids = true; break; }
    }
    if (has_report_ids) {
        if (len == 0) return 0;
        report_id = *report++;
        --len;
    }

    for (uint8_t i = 0; i < state->consumer_field_count; ++i) {
        struct consumer_field const *field = &state->consumer_fields[i];
        if (field->report_id != report_id) continue;
        uint16_t const value = read_report_bits(report, len,
                                                field->bit_offset,
                                                field->bit_size);
        uint16_t const usage = field->is_array ? value :
            (value != 0 ? field->usage : 0);
        if (consumer_usage_supported(usage)) return usage;
    }

    /* Fallback for the widespread Report-ID + 16-bit usage array layout. */
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        uint16_t const usage = (uint16_t)(report[i] | (report[i + 1] << 8));
        if (consumer_usage_supported(usage)) return usage;
    }
    return 0;
}

static void forward_consumer_usage(uint16_t usage)
{
    if (previous_consumer_valid && usage == previous_consumer_usage) return;
    uint8_t data[KBD_REPORT_LEN] = {
        (uint8_t)usage, (uint8_t)(usage >> 8), 0, 0, 0, 0, 0, 0
    };
    spi_send_input(LINK_TYPE_CONSUMER, data);
    previous_consumer_usage = usage;
    previous_consumer_valid = true;
#if CONSUMER_DEBUG
    printf("[CONSUMER] usage=0x%04x\n", usage);
#endif
}

static bool report_has_key(const uint8_t report[KBD_REPORT_LEN], uint8_t key)
{
    for (uint8_t i = 2; i < KBD_REPORT_LEN; ++i) {
        if (report[i] == key) return true;
    }
    return false;
}

static void keyboard_led_reset(void)
{
    keyboard_led_state = 0;
    keyboard_led_tx_value = 0;
    keyboard_lock_pressed = 0;
    keyboard_led_update_pending = true;
    keyboard_led_transfer_active = false;
    keyboard_led_retry_after_ms = 0;
}

static void keyboard_led_toggle_on_press(
    const uint8_t input[KBD_REPORT_LEN])
{
    static const struct {
        uint8_t key;
        uint8_t led;
    } lock_keys[] = {
        { HID_KEY_NUM_LOCK,    HID_LED_NUM_LOCK },
        { HID_KEY_CAPS_LOCK,   HID_LED_CAPS_LOCK },
        { HID_KEY_SCROLL_LOCK, HID_LED_SCROLL_LOCK },
    };

    uint8_t pressed_now = 0;

    for (size_t i = 0; i < sizeof(lock_keys) / sizeof(lock_keys[0]); ++i) {
        if (report_has_key(input, lock_keys[i].key)) {
            pressed_now |= lock_keys[i].led;
        }
        if ((pressed_now & lock_keys[i].led) != 0 &&
            (keyboard_lock_pressed & lock_keys[i].led) == 0) {
            keyboard_led_state ^= lock_keys[i].led;
            keyboard_led_update_pending = true;
#if PERIODIC_DEBUG
            printf("[KBD LED] local state=0x%02x\n", keyboard_led_state);
#endif
        }
    }

    keyboard_lock_pressed = pressed_now;
}

/* SET_REPORT is a control transfer, so keep its one-byte buffer alive and
 * submit it from the main loop rather than from the input-report callback. */
static void keyboard_led_task(void)
{
    if (!kbd_is_mounted || !keyboard_led_update_pending ||
        keyboard_led_transfer_active ||
        (int32_t)(board_millis() - keyboard_led_retry_after_ms) < 0) {
        return;
    }

    keyboard_led_tx_value = keyboard_led_state;
    if (tuh_hid_set_report(kbd_dev_addr, kbd_instance, keyboard_input_report_id,
                           HID_REPORT_TYPE_OUTPUT,
                           &keyboard_led_tx_value,
                           sizeof(keyboard_led_tx_value))) {
        keyboard_led_transfer_active = true;
    } else {
        keyboard_led_retry_after_ms = board_millis() + 100;
    }
}

static void null_movement_reset(void)
{
    memset(previous_physical_report, 0, sizeof(previous_physical_report));
    memset(previous_output_report, 0, sizeof(previous_output_report));
    active_ad_key = 0;
    active_ws_key = 0;
    previous_physical_valid = false;
    previous_output_valid = false;
}

static uint8_t select_last_input_key(bool first_now, bool second_now,
                                     bool first_pressed, bool second_pressed,
                                     uint8_t first_key, uint8_t second_key,
                                     uint8_t previous_active)
{
    if (first_pressed != second_pressed) {
        return first_pressed ? first_key : second_key;
    }
    if (first_now && second_now) {
        if (previous_active == first_key || previous_active == second_key) {
            return previous_active;
        }
        /* Both arrived in one USB poll, so their physical order is unknowable. */
        return second_key;
    }
    if (first_now) return first_key;
    if (second_now) return second_key;
    return 0;
}

static void append_key_once(uint8_t output[KBD_REPORT_LEN],
                            uint8_t *output_index, uint8_t key)
{
    if (key == 0 || *output_index >= KBD_REPORT_LEN) return;

    for (uint8_t i = 2; i < *output_index; ++i) {
        if (output[i] == key) return;
    }
    output[(*output_index)++] = key;
}

/*
 * Implements the attached AutoHotkey script in firmware. Physical state and
 * output state are kept separately: the most recently pressed key wins while
 * both opposites are held, and releasing it restores the still-held key.
 */
static void filter_null_movement(const uint8_t input[KBD_REPORT_LEN],
                                 uint8_t output[KBD_REPORT_LEN])
{
#if !NULL_MOVEMENT_ENABLED
    memcpy(output, input, KBD_REPORT_LEN);
    return;
#else
    bool const a_now = report_has_key(input, HID_KEY_A);
    bool const d_now = report_has_key(input, HID_KEY_D);
    bool const w_now = report_has_key(input, HID_KEY_W);
    bool const s_now = report_has_key(input, HID_KEY_S);
    bool const a_before = previous_physical_valid &&
                          report_has_key(previous_physical_report, HID_KEY_A);
    bool const d_before = previous_physical_valid &&
                          report_has_key(previous_physical_report, HID_KEY_D);
    bool const w_before = previous_physical_valid &&
                          report_has_key(previous_physical_report, HID_KEY_W);
    bool const s_before = previous_physical_valid &&
                          report_has_key(previous_physical_report, HID_KEY_S);

    active_ad_key = select_last_input_key(a_now, d_now,
                                         a_now && !a_before,
                                         d_now && !d_before,
                                         HID_KEY_A, HID_KEY_D, active_ad_key);
    active_ws_key = select_last_input_key(w_now, s_now,
                                         w_now && !w_before,
                                         s_now && !s_before,
                                         HID_KEY_W, HID_KEY_S, active_ws_key);

    memset(output, 0, KBD_REPORT_LEN);
    output[0] = input[0];
    output[1] = input[1];
    uint8_t output_index = 2;

    for (uint8_t i = 2; i < KBD_REPORT_LEN; ++i) {
        uint8_t const key = input[i];

        if ((key == HID_KEY_A || key == HID_KEY_D) && key != active_ad_key) {
            continue;
        }
        if ((key == HID_KEY_W || key == HID_KEY_S) && key != active_ws_key) {
            continue;
        }
        append_key_once(output, &output_index, key);
    }

    memcpy(previous_physical_report, input, KBD_REPORT_LEN);
    previous_physical_valid = true;
#endif
}

static void forward_keyboard_report(const uint8_t input[KBD_REPORT_LEN])
{
    uint8_t output[KBD_REPORT_LEN];

    keyboard_led_toggle_on_press(input);
    filter_null_movement(input, output);
    if (previous_output_valid &&
        memcmp(output, previous_output_report, KBD_REPORT_LEN) == 0) {
        return;
    }

    spi_send_input(LINK_TYPE_KEYBOARD, output);
    memcpy(previous_output_report, output, KBD_REPORT_LEN);
    previous_output_valid = true;
    total_output_reports_sent++;

#if HOT_PATH_DEBUG
    printf("[HID->SPI #%lu] %02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)total_output_reports_sent,
           output[0], output[1], output[2], output[3],
           output[4], output[5], output[6], output[7]);
#endif
}

/*--------------------------------------------------------------------+
 *  RGB battery LED — fully local to RP2040, PWM on GP10/11/12.
 *--------------------------------------------------------------------*/
struct rgb_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

static uint slice_r, slice_g, slice_b;
static uint chan_r, chan_g, chan_b;

static void led_pwm_init(void)
{
    gpio_set_function(PIN_LED_R, GPIO_FUNC_PWM);
    gpio_set_function(PIN_LED_G, GPIO_FUNC_PWM);
    gpio_set_function(PIN_LED_B, GPIO_FUNC_PWM);

    slice_r = pwm_gpio_to_slice_num(PIN_LED_R);
    slice_g = pwm_gpio_to_slice_num(PIN_LED_G);
    slice_b = pwm_gpio_to_slice_num(PIN_LED_B);
    chan_r  = pwm_gpio_to_channel(PIN_LED_R);
    chan_g  = pwm_gpio_to_channel(PIN_LED_G);
    chan_b  = pwm_gpio_to_channel(PIN_LED_B);

    pwm_set_wrap(slice_r, LED_PWM_WRAP);
    pwm_set_wrap(slice_g, LED_PWM_WRAP);
    pwm_set_wrap(slice_b, LED_PWM_WRAP);
    uint8_t const off_level = LED_COMMON_ANODE ? LED_PWM_WRAP : 0;
    pwm_set_chan_level(slice_r, chan_r, off_level);
    pwm_set_chan_level(slice_g, chan_g, off_level);
    pwm_set_chan_level(slice_b, chan_b, off_level);
    pwm_set_enabled(slice_r, true);
    pwm_set_enabled(slice_g, true);
    pwm_set_enabled(slice_b, true);

    printf("[LED] PWM init OK: R=GP%d G=GP%d B=GP%d\n", PIN_LED_R, PIN_LED_G, PIN_LED_B);
}

static uint8_t led_pwm_level(uint8_t brightness)
{
#if LED_COMMON_ANODE
    return LED_PWM_WRAP - brightness;
#else
    return brightness;
#endif
}

static void led_apply(struct rgb_color color)
{
    pwm_set_chan_level(slice_r, chan_r, led_pwm_level(color.r));
    pwm_set_chan_level(slice_g, chan_g, led_pwm_level(color.g));
    pwm_set_chan_level(slice_b, chan_b, led_pwm_level(color.b));
}

static void led_off(void)
{
    led_apply((struct rgb_color) { 0, 0, 0 });
}

static struct rgb_color battery_color_for_pct(uint8_t pct)
{
    if (pct >= 75) return (struct rgb_color) { 0, 255, 0 };   // green
    if (pct >= 50) return (struct rgb_color) { 255, 255, 0 }; // yellow
    if (pct >= 25) return (struct rgb_color) { 255, 80, 0 };  // orange
    return (struct rgb_color) { 255, 0, 0 };                  // red
}

/*--------------------------------------------------------------------+
 *  Battery measurement/state machine — local ADC only, no radio data.
 *--------------------------------------------------------------------*/
enum battery_led_state {
    BATT_LED_BOOT,
    BATT_LED_IDLE,
    BATT_LED_CHARGING,
    BATT_LED_FULL,
    BATT_LED_UNPLUG_SHOW,
};

static enum battery_led_state battery_led_state = BATT_LED_BOOT;
static uint32_t battery_state_started_ms;
static uint32_t battery_last_check_ms;
static uint32_t battery_filtered_mv;
static uint32_t battery_trend_reference_mv;
static uint32_t battery_charge_peak_mv;
static uint8_t battery_pct;
static uint8_t battery_rise_samples;
static uint8_t battery_drop_samples;
static bool battery_charge_detected_during_boot;

static void battery_adc_init(void)
{
    adc_init();
    adc_gpio_init(PIN_BATT_ADC);
    adc_select_input(BATT_ADC_INPUT);

    printf("[BATT] ADC init OK on GP%d (input %d)\n",
           PIN_BATT_ADC, BATT_ADC_INPUT);
}

static uint32_t battery_read_mv(void)
{
    adc_select_input(BATT_ADC_INPUT);
    uint32_t raw_sum = 0;

    for (uint8_t i = 0; i < 16; ++i) {
        raw_sum += adc_read();
    }

    uint32_t const raw_average = raw_sum / 16;
    uint32_t const pin_mv = raw_average * 3300 / 4095;
    return pin_mv * BATT_DIVIDER_RATIO;
}

static uint8_t battery_pct_for_mv(uint32_t batt_mv)
{
    if (batt_mv <= BATT_MIN_MV) return 0;
    if (batt_mv >= BATT_MAX_MV) return 100;
    return (uint8_t)((batt_mv - BATT_MIN_MV) * 100 /
                     (BATT_MAX_MV - BATT_MIN_MV));
}

static void battery_update_led(uint32_t now)
{
    struct rgb_color const color = battery_color_for_pct(battery_pct);

    switch (battery_led_state) {
    case BATT_LED_BOOT:
    case BATT_LED_FULL:
    case BATT_LED_UNPLUG_SHOW:
        led_apply(color);
        break;
    case BATT_LED_CHARGING:
        /* Exactly two ON/OFF cycles in every two-second animation window. */
        if (((now - battery_state_started_ms) % BATT_PULSE_WINDOW_MS) %
             BATT_PULSE_PERIOD_MS < (BATT_PULSE_PERIOD_MS / 2)) {
            led_apply(color);
        } else {
            led_off();
        }
        break;
    case BATT_LED_IDLE:
    default:
        led_off();
        break;
    }
}

static void battery_start_display(void)
{
    uint32_t const initial_mv = battery_read_mv();

    battery_filtered_mv = initial_mv;
    battery_trend_reference_mv = initial_mv;
    battery_charge_peak_mv = initial_mv;
    battery_pct = battery_pct_for_mv(initial_mv);
    battery_state_started_ms = board_millis();
    battery_last_check_ms = battery_state_started_ms;
    battery_led_state = BATT_LED_BOOT;
    battery_update_led(battery_state_started_ms);

#if PERIODIC_DEBUG
    printf("[BATT] boot mv=%lu pct=%u\n",
           (unsigned long)initial_mv, battery_pct);
#endif
}

static void battery_sample_task(uint32_t now)
{
    if (now - battery_last_check_ms < BATT_CHECK_MS) return;
    battery_last_check_ms = now;

    uint32_t const sample_mv = battery_read_mv();
    battery_filtered_mv = (battery_filtered_mv * 3 + sample_mv) / 4;
    battery_pct = battery_pct_for_mv(battery_filtered_mv);

    if (battery_filtered_mv > battery_charge_peak_mv) {
        battery_charge_peak_mv = battery_filtered_mv;
    }

    if (battery_led_state == BATT_LED_FULL) {
        if (battery_pct <= 99 &&
            battery_filtered_mv + BATT_ONE_PERCENT_MV <= BATT_MAX_MV) {
            if (++battery_drop_samples >= BATT_TREND_SAMPLES) {
                battery_led_state = BATT_LED_UNPLUG_SHOW;
                battery_state_started_ms = now;
                battery_drop_samples = 0;
            }
        } else {
            battery_drop_samples = 0;
        }
        return;
    }

    if (battery_led_state == BATT_LED_CHARGING) {
        if (battery_pct >= 100) {
            battery_led_state = BATT_LED_FULL;
            battery_state_started_ms = now;
            battery_drop_samples = 0;
        } else if (battery_filtered_mv + BATT_UNPLUG_DROP_MV <
                   battery_charge_peak_mv) {
            if (++battery_drop_samples >= BATT_TREND_SAMPLES) {
                battery_led_state = BATT_LED_IDLE;
                battery_state_started_ms = now;
                battery_trend_reference_mv = battery_filtered_mv;
                battery_rise_samples = 0;
                battery_drop_samples = 0;
            }
        } else {
            battery_drop_samples = 0;
        }
        return;
    }

    if (battery_filtered_mv >=
        battery_trend_reference_mv + BATT_CHARGE_RISE_MV) {
        if (++battery_rise_samples >= BATT_TREND_SAMPLES) {
            if (battery_led_state == BATT_LED_BOOT) {
                battery_charge_detected_during_boot = true;
            } else {
                battery_led_state = battery_pct >= 100 ?
                    BATT_LED_FULL : BATT_LED_CHARGING;
                battery_state_started_ms = now;
            }
            battery_charge_peak_mv = battery_filtered_mv;
            battery_rise_samples = 0;
        }
    } else {
        battery_rise_samples = 0;
        /* Follow slow discharge downward without following noise upward. */
        if (battery_filtered_mv < battery_trend_reference_mv) {
            battery_trend_reference_mv = battery_filtered_mv;
        }
    }
}

static void battery_task(void)
{
    uint32_t const now = board_millis();

    battery_sample_task(now);

    if (battery_led_state == BATT_LED_BOOT &&
        now - battery_state_started_ms >= BATT_BOOT_SHOW_MS) {
        battery_led_state = battery_charge_detected_during_boot ?
            (battery_pct >= 100 ? BATT_LED_FULL : BATT_LED_CHARGING) :
            BATT_LED_IDLE;
        battery_state_started_ms = now;
    } else if (battery_led_state == BATT_LED_UNPLUG_SHOW &&
               now - battery_state_started_ms >= BATT_BOOT_SHOW_MS) {
        battery_led_state = BATT_LED_IDLE;
        battery_state_started_ms = now;
        battery_trend_reference_mv = battery_filtered_mv;
    }

    battery_update_led(now);
}

/*--------------------------------------------------------------------+
 *  Periodic status dump
 *--------------------------------------------------------------------*/
static void status_task(void)
{
#if PERIODIC_DEBUG
    static uint32_t last_print_ms = 0;
    uint32_t now = board_millis();
    if (now - last_print_ms < 3000) return;
    last_print_ms = now;

    printf("[STATUS] uptime=%lus | HID mounted=%d addr=%u | HID reports rx=%lu | SPI frames tx=%lu\n",
           (unsigned long)(now / 1000), kbd_is_mounted, kbd_dev_addr,
           (unsigned long)total_hid_reports_received,
           (unsigned long)total_spi_frames_sent);
#endif
}

static void usb_descriptor_dump_task(void)
{
#if CONSUMER_DEBUG
    if (pending_descriptor_dev_addr == 0 ||
        (int32_t)(board_millis() - descriptor_dump_after_ms) < 0) return;

    uint8_t const dev_addr = pending_descriptor_dev_addr;
    pending_descriptor_dev_addr = 0;
    tusb_desc_device_t device_desc;
    uint8_t const device_result = tuh_descriptor_get_device_sync(
        dev_addr, &device_desc, sizeof(device_desc));
    if (device_result != XFER_RESULT_SUCCESS) {
        printf("[USB DESC] device read failed: result=%u\n", device_result);
        return;
    }

    printf("[USB DESC] VID=%04x PID=%04x USB=%04x class=%02x/%02x/%02x configs=%u\n",
           device_desc.idVendor, device_desc.idProduct, device_desc.bcdUSB,
           device_desc.bDeviceClass, device_desc.bDeviceSubClass,
           device_desc.bDeviceProtocol, device_desc.bNumConfigurations);

    static uint8_t config_desc[512] __attribute__((aligned(4)));
    for (uint8_t config_index = 0;
         config_index < device_desc.bNumConfigurations; ++config_index) {
        uint8_t result = tuh_descriptor_get_configuration_sync(
            dev_addr, config_index, config_desc, sizeof(tusb_desc_configuration_t));
        if (result != XFER_RESULT_SUCCESS) {
            printf("[USB DESC] config[%u] header failed: result=%u\n",
                   config_index, result);
            continue;
        }

        tusb_desc_configuration_t const *header =
            (tusb_desc_configuration_t const *)config_desc;
        uint16_t const requested_len = header->wTotalLength < sizeof(config_desc) ?
            header->wTotalLength : sizeof(config_desc);
        result = tuh_descriptor_get_configuration_sync(
            dev_addr, config_index, config_desc, requested_len);
        if (result != XFER_RESULT_SUCCESS) {
            printf("[USB DESC] config[%u] body failed: result=%u\n",
                   config_index, result);
            continue;
        }

        header = (tusb_desc_configuration_t const *)config_desc;
        printf("[USB DESC] config[%u] value=%u interfaces=%u total=%u captured=%u\n",
               config_index, header->bConfigurationValue,
               header->bNumInterfaces, header->wTotalLength, requested_len);
        for (uint16_t offset = 0; offset + 2 <= requested_len;) {
            uint8_t const length = config_desc[offset];
            uint8_t const type = config_desc[offset + 1];
            if (length < 2 || offset + length > requested_len) break;
            printf("[USB CFG %03u]", offset);
            for (uint8_t i = 0; i < length; ++i) {
                printf(" %02x", config_desc[offset + i]);
            }
            printf("\n");
            if (type == TUSB_DESC_INTERFACE &&
                length >= sizeof(tusb_desc_interface_t)) {
                tusb_desc_interface_t const *itf =
                    (tusb_desc_interface_t const *)(config_desc + offset);
                printf("[USB ITF] num=%u alt=%u eps=%u class=%02x/%02x/%02x\n",
                       itf->bInterfaceNumber, itf->bAlternateSetting,
                       itf->bNumEndpoints, itf->bInterfaceClass,
                       itf->bInterfaceSubClass, itf->bInterfaceProtocol);
            }
            offset = (uint16_t)(offset + length);
        }
    }
#endif
}

/*--------------------------------------------------------------------+
 *  USB HID host callbacks
 *--------------------------------------------------------------------*/
void tuh_mount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE MOUNTED: addr=%u ***\n", dev_addr);
    blink_interval_ms = BLINK_MOUNTED;
    pending_descriptor_dev_addr = dev_addr;
    descriptor_dump_after_ms = board_millis() + 250;
}

void tuh_umount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE UNMOUNTED: addr=%u ***\n", dev_addr);
    blink_interval_ms = BLINK_NOT_MOUNTED;
    if (dev_addr == kbd_dev_addr) { kbd_dev_addr = 0; kbd_is_mounted = false; }
}

static tuh_hid_report_info_t const *hid_report_info_for_input(
    struct hid_instance_state const *state,
    uint8_t const **report, uint16_t *len)
{
    if (state->report_count == 0) return NULL;
    if (state->report_count == 1 && state->reports[0].report_id == 0) {
        return &state->reports[0];
    }
    if (*len == 0) return NULL;

    uint8_t const report_id = (*report)[0];
    ++(*report);
    --(*len);
    for (uint8_t i = 0; i < state->report_count; ++i) {
        if (state->reports[i].report_id == report_id) return &state->reports[i];
    }
    return NULL;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    /* Pico-PIO-USB reaches the HID class callback reliably even on paths
     * where the optional general tuh_mount_cb() is not observed. */
    if (pending_descriptor_dev_addr == 0) {
        pending_descriptor_dev_addr = dev_addr;
        descriptor_dump_after_ms = board_millis() + 250;
    }
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("\n[HID] MOUNTED dev=%u inst=%u protocol=%s\n", dev_addr, instance,
           itf_protocol == HID_ITF_PROTOCOL_KEYBOARD ? "KEYBOARD" :
           itf_protocol == HID_ITF_PROTOCOL_MOUSE    ? "MOUSE"    : "NONE");
    struct hid_instance_state *state = instance < CFG_TUH_HID ?
        &hid_instances[instance] : NULL;
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
        state->dev_addr = dev_addr;
        if (desc_report != NULL && desc_len != 0) {
            state->report_count = tuh_hid_parse_report_descriptor(
                state->reports, MAX_HID_REPORTS, desc_report, desc_len);
            parse_consumer_fields(state, desc_report, desc_len);
        }
        printf("[HID] reports=%u consumer_fields=%u\n",
               state->report_count, state->consumer_field_count);
#if CONSUMER_DEBUG
        printf("[HID] descriptor len=%u:", desc_len);
        for (uint16_t i = 0; i < desc_len; ++i) {
            if ((i % 16u) == 0) printf("\n[HID DESC %03u]", i);
            printf(" %02x", desc_report[i]);
        }
        printf("\n");
        for (uint8_t i = 0; i < state->report_count; ++i) {
            printf("[HID] report[%u] id=%u page=0x%04x usage=0x%02x\n",
                   i, state->reports[i].report_id,
                   state->reports[i].usage_page, state->reports[i].usage);
        }
#endif
    }

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        kbd_dev_addr = dev_addr;
        kbd_instance = instance;
        kbd_is_mounted = true;
        keyboard_led_reset();
        previous_consumer_usage = 0;
        previous_consumer_valid = false;
        keyboard_input_report_id = 0;
        /* Diagnostic and normal operation both require REPORT protocol: boot
         * protocol intentionally removes non-boot multimedia information. */
        keyboard_uses_report_protocol = true;
        if (state != NULL) {
            for (uint8_t i = 0; i < state->report_count; ++i) {
                if (state->reports[i].usage_page == HID_USAGE_PAGE_DESKTOP &&
                    state->reports[i].usage == HID_USAGE_DESKTOP_KEYBOARD) {
                    keyboard_input_report_id = state->reports[i].report_id;
                    break;
                }
            }
        }
        /* The default REPORT protocol is selected before tuh_init(). Starting
         * another control transfer from this mount callback would interrupt
         * TinyUSB while it is still configuring the remaining HID interfaces. */
    }
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("[HID][ERROR] Failed to arm receive for dev=%u inst=%u\n", dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("[HID] UNMOUNTED dev=%u inst=%u\n", dev_addr, instance);
    if (dev_addr == kbd_dev_addr && instance == kbd_instance) {
        uint8_t const released[KBD_REPORT_LEN] = { 0 };

        spi_send_input(LINK_TYPE_KEYBOARD, released);
        forward_consumer_usage(0);
        null_movement_reset();
        keyboard_led_transfer_active = false;
        keyboard_led_update_pending = false;
        kbd_dev_addr = 0;
        kbd_is_mounted = false;
    }
    if (instance < CFG_TUH_HID) {
        memset(&hid_instances[instance], 0, sizeof(hid_instances[instance]));
    }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len)
{
    if (dev_addr != kbd_dev_addr || instance != kbd_instance ||
        report_id != keyboard_input_report_id ||
        report_type != HID_REPORT_TYPE_OUTPUT) {
        return;
    }

    keyboard_led_transfer_active = false;
    if (len == sizeof(keyboard_led_tx_value) &&
        keyboard_led_tx_value == keyboard_led_state) {
        keyboard_led_update_pending = false;
    } else {
        keyboard_led_update_pending = true;
        keyboard_led_retry_after_ms = board_millis() + 100;
    }
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol)
{
    printf("[HID] Protocol set complete: dev=%u inst=%u protocol=%s\n",
           dev_addr, instance, protocol == HID_PROTOCOL_BOOT ? "BOOT" : "REPORT");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len)
{
    total_hid_reports_received++;

#if CONSUMER_DEBUG
    static uint8_t previous_raw[CFG_TUH_HID][64];
    static uint16_t previous_raw_len[CFG_TUH_HID];
    bool const raw_changed = instance >= CFG_TUH_HID || len > 64 ||
        previous_raw_len[instance] != len ||
        memcmp(previous_raw[instance], report, len) != 0;
    if (raw_changed) {
        printf("[HID RAW] dev=%u inst=%u len=%u:", dev_addr, instance, len);
        for (uint16_t i = 0; i < len; ++i) printf(" %02x", report[i]);
        printf("\n");
        if (instance < CFG_TUH_HID && len <= 64) {
            memcpy(previous_raw[instance], report, len);
            previous_raw_len[instance] = len;
        }
    }
#endif

    struct hid_instance_state const *state =
        instance < CFG_TUH_HID && hid_instances[instance].dev_addr == dev_addr ?
        &hid_instances[instance] : NULL;
    uint8_t const *payload = report;
    uint16_t payload_len = len;
    tuh_hid_report_info_t const *info = state != NULL ?
        hid_report_info_for_input(state, &payload, &payload_len) : NULL;
    bool const consumer_report = info != NULL &&
        info->usage_page == HID_USAGE_PAGE_CONSUMER_CONTROL &&
        info->usage == HID_USAGE_CONSUMER_CONTROL;

    if (dev_addr == kbd_dev_addr && instance == kbd_instance) {
        bool const keyboard_report = !keyboard_uses_report_protocol ||
            (info != NULL && info->usage_page == HID_USAGE_PAGE_DESKTOP &&
             info->usage == HID_USAGE_DESKTOP_KEYBOARD);
        if (keyboard_report && payload_len >= KBD_REPORT_LEN) {
            forward_keyboard_report(payload);
#if HOT_PATH_DEBUG
            printf("[HID RX #%lu] mod=%02x res=%02x keys=%02x %02x %02x %02x %02x %02x\n",
                   (unsigned long)total_hid_reports_received,
                   payload[0], payload[1], payload[2], payload[3],
                   payload[4], payload[5], payload[6], payload[7]);
#endif
        }
    }

    if (consumer_report && state != NULL) {
        forward_consumer_usage(decode_consumer_usage(state, report, len));
    }

    /* Re-arm immediately; no UART output is allowed before this at 1 kHz. */
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("[HID][ERROR] Failed to re-arm receive for dev=%u inst=%u\n", dev_addr, instance);
    }
}

/*--------------------------------------------------------------------+
 *  Onboard status LED task (link/mount heartbeat — separate from RGB)
 *--------------------------------------------------------------------*/
static void led_blinking_task(void)
{
    static uint32_t start_ms = 0;
    static bool led_state = false;
    if (!blink_interval_ms) return;
    if (board_millis() - start_ms < blink_interval_ms) return;
    start_ms += blink_interval_ms;
    board_led_write(led_state);
    led_state = !led_state;
}

/*--------------------------------------------------------------------+
 *  MAIN
 *--------------------------------------------------------------------*/
int main(void)
{
    set_sys_clock_khz(120000, true);

    board_init();
    stdio_init_all();
    sleep_ms(100);

    printf("\n\n=== RP2040 USB HID HOST + BATTERY/RGB LED (fully local) -> SPI BRIDGE ===\n");
    printf("[INIT] D+ = GP%d, D- = GP%d\n", USB_HOST_DP_PIN, USB_HOST_DP_PIN + 1);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB_HOST_DP_PIN;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    tuh_init(BOARD_TUH_RHPORT);
    printf("[INIT] TinyUSB host stack initialized\n");

    spi_master_init();
    led_pwm_init();
    battery_adc_init();

    printf("[INIT] Starting 5-second battery display...\n");
    battery_start_display();

    printf("[INIT] Ready. Waiting for keyboard on D+/D-...\n\n");

    while (1) {
        tuh_task();
        usb_descriptor_dump_task();
#if SPI_LINK_TEST_MODE
        spi_link_test_task();
#endif
        led_blinking_task();
        keyboard_led_task();
        battery_task();
        status_task();
    }
}
