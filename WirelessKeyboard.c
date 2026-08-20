#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "host/usbh.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"

#ifndef RUNTIME_LOGGING
#define RUNTIME_LOGGING 0
#endif

#if !RUNTIME_LOGGING
#define printf(...) do { } while (0)
#endif

/*--------------------------------------------------------------------+
 *  Pin assignments
 *--------------------------------------------------------------------*/
#define USB_HOST_DP_PIN   4   // D+  (D- is forced to DP_PIN + 1 = GP5 by the lib)

#define SPI_PORT          spi0
#define SPI_BAUD_HZ       8000000
#define PIN_SPI_SCK       6    // SPI0 SCK  -> nRF P0.17
#define PIN_SPI_MOSI      7    // SPI0 TX   -> nRF P0.20
#define PIN_SPI_MISO      8    // SPI0 RX   <- nRF P0.08 (reverse ACK/status path)
#define PIN_SPI_CSN       9    // manual GPIO -> nRF P0.22 (SPIS CSN)

#define KBD_REPORT_LEN    8    // boot report: modifier, reserved, key1..key6
#define LINK_FRAME_LEN    12
#define LINK_MAGIC        0xA5
#define LINK_VERSION      0x03
#define LINK_TYPE_KEYBOARD 0x01
#define LINK_TYPE_CONSUMER 0x02
#define LINK_TYPE_CONTROL  0x03
#define LINK_TYPE_BATTERY  0x04
#define LINK_TYPE_DFU_START     0x10
#define LINK_TYPE_DFU_DATA      0x11
#define LINK_TYPE_DFU_FINISH    0x12
#define LINK_TYPE_DFU_STATUS    0x13
#define LINK_CONTROL_SYSTEM_OFF 0x01
#define LINK_CONTROL_POLL_ACK   0x02
#define LINK_CONTROL_SPI_POLL   0x03
#define LINK_ACK_MAGIC           0x5A
#define LINK_ACK_TYPE_LOCK_STATE 0x01
#define LINK_ACK_TYPE_DFU       0x02

#define FLASH_STAGING_OFFSET (2048u * 1024u)   /* 2MB offset for WeAct RP2040 4MB Flash */
#define FLASH_STAGING_MAX_SIZE (1900u * 1024u) /* Up to 1.9MB firmware image size */

#define DFU_STATUS_IDLE         0x00u
#define DFU_STATUS_BUSY         0x01u
#define DFU_STATUS_OK           0x02u
#define DFU_STATUS_ERR_SIZE     0x03u
#define DFU_STATUS_ERR_CRC      0x04u
#define DFU_STATUS_ERR_FLASH    0x05u
#define DFU_STATUS_SUCCESS      0x06u
#define RADIO_INACTIVITY_MS (5u * 60u * 1000u)
#define RADIO_OFF_SETTLE_MS 10u
#define RADIO_BOOT_WAIT_MS  75u
#define RADIO_WAKE_QUEUE_DEPTH 128u
#define SPI_INPUT_QUEUE_DEPTH 128u
#define RADIO_SLEEP_BLINK_COUNT 4u
#define RADIO_SLEEP_BLINK_HALF_PERIOD_MS 100u
#define SPI_REARM_GUARD_US 250u
#define SPI_ACK_POLL_MS 100u
#define KEYBOARD_HID_STALL_RECOVERY_MS 250u
#define SONIX_KEYBOARD_EP_IN 0x81u
#define PIO_USB_ROOT_INDEX 0u
#define RP2040_WATCHDOG_TIMEOUT_MS 1000u
#define BATTERY_TELEMETRY_PERIOD_MS 30000u
#define BATTERY_HID_QUIET_GUARD_MS 50u
#define MAX_HID_REPORTS   8
#define MAX_CONSUMER_FIELDS 16
#define MAX_LED_LOCAL_USAGES 16
#define MAX_LED_OUTPUT_REPORT_LEN 16
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

#define HID_USAGE_PAGE_LEDS             0x08
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
#define BATT_ADC_CAL_FULL_SCALE_MV 3300u
#define BATT_DIVIDER_NUMERATOR     3015u
#define BATT_DIVIDER_DENOMINATOR   1000u
#define BATT_MIN_MV        3430  // 1S empty: 3.43V
#define BATT_MAX_MV        4110  // 1S gauge full under the keyboard's normal load
#define BATT_FULL_EXIT_MV  4080  // hysteresis: avoid Full/Idle flicker
#define BATT_TREND_STEP_MV 2
#define BATT_CHARGE_TREND_COUNT 6
#define BATT_CHECK_MS      1000
#define BATT_BOOT_SHOW_MS  5000
#define BATT_EVENT_SHOW_MS 5000
#define BATT_PULSE_WINDOW_MS 2000
#define BATT_PULSE_PERIOD_MS 1000
#define BATT_PIN_EVENT_DELTA_MV 15
#define BATT_EVENT_DELTA_MV \
    ((BATT_PIN_EVENT_DELTA_MV * BATT_DIVIDER_NUMERATOR + 500u) / 1000u)

// --- RGB LED, driven locally by RP2040 PWM on GP21, GP20, GP19 (Catod Comun) -----
#define PIN_LED_R         21
#define PIN_LED_G         20
#define PIN_LED_B         19
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
#define CONSUMER_DEBUG    0   /* Set to 1 only for descriptor/raw diagnostics. */
#endif

#ifndef HID_DIAGNOSTIC_LOG
#define HID_DIAGNOSTIC_LOG 0 /* Bounded changed-report UART trace for DAPLink. */
#endif

#ifndef NULL_MOVEMENT_ENABLED
#define NULL_MOVEMENT_ENABLED 1 /* Null Movement (Snap Tap / SOCD Last Win) enabled for A/D and W/S */
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
static uint8_t           keyboard_led_output_report_id;
static uint8_t           keyboard_led_output_report_len = 1;
static uint16_t          keyboard_led_output_bit_offsets[3] = { 0, 1, 2 };
static bool              keyboard_uses_report_protocol;
static uint32_t          keyboard_last_report_ms;
static uint32_t          keyboard_recovery_after_ms;
static bool              keyboard_halt_recovery_pending;
static bool              keyboard_halt_recovery_in_progress;
#if HID_DIAGNOSTIC_LOG
static uint32_t          keyboard_last_diagnostic_ms;
#endif

struct link_input_frame {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t data[KBD_REPORT_LEN];
} __attribute__((packed));

_Static_assert(sizeof(struct link_input_frame) == LINK_FRAME_LEN,
               "SPI link frame must remain 12 bytes");

struct link_ack_frame {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t data[KBD_REPORT_LEN];
} __attribute__((packed));

_Static_assert(sizeof(struct link_ack_frame) == LINK_FRAME_LEN,
               "SPI ACK frame must remain 12 bytes");

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
    uint8_t led_output_report_id;
    uint8_t led_output_report_len;
    uint16_t led_output_bit_offsets[3];
    bool has_led_output;
};

static struct hid_instance_state hid_instances[CFG_TUH_HID];
static uint16_t previous_consumer_usage;
static bool previous_consumer_valid;

enum radio_power_state {
    RADIO_AWAKE,
    RADIO_SYSTEM_OFF,
    RADIO_WAKING,
};

static enum radio_power_state radio_power_state = RADIO_AWAKE;
static uint32_t radio_last_activity_ms;
static uint32_t radio_transition_after_ms;
static bool radio_wake_requested;
static uint32_t hid_activity_hash[CFG_TUH_HID];
static uint16_t hid_activity_len[CFG_TUH_HID];
static bool hid_activity_valid[CFG_TUH_HID];
static bool hid_receive_rearm_pending[CFG_TUH_HID];

/* PIO-USB is timing-sensitive: core 1 owns TinyUSB/PIO and writes this
 * single-producer/single-consumer ring, while core 0 owns SPI/radio.  No
 * callback may wait for SPI or issue radio traffic.  At 1 kHz this leaves
 * 64 complete keyboard states (64 ms) of headroom without allocation/locks. */
#define USB_HOST_EVENT_QUEUE_DEPTH 64u
_Static_assert((USB_HOST_EVENT_QUEUE_DEPTH &
                (USB_HOST_EVENT_QUEUE_DEPTH - 1u)) == 0,
               "USB host event queue depth must be a power of two");
enum usb_host_event_type {
    USB_HOST_EVENT_DEVICE_MOUNT,
    USB_HOST_EVENT_DEVICE_UMOUNT,
    USB_HOST_EVENT_KEYBOARD_MOUNT,
    USB_HOST_EVENT_KEYBOARD_UMOUNT,
    USB_HOST_EVENT_KEYBOARD_REPORT,
    USB_HOST_EVENT_CONSUMER_REPORT,
};
struct usb_host_event {
    uint8_t type;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t data[KBD_REPORT_LEN];
};
static struct usb_host_event usb_host_event_queue[USB_HOST_EVENT_QUEUE_DEPTH];
static volatile uint8_t usb_host_event_head;
static volatile uint8_t usb_host_event_tail;
static volatile uint32_t usb_host_event_overruns;

static bool usb_host_event_push(uint8_t type, uint8_t dev_addr,
                                uint8_t instance, uint8_t const *data)
{
    uint8_t const head = usb_host_event_head;
    uint8_t const next = (uint8_t)((head + 1u) &
                                   (USB_HOST_EVENT_QUEUE_DEPTH - 1u));
    if (next == usb_host_event_tail) {
        /* Keyboard reports are absolute state snapshots.  Prefer the newest
         * state to an old one, so a saturated queue can never leave a key
         * logically held.  Normal operation never reaches this path. */
        usb_host_event_tail = (uint8_t)((usb_host_event_tail + 1u) &
                                        (USB_HOST_EVENT_QUEUE_DEPTH - 1u));
        ++usb_host_event_overruns;
    }

    struct usb_host_event *event = &usb_host_event_queue[head];
    event->type = type;
    event->dev_addr = dev_addr;
    event->instance = instance;
    if (data != NULL) memcpy(event->data, data, KBD_REPORT_LEN);
    else memset(event->data, 0, KBD_REPORT_LEN);
    __dmb();
    usb_host_event_head = next;
    return true;
}

static bool usb_host_event_pop(struct usb_host_event *event)
{
    uint8_t const tail = usb_host_event_tail;
    if (tail == usb_host_event_head) return false;

    __dmb();
    *event = usb_host_event_queue[tail];
    __dmb();
    usb_host_event_tail = (uint8_t)((tail + 1u) &
                                    (USB_HOST_EVENT_QUEUE_DEPTH - 1u));
    return true;
}

struct pending_radio_input {
    uint8_t type;
    uint8_t data[KBD_REPORT_LEN];
};

static struct pending_radio_input radio_wake_queue[RADIO_WAKE_QUEUE_DEPTH];
static uint8_t radio_wake_queue_head;
static uint8_t radio_wake_queue_count;
static struct pending_radio_input spi_input_queue[SPI_INPUT_QUEUE_DEPTH];
static uint8_t spi_input_queue_head;
static uint8_t spi_input_queue_count;
static bool radio_sleep_indicator_active;
static uint32_t radio_sleep_indicator_started_ms;
static uint8_t spi_sequence;
static uint8_t spi_control_sequence;
static struct link_input_frame spi_retry_frame;
static bool spi_retry_pending;
static uint32_t spi_retry_after_us;
static struct pending_radio_input battery_spi_pending_frame;
static bool battery_spi_pending;
static uint32_t __unused spi_last_ack_poll_ms;

static uint8_t remote_keyboard_led_state;
static uint8_t remote_keyboard_led_sequence;
static uint8_t remote_keyboard_led_epoch;
static bool remote_keyboard_led_valid;

/* Locally simulated keyboard lock state. */
static volatile uint8_t  keyboard_led_state;
static uint8_t           keyboard_led_tx_report[MAX_LED_OUTPUT_REPORT_LEN];
static uint8_t           keyboard_led_tx_state;
static uint8_t           keyboard_lock_pressed;
static volatile bool     keyboard_led_update_pending;
static volatile bool     keyboard_led_transfer_active;
static volatile uint32_t keyboard_led_retry_after_ms;

static bool              battery_tx_pending;
static bool              battery_material_step;
static uint32_t          battery_last_tx_ms;
static uint32_t          battery_last_check_ms;
static uint8_t           battery_sequence;

static uint32_t total_hid_reports_received = 0;
static uint32_t total_spi_frames_sent      = 0;
static uint32_t total_output_reports_sent  = 0;

enum { BLINK_NOT_MOUNTED = 250, BLINK_MOUNTED = 1000, BLINK_SUSPENDED = 2500 };
static volatile uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
static volatile uint8_t pending_descriptor_dev_addr;
static volatile uint32_t descriptor_dump_after_ms;

/*--------------------------------------------------------------------+
 *  SPI master bridge to the nRF52840 (SPI slave). The fixed 12-byte frame
 *  carries either an 8-byte boot keyboard state, a normalized 16-bit Consumer
 *  Control usage, or a low-priority latest-state Battery record. RGB control
 *  remains local; the RP2040 remains the battery measurement authority.
 *--------------------------------------------------------------------*/
static void spi_master_init(void)
{
    spi_init(SPI_PORT, SPI_BAUD_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_SPI_CSN);
    gpio_set_dir(PIN_SPI_CSN, GPIO_OUT);
    gpio_put(PIN_SPI_CSN, 1);

    printf("[SPI] Master init OK: %d Hz, SCK=GP%d MOSI=GP%d MISO=GP%d ACK CSN=GP%d\n",
           SPI_BAUD_HZ, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_CSN);
#if SPI_LINK_TEST_MODE
    printf("[SPI] *** LINK TEST MODE ENABLED ***\n");
#endif
}

static bool spi_sequence_is_newer(uint8_t sequence, uint8_t previous)
{
    return (int8_t)(sequence - previous) > 0;
}

static bool pending_radio_queue_push(struct pending_radio_input *queue,
                                     uint8_t *head, uint8_t *count,
                                     uint8_t depth, uint8_t type,
                                     const uint8_t data[KBD_REPORT_LEN])
{
    if (*count >= depth) return false;

    uint8_t const tail = (uint8_t)((*head + *count) % depth);
    queue[tail].type = type;
    memcpy(queue[tail].data, data, KBD_REPORT_LEN);
    ++(*count);
    return true;
}

static bool pending_radio_queue_pop(struct pending_radio_input *queue,
                                    uint8_t *head, uint8_t *count,
                                    uint8_t depth,
                                    struct pending_radio_input *output)
{
    if (*count == 0) return false;

    *output = queue[*head];
    *head = (uint8_t)((*head + 1u) % depth);
    --(*count);
    return true;
}

static uint32_t dfu_total_size = 0;
static uint32_t dfu_expected_crc32 = 0;
static uint32_t dfu_bytes_written = 0;
static uint8_t dfu_page_buffer[FLASH_PAGE_SIZE];
static uint16_t dfu_page_buffer_len = 0;
static uint8_t dfu_last_seq = 0xFF;

static uint32_t calculate_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return ~crc;
}

static void dfu_flash_erase_staging(uint32_t size)
{
    uint32_t const aligned_size = (size + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t const ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_STAGING_OFFSET, aligned_size);
    restore_interrupts(ints);
}

static void dfu_flash_program_page(uint32_t offset, const uint8_t *data)
{
    uint32_t const ints = save_and_disable_interrupts();
    flash_range_program(FLASH_STAGING_OFFSET + offset, data, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

static bool spi_queue_input(uint8_t type, const uint8_t data[KBD_REPORT_LEN]);

static void dfu_send_status(uint8_t status, uint8_t progress, uint16_t offset_div_4)
{
    uint8_t data[KBD_REPORT_LEN] = { 0 };
    data[0] = status;
    data[1] = progress;
    data[2] = (uint8_t)offset_div_4;
    data[3] = (uint8_t)(offset_div_4 >> 8);
    spi_queue_input(LINK_TYPE_DFU_STATUS, data);
}

static void __no_inline_not_in_flash_func(dfu_apply_and_reboot)(uint32_t size)
{
    /* 0. Stop Core 1 completely to prevent XIP instruction fetch collisions */
    multicore_reset_core1();

    uint8_t ram_page[FLASH_PAGE_SIZE]; /* Must reside in SRAM */
    uint32_t const aligned_size = (size + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t const ints = save_and_disable_interrupts();
    (void)ints;

    /* 1. Erase Slot A (active application starting at offset 0) */
    flash_range_erase(0, aligned_size);

    /* 2. Copy page-by-page from Staging Slot into SRAM, then program into Slot A */
    for (uint32_t offset = 0; offset < aligned_size; offset += FLASH_PAGE_SIZE) {
        const uint8_t *src_xip = (const uint8_t *)(XIP_BASE + FLASH_STAGING_OFFSET + offset);
        for (uint16_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
            ram_page[i] = src_xip[i];
        }
        flash_range_program(offset, ram_page, FLASH_PAGE_SIZE);
    }

    /* 3. Reboot RP2040 into the newly installed firmware */
    watchdog_reboot(0, 0, 0);
    while (1) {
        tight_loop_contents();
    }
}

static void dfu_process_command(struct link_ack_frame const *ack)
{
    uint8_t const cmd = ack->data[0];

    if (cmd == LINK_TYPE_DFU_START) {
        dfu_total_size = (uint32_t)ack->data[1] |
                         ((uint32_t)ack->data[2] << 8) |
                         ((uint32_t)ack->data[3] << 16) |
                         ((uint32_t)ack->data[4] << 24);
        dfu_expected_crc32 = (uint32_t)ack->data[5] |
                            ((uint32_t)ack->data[6] << 8) |
                            ((uint32_t)ack->data[7] << 16);
        if (dfu_total_size == 0 || dfu_total_size > FLASH_STAGING_MAX_SIZE) {
            dfu_send_status(DFU_STATUS_ERR_SIZE, 0, 0);
            return;
        }
        dfu_flash_erase_staging(dfu_total_size);
        dfu_bytes_written = 0;
        dfu_page_buffer_len = 0;
        dfu_last_seq = ack->sequence;
        dfu_send_status(DFU_STATUS_OK, 0, 0);
        return;
    }

    if (cmd == LINK_TYPE_DFU_DATA) {
        if (ack->sequence == dfu_last_seq) {
            uint8_t const prog = (uint8_t)((dfu_bytes_written * 100u) / (dfu_total_size ? dfu_total_size : 1u));
            dfu_send_status(DFU_STATUS_OK, prog, (uint16_t)(dfu_bytes_written / 4u));
            return;
        }
        dfu_last_seq = ack->sequence;

        for (int i = 2; i < 8; ++i) {
            if (dfu_bytes_written + dfu_page_buffer_len < dfu_total_size) {
                dfu_page_buffer[dfu_page_buffer_len++] = ack->data[i];
                if (dfu_page_buffer_len == FLASH_PAGE_SIZE) {
                    dfu_flash_program_page(dfu_bytes_written, dfu_page_buffer);
                    dfu_bytes_written += FLASH_PAGE_SIZE;
                    dfu_page_buffer_len = 0;
                }
            }
        }

        uint8_t const prog = (uint8_t)((dfu_bytes_written * 100u) / (dfu_total_size ? dfu_total_size : 1u));
        dfu_send_status(DFU_STATUS_OK, prog, (uint16_t)(dfu_bytes_written / 4u));
        return;
    }

    if (cmd == LINK_TYPE_DFU_FINISH) {
        if (dfu_page_buffer_len > 0) {
            memset(&dfu_page_buffer[dfu_page_buffer_len], 0xFF, FLASH_PAGE_SIZE - dfu_page_buffer_len);
            dfu_flash_program_page(dfu_bytes_written, dfu_page_buffer);
            dfu_bytes_written += dfu_page_buffer_len;
            dfu_page_buffer_len = 0;
        }

        const uint8_t *staging_flash = (const uint8_t *)(XIP_BASE + FLASH_STAGING_OFFSET);
        uint32_t const actual_crc = calculate_crc32(staging_flash, dfu_total_size);

        if (dfu_expected_crc32 != 0 && (actual_crc & 0x00FFFFFFu) != (dfu_expected_crc32 & 0x00FFFFFFu)) {
            dfu_send_status(DFU_STATUS_ERR_CRC, 0, 0);
        } else {
            dfu_send_status(DFU_STATUS_SUCCESS, 100, (uint16_t)(dfu_total_size / 4u));
            sleep_ms(250);
            dfu_apply_and_reboot(dfu_total_size);
        }
        return;
    }
}

static void spi_process_ack(uint8_t const rx[LINK_FRAME_LEN])
{
    struct link_ack_frame ack;

    memcpy(&ack, rx, sizeof(ack));
    if (ack.magic != LINK_ACK_MAGIC || ack.version != LINK_VERSION) {
        return;
    }

    if (ack.type == LINK_ACK_TYPE_DFU) {
        dfu_process_command(&ack);
        return;
    }

    if (ack.type == LINK_ACK_TYPE_LOCK_STATE && (ack.data[1] & 0x02u)) {
        battery_last_check_ms = 0;
        battery_tx_pending = true;
        battery_last_tx_ms = 0;
    }

    if (ack.type != LINK_ACK_TYPE_LOCK_STATE ||
        (ack.data[1] & 0x01u) == 0) {
        return;
    }

    if (remote_keyboard_led_valid &&
        ack.data[2] == remote_keyboard_led_epoch &&
        !spi_sequence_is_newer(ack.sequence, remote_keyboard_led_sequence)) {
        return;
    }

    if (!remote_keyboard_led_valid ||
        ack.data[2] != remote_keyboard_led_epoch) {
        remote_keyboard_led_epoch = ack.data[2];
        remote_keyboard_led_valid = false;
    }
    remote_keyboard_led_sequence = ack.sequence;
    remote_keyboard_led_valid = true;
    remote_keyboard_led_state = ack.data[0] &
        (HID_LED_NUM_LOCK | HID_LED_CAPS_LOCK | HID_LED_SCROLL_LOCK);

    if (keyboard_led_state != remote_keyboard_led_state) {
        keyboard_led_state = remote_keyboard_led_state;
        keyboard_led_update_pending = false;
    }
#if HID_DIAGNOSTIC_LOG
    static uint8_t last_logged_led = 0xFF;
    if (ack.data[0] != last_logged_led) {
        printf("[SPI ACK] seq=%u led=0x%02x\n", ack.sequence, ack.data[0]);
        last_logged_led = ack.data[0];
    }
#endif
}

static void spi_write_frame(struct link_input_frame const *frame)
{
    uint8_t rx[LINK_FRAME_LEN] = { 0 };
#if HOT_PATH_DEBUG
    uint32_t const t0 = time_us_32();
#endif

    gpio_put(PIN_SPI_CSN, 0);
    sleep_us(1);
    spi_write_read_blocking(SPI_PORT, (uint8_t const *)frame, rx,
                            sizeof(*frame));
    sleep_us(1);
    gpio_put(PIN_SPI_CSN, 1);

    total_spi_frames_sent++;
    spi_process_ack(rx);
#if HID_DIAGNOSTIC_LOG
    printf("[SPI TX #%lu] type=%u seq=%u data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)total_spi_frames_sent,
           frame->type, frame->sequence,
           frame->data[0], frame->data[1], frame->data[2], frame->data[3],
           frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
#elif HOT_PATH_DEBUG
    uint32_t const dt_us = time_us_32() - t0;
    printf("[SPI TX #%lu] took %lu us | type=%u seq=%u data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)total_spi_frames_sent, (unsigned long)dt_us,
           frame->type, frame->sequence,
           frame->data[0], frame->data[1], frame->data[2], frame->data[3],
           frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
#endif
}

static bool spi_queue_input(uint8_t type, const uint8_t data[KBD_REPORT_LEN])
{
    if (type == LINK_TYPE_BATTERY) {
        battery_spi_pending_frame.type = type;
        memcpy(battery_spi_pending_frame.data, data, KBD_REPORT_LEN);
        battery_spi_pending = true;
        return true;
    }

    if (type != LINK_TYPE_CONTROL && radio_power_state != RADIO_AWAKE) {
        return pending_radio_queue_push(radio_wake_queue,
                                        &radio_wake_queue_head,
                                        &radio_wake_queue_count,
                                        RADIO_WAKE_QUEUE_DEPTH, type, data);
    }

    return pending_radio_queue_push(spi_input_queue,
                                    &spi_input_queue_head,
                                    &spi_input_queue_count,
                                    SPI_INPUT_QUEUE_DEPTH, type, data);
}

static bool spi_send_keyboard_transition(
    const uint8_t data[KBD_REPORT_LEN])
{
    return spi_queue_input(LINK_TYPE_KEYBOARD, data);
}

static uint32_t spi_last_tx_us = 0;

static void spi_service_task(void)
{
    struct pending_radio_input pending;

    if (spi_retry_pending) {
        if (radio_power_state != RADIO_AWAKE ||
            (int32_t)(time_us_32() - spi_retry_after_us) < 0) {
            return;
        }

        spi_retry_pending = false;
        spi_write_frame(&spi_retry_frame);
        spi_last_tx_us = time_us_32();
        return;
    }

    if (radio_power_state != RADIO_AWAKE) return;

    /* Enforce 150us guard time so nRF52840 SPIS EasyDMA has time to re-arm between frames */
    if ((uint32_t)(time_us_32() - spi_last_tx_us) < 150u) {
        return;
    }

    if (!pending_radio_queue_pop(spi_input_queue, &spi_input_queue_head,
                                 &spi_input_queue_count,
                                 SPI_INPUT_QUEUE_DEPTH, &pending)) {
        if (!battery_spi_pending) return;
        pending = battery_spi_pending_frame;
        battery_spi_pending = false;
    }

    spi_retry_frame = (struct link_input_frame) {
        .magic = LINK_MAGIC,
        .version = LINK_VERSION,
        .type = pending.type,
        .sequence = spi_sequence++,
        .data = { pending.data[0], pending.data[1], pending.data[2],
                  pending.data[3], pending.data[4], pending.data[5],
                  pending.data[6], pending.data[7] },
    };
    spi_write_frame(&spi_retry_frame);
    spi_last_tx_us = time_us_32();

    /* Redundant retry only for Consumer Control edges (volume/media release) */
    if (pending.type == LINK_TYPE_CONSUMER) {
        spi_retry_after_us = time_us_32() + SPI_REARM_GUARD_US;
        spi_retry_pending = true;
    } else {
        spi_retry_pending = false;
    }
}

static void __unused spi_send_control_command(uint8_t command)
{
    struct link_input_frame const frame = {
        .magic = LINK_MAGIC,
        .version = LINK_VERSION,
        .type = LINK_TYPE_CONTROL,
        .sequence = spi_control_sequence++,
        .data = { command, 0, 0, 0, 0, 0, 0, 0 },
    };

    spi_write_frame(&frame);
    spi_last_tx_us = time_us_32();
}

static void spi_ack_poll_task(void)
{
    uint32_t const now = board_millis();

    if (radio_power_state != RADIO_AWAKE || spi_retry_pending ||
        spi_input_queue_count != 0 || battery_spi_pending ||
        (uint32_t)(now - radio_last_activity_ms) <
            BATTERY_HID_QUIET_GUARD_MS ||
        (uint32_t)(now - spi_last_ack_poll_ms) < SPI_ACK_POLL_MS) {
        return;
    }

    spi_last_ack_poll_ms = now;
    /* Clock the cached reverse ESB ACK without forwarding a radio frame. */
    spi_send_control_command(LINK_CONTROL_SPI_POLL);
}

static bool keyboard_report_is_released(void)
{
    static uint8_t const released[KBD_REPORT_LEN] = { 0 };

    return !previous_output_valid ||
           memcmp(previous_output_report, released, sizeof(released)) == 0;
}

static uint32_t hid_report_hash(uint8_t const *report, uint16_t len)
{
    uint32_t hash = 2166136261u;

    for (uint16_t i = 0; i < len; ++i) {
        hash ^= report[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool hid_report_changed(uint8_t instance,
                               uint8_t const *report, uint16_t len)
{
    if (instance >= CFG_TUH_HID) return true;

    uint32_t const hash = hid_report_hash(report, len);
    bool const changed = !hid_activity_valid[instance] ||
        hid_activity_len[instance] != len ||
        hid_activity_hash[instance] != hash;

    hid_activity_hash[instance] = hash;
    hid_activity_len[instance] = len;
    hid_activity_valid[instance] = true;
    return changed;
}

static void radio_note_activity(void)
{
    radio_last_activity_ms = board_millis();
    if (radio_power_state == RADIO_SYSTEM_OFF) {
        radio_wake_requested = true;
    }
}

static void radio_start_wake(void)
{
    /* P0.22 is armed for active-low sense in System OFF. This first CSN pulse
     * only wakes/reset the nRF; no SPI clocks are generated until it boots. */
    gpio_put(PIN_SPI_CSN, 0);
    sleep_us(100);
    gpio_put(PIN_SPI_CSN, 1);
    /* A Receiver reboot may restart its LED sequence. The next valid ACK is
     * allowed to establish a fresh epoch after wake. */
    remote_keyboard_led_valid = false;
    radio_power_state = RADIO_WAKING;
    radio_wake_requested = false;
    radio_transition_after_ms = board_millis() + RADIO_BOOT_WAIT_MS;
}

static void __unused radio_power_task(void)
{
    uint32_t const now = board_millis();

    if (radio_power_state == RADIO_SYSTEM_OFF) {
        if (radio_wake_requested &&
            (int32_t)(now - radio_transition_after_ms) >= 0) {
            radio_start_wake();
        }
        return;
    }

    if (radio_power_state == RADIO_WAKING) {
        if ((int32_t)(now - radio_transition_after_ms) < 0) return;

        uint8_t const released[KBD_REPORT_LEN] = { 0 };
        uint8_t consumer[KBD_REPORT_LEN] = {
            (uint8_t)previous_consumer_usage,
            (uint8_t)(previous_consumer_usage >> 8),
            0, 0, 0, 0, 0, 0
        };

        radio_power_state = RADIO_AWAKE;
        while (radio_wake_queue_count != 0) {
            struct pending_radio_input pending;

            if (!pending_radio_queue_pop(radio_wake_queue,
                                         &radio_wake_queue_head,
                                         &radio_wake_queue_count,
                                         RADIO_WAKE_QUEUE_DEPTH, &pending)) {
                break;
            }
            (void)spi_queue_input(pending.type, pending.data);
        }
        (void)spi_queue_input(LINK_TYPE_KEYBOARD,
                              previous_output_valid ? previous_output_report : released);
        (void)spi_queue_input(LINK_TYPE_CONSUMER, consumer);
        return;
    }

    if ((uint32_t)(now - radio_last_activity_ms) < RADIO_INACTIVITY_MS ||
        !keyboard_report_is_released() ||
        (previous_consumer_valid && previous_consumer_usage != 0) ||
        keyboard_led_transfer_active || spi_retry_pending ||
        spi_input_queue_count != 0) {
        return;
    }

    spi_send_control_command(LINK_CONTROL_SYSTEM_OFF);
    radio_sleep_indicator_active = true;
    radio_sleep_indicator_started_ms = now;
    radio_power_state = RADIO_SYSTEM_OFF;
    radio_wake_queue_head = 0;
    radio_wake_queue_count = 0;
    radio_wake_requested = false;
    radio_transition_after_ms = now + RADIO_OFF_SETTLE_MS;
#if PERIODIC_DEBUG
    printf("[POWER] nRF52840 System OFF requested after 5 minutes idle\n");
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
    (void)spi_queue_input(LINK_TYPE_KEYBOARD, test_data);
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

/* Locate the Num/Caps/Scroll LED bits in the keyboard's real Output report.
 * Report IDs and bit positions are independent from the Input report, so the
 * common one-byte/same-ID layout is only a fallback. */
static void parse_keyboard_led_output(struct hid_instance_state *state,
                                      uint8_t const *desc, uint16_t len)
{
    uint16_t usage_page = 0;
    uint8_t report_size = 0;
    uint8_t report_count = 0;
    uint8_t report_id = 0;
    uint16_t output_offsets[256] = { 0 };
    uint16_t usages[MAX_LED_LOCAL_USAGES] = { 0 };
    uint8_t usage_count = 0;
    uint16_t usage_min = 0;
    bool usage_min_valid = false;
    bool selected = false;
    uint8_t selected_report_id = 0;
    uint16_t selected_offsets[3] = { 0xFFFFu, 0xFFFFu, 0xFFFFu };

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
            if (tag == 0 && usage_count < MAX_LED_LOCAL_USAGES) {
                usages[usage_count++] = (uint16_t)value;
            } else if (tag == 1) {
                usage_min = (uint16_t)value;
                usage_min_valid = true;
            }
            continue;
        }
        if (type != 0) continue;

        if (tag == 9) { /* Output */
            bool const constant = (value & 0x01u) != 0;
            bool const variable = (value & 0x02u) != 0;
            uint16_t const base = output_offsets[report_id];

            if (!constant && variable && usage_page == HID_USAGE_PAGE_LEDS &&
                report_size == 1) {
                for (uint8_t i = 0; i < report_count; ++i) {
                    uint16_t const usage = i < usage_count ? usages[i] :
                        (usage_min_valid ? (uint16_t)(usage_min + i) : 0);
                    if (usage < 1 || usage > 3) continue;

                    if (!selected) {
                        selected = true;
                        selected_report_id = report_id;
                    }
                    if (selected_report_id == report_id) {
                        selected_offsets[usage - 1] =
                            (uint16_t)(base + i);
                    }
                }
            }
            output_offsets[report_id] =
                (uint16_t)(base + (uint16_t)report_size * report_count);
        }

        /* HID local items apply only to the next Main item. */
        usage_count = 0;
        usage_min_valid = false;
    }

    if (!selected || selected_offsets[0] == 0xFFFFu ||
        selected_offsets[1] == 0xFFFFu ||
        selected_offsets[2] == 0xFFFFu) {
        return;
    }

    uint16_t const output_bytes =
        (uint16_t)((output_offsets[selected_report_id] + 7u) / 8u);
    if (output_bytes == 0 || output_bytes > MAX_LED_OUTPUT_REPORT_LEN) {
        return;
    }

    state->led_output_report_id = selected_report_id;
    state->led_output_report_len = (uint8_t)output_bytes;
    memcpy(state->led_output_bit_offsets, selected_offsets,
           sizeof(selected_offsets));
    state->has_led_output = true;
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
    if (!spi_queue_input(LINK_TYPE_CONSUMER, data)) return;
    previous_consumer_usage = usage;
    previous_consumer_valid = true;
#if CONSUMER_DEBUG
    printf("[CONSUMER] usage=0x%04x\n", usage);
#endif
}

static void consumer_task(void)
{
}

static void forward_keyboard_report(const uint8_t input[KBD_REPORT_LEN]);

static bool report_has_key(const uint8_t report[KBD_REPORT_LEN], uint8_t key)
{
    for (uint8_t i = 2; i < KBD_REPORT_LEN; ++i) {
        if (report[i] == key) return true;
    }
    return false;
}

static bool keyboard_boot_report_has_error(
    const uint8_t report[KBD_REPORT_LEN])
{
    for (uint8_t i = 2; i < KBD_REPORT_LEN; ++i) {
        /* HID Keyboard page reserves 0x01..0x03 for ErrorRollOver,
         * POSTFail and ErrorUndefined. They are not actual pressed keys. */
        if (report[i] >= 0x01u && report[i] <= 0x03u) return true;
    }
    return false;
}

/*
 * Decodes any incoming keyboard report:
 * 1. Standard 6KRO 8-byte report: [modifier, reserved, k1..k6]
 * 2. 6KRO with 1-byte Report ID (9 bytes): [id, modifier, reserved, k1..k6]
 * 3. NKRO Bitmap report (10 to 64 bytes): [modifier, bitmap...] or [id, modifier, bitmap...]
 * Outputs standard 8-byte normalized report [modifier, 0, key1..key6].
 * Returns true if valid keyboard report decoded.
 */
static bool keyboard_decode_report(uint8_t const *report, uint16_t len,
                                   uint8_t output[KBD_REPORT_LEN])
{
    if (report == NULL || len == 0) return false;

    memset(output, 0, KBD_REPORT_LEN);

    /* Case 1: Standard 8-byte boot keyboard report */
    if (len == KBD_REPORT_LEN) {
        memcpy(output, report, KBD_REPORT_LEN);
        output[1] = 0; /* Clear reserved byte */
        if (keyboard_boot_report_has_error(output)) {
            /* Rollover/error: clear keys, retain modifiers */
            memset(output + 2, 0, KBD_REPORT_LEN - 2);
        }
        return true;
    }

    /* Case 2: 9-byte report (1 byte Report ID + standard 8-byte 6KRO) */
    if (len == 9) {
        output[0] = report[1]; /* Modifier */
        output[1] = 0;
        memcpy(output + 2, report + 3, 6);
        if (keyboard_boot_report_has_error(output)) {
            memset(output + 2, 0, KBD_REPORT_LEN - 2);
        }
        return true;
    }

    /* Case 3: NKRO Bitmap report (> 9 bytes, e.g. 15, 16, 29, 32, 64 bytes) */
    if (len >= 10) {
        uint8_t modifier = 0;
        uint8_t const *bitmap = NULL;
        uint16_t bitmap_len = 0;

        /* Check if report has a Report ID prefix in byte 0 */
        if (keyboard_input_report_id != 0 && report[0] == keyboard_input_report_id) {
            modifier = report[1];
            bitmap = report + 2;
            bitmap_len = len - 2;
        } else if (report[0] != 0 && report[0] <= 0x0F && len >= 12) {
            /* Likely Report ID in byte 0, modifier in byte 1 */
            modifier = report[1];
            bitmap = report + 2;
            bitmap_len = len - 2;
        } else {
            /* Modifier in byte 0, bitmap starts at byte 1 */
            modifier = report[0];
            bitmap = report + 1;
            bitmap_len = len - 1;
        }

        output[0] = modifier;
        output[1] = 0;

        uint8_t key_idx = 2;

        /* Standard USB HID NKRO key bitmap layout (0-indexed usages 0..255) */
        for (uint16_t b = 0; b < bitmap_len && key_idx < KBD_REPORT_LEN; ++b) {
            uint8_t const byte_val = bitmap[b];
            if (byte_val == 0) continue;

            for (uint8_t bit = 0; bit < 8 && key_idx < KBD_REPORT_LEN; ++bit) {
                if ((byte_val & (1u << bit)) != 0) {
                    uint16_t const usage = (uint16_t)(b * 8u + bit);
                    if (usage >= 0x04 && usage <= 0xE7) {
                        output[key_idx++] = (uint8_t)usage;
                    }
                }
            }
        }
        return true;
    }

    /* Short or non-standard report (< 8 bytes) */
    if (len > 0) {
        output[0] = report[0];
        output[1] = 0;
        uint16_t const copy_len = len > 2 ? (len - 2 > 6 ? 6 : len - 2) : 0;
        if (copy_len > 0) {
            memcpy(output + 2, report + 2, copy_len);
        }
        return true;
    }

    return false;
}

static void keyboard_led_reset(void)
{
    /* The target Windows installation boots with Num Lock enabled. Mirror
     * that known state locally as soon as the keyboard mounts so its keypad
     * and physical Num Lock LED do not start inverted relative to Windows. */
    keyboard_led_state = HID_LED_NUM_LOCK;
    keyboard_lock_pressed = 0;
    keyboard_led_update_pending = false;
    keyboard_led_retry_after_ms = 0;
}

static void __unused keyboard_led_build_output_report(uint8_t led_state)
{
    memset(keyboard_led_tx_report, 0, sizeof(keyboard_led_tx_report));
    for (uint8_t i = 0; i < 3; ++i) {
        if ((led_state & (1u << i)) == 0) continue;

        uint16_t const bit = keyboard_led_output_bit_offsets[i];
        if (bit < (uint16_t)keyboard_led_output_report_len * 8u) {
            keyboard_led_tx_report[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        }
    }
    keyboard_led_tx_state = led_state;
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
            keyboard_led_update_pending = false;
#if PERIODIC_DEBUG
            printf("[KBD LED] local state=0x%02x\n", keyboard_led_state);
#endif
        }
    }

    keyboard_lock_pressed = pressed_now;
}

/* Disabled: sending SET_REPORT control transfers over USB to physical keyboard
 * crashes the keyboard's onboard controller and stalls the software host engine. */
static void keyboard_led_task(void)
{
    keyboard_led_update_pending = false;
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

#if NULL_MOVEMENT_ENABLED
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
#endif

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

    /* If Fn was released and keyboard transitioned back to normal keys, release any active consumer usage */
    if (previous_consumer_valid && previous_consumer_usage != 0) {
        forward_consumer_usage(0);
    }

    keyboard_led_toggle_on_press(input);
    filter_null_movement(input, output);
    if (previous_output_valid &&
        memcmp(output, previous_output_report, KBD_REPORT_LEN) == 0) {
        return;
    }

    if (!spi_send_keyboard_transition(output)) return;
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
 *  RGB battery LED — fully local to RP2040, PWM on GP21/20/19.
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
 *  Battery measurement/state machine — local ADC authority plus low-priority
 *  telemetry; never blocks or wakes the urgent input path.
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
static uint32_t battery_filtered_mv;
static uint32_t battery_previous_sample_mv;
static int8_t   battery_trend_counter;
static uint8_t battery_pct;
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
    uint32_t const raw_sample = adc_read();

    /* Hardware calibration measured on this board/battery:
     *   battery = 4.185 V, GP28 = 1.388 V
     *   divider multiplier = 4.185 / 1.388 = 3.01513
     * The 200k/100k divider has ~66.7k source impedance. Do one conversion
     * per one-second sample; the existing IIR filter supplies smoothing
     * without 16 back-to-back ADC acquisitions loading the tap downward.
     * Keep conversion in one 64-bit expression to avoid intermediate loss. */
    uint64_t const numerator = (uint64_t)raw_sample *
        BATT_ADC_CAL_FULL_SCALE_MV * BATT_DIVIDER_NUMERATOR;
    uint32_t const denominator = 4095u * BATT_DIVIDER_DENOMINATOR;
    return (uint32_t)((numerator + denominator / 2u) / denominator);
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
    static uint32_t last_led_update_ms = 0;
    if ((uint32_t)(now - last_led_update_ms) < 20u) {
        return;
    }
    last_led_update_ms = now;

    if (radio_sleep_indicator_active) {
        uint32_t const elapsed = now - radio_sleep_indicator_started_ms;
        uint32_t const duration = RADIO_SLEEP_BLINK_COUNT * 2u *
                                  RADIO_SLEEP_BLINK_HALF_PERIOD_MS;

        if (elapsed < duration) {
            if ((elapsed / RADIO_SLEEP_BLINK_HALF_PERIOD_MS) % 2u == 0u) {
                led_apply((struct rgb_color) { 0, 0, 255 });
            } else {
                led_off();
            }
            return;
        }
        radio_sleep_indicator_active = false;
    }

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
    battery_previous_sample_mv = initial_mv;
    battery_trend_counter = 0;
    battery_pct = battery_pct_for_mv(initial_mv);
    battery_state_started_ms = board_millis();
    battery_last_check_ms = battery_state_started_ms;
    battery_led_state = BATT_LED_BOOT;
    battery_last_tx_ms = battery_state_started_ms;
    battery_tx_pending = false;
    battery_spi_pending = false;
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

    int32_t const sample_delta = (int32_t)battery_filtered_mv -
                                 (int32_t)battery_previous_sample_mv;
    battery_previous_sample_mv = battery_filtered_mv;

    /* Charging is a sustained positive slope, never merely a high voltage.
     * This rejects ADC noise and a resting/full battery with no charger. */
    if (sample_delta >= BATT_TREND_STEP_MV) {
        if (battery_trend_counter < 10) battery_trend_counter++;
    } else if (sample_delta <= -BATT_TREND_STEP_MV) {
        if (battery_trend_counter > -10) battery_trend_counter -= 2;
    } else {
        if (battery_trend_counter > 0) battery_trend_counter--;
        else if (battery_trend_counter < 0) battery_trend_counter++;
    }

    bool const is_full = battery_filtered_mv >= BATT_MAX_MV ||
        (battery_led_state == BATT_LED_FULL &&
         battery_filtered_mv >= BATT_FULL_EXIT_MV);
    bool const is_charging = !is_full &&
        battery_trend_counter >= BATT_CHARGE_TREND_COUNT;

    if (is_full || is_charging) {
        battery_material_step = true;
        if (battery_led_state == BATT_LED_BOOT) {
            battery_charge_detected_during_boot = true;
        } else {
            enum battery_led_state const next_state = is_full ?
                BATT_LED_FULL : BATT_LED_CHARGING;
            if (battery_led_state != next_state) {
                battery_led_state = next_state;
                battery_state_started_ms = now;
            }
        }
    } else {
        if ((battery_led_state == BATT_LED_CHARGING ||
             battery_led_state == BATT_LED_FULL) &&
            sample_delta <= -BATT_TREND_STEP_MV) {
            battery_material_step = true;
            battery_led_state = BATT_LED_UNPLUG_SHOW;
            battery_state_started_ms = now;
        }
    }
}

static uint8_t battery_link_state(void)
{
    switch (battery_led_state) {
    case BATT_LED_CHARGING:
        return 1; /* charging */
    case BATT_LED_FULL:
        return 3; /* full */
    case BATT_LED_UNPLUG_SHOW:
        return 2; /* discharging */
    case BATT_LED_IDLE:
        return 0; /* idle */
    case BATT_LED_BOOT:
    default:
        return 4; /* unknown */
    }
}

static void battery_telemetry_task(uint32_t now)
{
    uint32_t const period_ms = (battery_led_state == BATT_LED_CHARGING ||
                                battery_led_state == BATT_LED_FULL) ? 5000u : BATTERY_TELEMETRY_PERIOD_MS;

    if ((uint32_t)(now - battery_last_tx_ms) >= period_ms) {
        battery_tx_pending = true;
    }

    if (!battery_tx_pending || radio_power_state != RADIO_AWAKE) {
        return;
    }

    if ((uint32_t)(now - radio_last_activity_ms) <
            BATTERY_HID_QUIET_GUARD_MS ||
        radio_wake_queue_count != 0 || spi_input_queue_count != 0 ||
        spi_retry_pending || battery_spi_pending ||
        !keyboard_report_is_released() ||
        (previous_consumer_valid && previous_consumer_usage != 0) ||
        keyboard_led_transfer_active) {
        return;
    }

    uint8_t data[KBD_REPORT_LEN] = {
        battery_pct,
        battery_link_state(),
        (uint8_t)battery_filtered_mv,
        (uint8_t)(battery_filtered_mv >> 8),
        battery_sequence++,
        (uint8_t)(0x01u | (battery_material_step ? 0x02u : 0x00u)),
        0,
        0,
    };

    if (spi_queue_input(LINK_TYPE_BATTERY, data)) {
        battery_tx_pending = false;
        battery_material_step = false;
        battery_last_tx_ms = now;
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
               now - battery_state_started_ms >= BATT_EVENT_SHOW_MS) {
        battery_led_state = BATT_LED_IDLE;
        battery_state_started_ms = now;
    }

    battery_update_led(now);
    battery_telemetry_task(now);
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
static void hid_receive_arm_or_defer(uint8_t dev_addr, uint8_t instance)
{
    if (instance >= CFG_TUH_HID) return;

    /* A completed interrupt-IN transfer can briefly remain busy inside the
     * host stack. Losing this one re-arm permanently stops all keyboard input,
     * so remember the failure and retry from the main loop without blocking. */
    hid_receive_rearm_pending[instance] =
        !tuh_hid_receive_report(dev_addr, instance);
}

static void hid_receive_rearm_task(void)
{
    static uint32_t last_rearm_check_ms = 0;
    uint32_t const now = board_millis();
    if ((uint32_t)(now - last_rearm_check_ms) < 2u) return;
    last_rearm_check_ms = now;

    for (uint8_t instance = 0; instance < CFG_TUH_HID; ++instance) {
        if (!hid_receive_rearm_pending[instance]) continue;

        struct hid_instance_state const *state = &hid_instances[instance];
        if (state->dev_addr == 0) {
            hid_receive_rearm_pending[instance] = false;
            continue;
        }

        if (tuh_hid_receive_report(state->dev_addr, instance)) {
            hid_receive_rearm_pending[instance] = false;
        }
    }
}

static void keyboard_halt_recovery_complete(tuh_xfer_t *xfer)
{
    keyboard_halt_recovery_in_progress = false;

    if (!kbd_is_mounted || xfer->daddr != kbd_dev_addr ||
        xfer->result != XFER_RESULT_SUCCESS) {
        keyboard_recovery_after_ms = board_millis() + 20u;
        return;
    }

    /* CLEAR_FEATURE(ENDPOINT_HALT) resets the device toggle to DATA0. Keep
     * the PIO scheduler in the same state before the first new IN poll. */
    pio_usb_host_endpoint_reset_toggle(PIO_USB_ROOT_INDEX, kbd_dev_addr,
                                       SONIX_KEYBOARD_EP_IN);
    keyboard_halt_recovery_pending = false;
    keyboard_last_report_ms = board_millis();
    hid_receive_arm_or_defer(kbd_dev_addr, kbd_instance);
}

static void keyboard_halt_recovery_task(void)
{
    if (!kbd_is_mounted || !keyboard_halt_recovery_pending ||
        keyboard_halt_recovery_in_progress ||
        (int32_t)(board_millis() - keyboard_recovery_after_ms) < 0) {
        return;
    }

    static tusb_control_request_t const clear_halt_request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_ENDPOINT,
            .type = TUSB_REQ_TYPE_STANDARD,
            .direction = TUSB_DIR_OUT,
        },
        .bRequest = TUSB_REQ_CLEAR_FEATURE,
        .wValue = tu_htole16(TUSB_REQ_FEATURE_EDPT_HALT),
        .wIndex = tu_htole16(SONIX_KEYBOARD_EP_IN),
        .wLength = 0,
    };
    tuh_xfer_t const xfer = {
        .daddr = kbd_dev_addr,
        .ep_addr = 0,
        .setup = &clear_halt_request,
        .complete_cb = keyboard_halt_recovery_complete,
    };

    if (tuh_control_xfer((tuh_xfer_t *)&xfer)) {
        keyboard_halt_recovery_in_progress = true;
    } else {
        /* An unrelated control transfer is active. Retry shortly without
         * touching any interrupt endpoint or delaying input. */
        keyboard_recovery_after_ms = board_millis() + 2u;
    }
}

static void keyboard_hid_stall_recovery_task(void)
{
    if (!kbd_is_mounted || keyboard_halt_recovery_pending) return;

    uint32_t const now = board_millis();
    if ((int32_t)(now - keyboard_recovery_after_ms) < 0 ||
        (uint32_t)(now - keyboard_last_report_ms) <
            KEYBOARD_HID_STALL_RECOVERY_MS) {
        return;
    }

    /* An endpoint made ready by an error completion has no callback to re-arm
     * it. Repair that case even if the last keyboard state was released. */
    if (tuh_hid_receive_ready(kbd_dev_addr, kbd_instance)) {
        hid_receive_arm_or_defer(kbd_dev_addr, kbd_instance);
    }
    keyboard_last_report_ms = now;
}

void tuh_mount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE MOUNTED: addr=%u ***\n", dev_addr);
    (void)usb_host_event_push(USB_HOST_EVENT_DEVICE_MOUNT, dev_addr, 0, NULL);
}

void tuh_umount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE UNMOUNTED: addr=%u ***\n", dev_addr);
    (void)usb_host_event_push(USB_HOST_EVENT_DEVICE_UMOUNT, dev_addr, 0, NULL);
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
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("\n[HID] MOUNTED dev=%u inst=%u protocol=%s\n", dev_addr, instance,
           itf_protocol == HID_ITF_PROTOCOL_KEYBOARD ? "KEYBOARD" :
           itf_protocol == HID_ITF_PROTOCOL_MOUSE    ? "MOUSE"    : "NONE");
    struct hid_instance_state *state = instance < CFG_TUH_HID ?
        &hid_instances[instance] : NULL;
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
        hid_activity_valid[instance] = false;
        state->dev_addr = dev_addr;
        if (desc_report != NULL && desc_len != 0) {
            state->report_count = tuh_hid_parse_report_descriptor(
                state->reports, MAX_HID_REPORTS, desc_report, desc_len);
            parse_consumer_fields(state, desc_report, desc_len);
            parse_keyboard_led_output(state, desc_report, desc_len);
        }
        printf("[HID] reports=%u consumer_fields=%u led_output=%u\n",
               state->report_count, state->consumer_field_count,
               state->has_led_output ? 1u : 0u);
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
        keyboard_input_report_id = 0;
        keyboard_led_output_report_id = 0;
        keyboard_led_output_report_len = 1;
        keyboard_led_output_bit_offsets[0] = 0;
        keyboard_led_output_bit_offsets[1] = 1;
        keyboard_led_output_bit_offsets[2] = 2;
        /* Keep the interface in the default REPORT protocol. Do not issue a
         * runtime SET_PROTOCOL(BOOT): this composite keyboard has separate
         * keyboard/mouse/consumer collections and a concurrent control
         * transfer can leave the PIO-USB HID IN path stale during rapid
         * multi-key transitions. */
        keyboard_uses_report_protocol = true;
        keyboard_last_report_ms = board_millis();
        keyboard_recovery_after_ms = keyboard_last_report_ms;
        keyboard_halt_recovery_pending = false;
        keyboard_halt_recovery_in_progress = false;
        keyboard_led_transfer_active = false;
        if (state != NULL) {
            for (uint8_t i = 0; i < state->report_count; ++i) {
                if (state->reports[i].usage_page == HID_USAGE_PAGE_DESKTOP &&
                    state->reports[i].usage == HID_USAGE_DESKTOP_KEYBOARD) {
                    keyboard_input_report_id = state->reports[i].report_id;
                    break;
                }
            }
            if (state->has_led_output) {
                keyboard_led_output_report_id = state->led_output_report_id;
                keyboard_led_output_report_len = state->led_output_report_len;
                memcpy(keyboard_led_output_bit_offsets,
                       state->led_output_bit_offsets,
                       sizeof(keyboard_led_output_bit_offsets));
            } else {
                /* Standard keyboards commonly share the Input Report ID for
                 * one-byte LED Output; retain that safe legacy fallback. */
                keyboard_led_output_report_id = keyboard_input_report_id;
            }
        }
        /* The default REPORT protocol is selected before tuh_init(). Starting
         * another control transfer from this mount callback would interrupt
         * TinyUSB while it is still configuring the remaining HID interfaces. */
    }
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        (void)usb_host_event_push(USB_HOST_EVENT_KEYBOARD_MOUNT,
                                  dev_addr, instance, NULL);
    }
    hid_receive_arm_or_defer(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("[HID] UNMOUNTED dev=%u inst=%u\n", dev_addr, instance);
    if (dev_addr == kbd_dev_addr && instance == kbd_instance) {
        (void)usb_host_event_push(USB_HOST_EVENT_KEYBOARD_UMOUNT,
                                  dev_addr, instance, NULL);
        kbd_dev_addr = 0;
        kbd_is_mounted = false;
        keyboard_last_report_ms = 0;
        keyboard_recovery_after_ms = 0;
        keyboard_halt_recovery_pending = false;
        keyboard_halt_recovery_in_progress = false;
    }
    if (instance < CFG_TUH_HID) {
        hid_receive_rearm_pending[instance] = false;
        memset(&hid_instances[instance], 0, sizeof(hid_instances[instance]));
        hid_activity_valid[instance] = false;
    }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len)
{
    (void)len;
    if (dev_addr != kbd_dev_addr || instance != kbd_instance ||
        report_id != keyboard_led_output_report_id ||
        report_type != HID_REPORT_TYPE_OUTPUT) {
        return;
    }

    keyboard_led_transfer_active = false;
    keyboard_led_update_pending = false;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len)
{
    total_hid_reports_received++;

    bool const hid_changed = hid_report_changed(instance, report, len);
    (void)hid_changed;

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
        bool const keyboard_xfer_failed = len == 0;
        if (keyboard_xfer_failed) {
            /* TinyUSB's HID callback exposes both a real STALL and a transient
             * PIO transport/CRC failure as a zero-length completion. Most
             * failures are not a STALL, so issuing CLEAR_FEATURE first leaves
             * EP 0x81 unpolled during a key burst. The completed transfer is
             * already released by TinyUSB: arm it again immediately below.
             * A genuine STALL will surface again and remains visible to the
             * periodic host recovery, without blocking normal input. */
            keyboard_halt_recovery_pending = false;
            keyboard_recovery_after_ms = board_millis();
        } else {
            keyboard_last_report_ms = board_millis();
            uint8_t normalized[KBD_REPORT_LEN];
            if (keyboard_decode_report(report, len, normalized)) {
                (void)usb_host_event_push(USB_HOST_EVENT_KEYBOARD_REPORT,
                                          dev_addr, instance, normalized);
            }
#if HOT_PATH_DEBUG
            if (len >= KBD_REPORT_LEN) {
                printf("[HID RX #%lu] len=%u mod=%02x keys=%02x %02x %02x %02x %02x %02x\n",
                        (unsigned long)total_hid_reports_received, len,
                        normalized[0], normalized[2], normalized[3],
                        normalized[4], normalized[5], normalized[6], normalized[7]);
            }
#endif
        }
#if HID_DIAGNOSTIC_LOG
        if (hid_changed &&
            (uint32_t)(keyboard_last_report_ms - keyboard_last_diagnostic_ms) >= 5u) {
            uint8_t const b0 = len > 0 ? report[0] : 0;
            uint8_t const b1 = len > 1 ? report[1] : 0;
            uint8_t const b2 = len > 2 ? report[2] : 0;
            uint8_t const b3 = len > 3 ? report[3] : 0;
            uint8_t const b4 = len > 4 ? report[4] : 0;
            uint8_t const b5 = len > 5 ? report[5] : 0;
            uint8_t const b6 = len > 6 ? report[6] : 0;
            uint8_t const b7 = len > 7 ? report[7] : 0;
            printf("[KBD RX] len=%u raw=%02x %02x %02x %02x %02x %02x %02x %02x q=%u retry=%u\n",
                   len, b0, b1, b2, b3, b4, b5, b6, b7,
                   spi_input_queue_count, spi_retry_pending ? 1u : 0u);
            keyboard_last_diagnostic_ms = keyboard_last_report_ms;
        }
#endif
    }

    if (consumer_report && state != NULL) {
        uint16_t const usage = decode_consumer_usage(state, report, len);
        uint8_t consumer[KBD_REPORT_LEN] = {
            (uint8_t)usage, (uint8_t)(usage >> 8), 0, 0, 0, 0, 0, 0
        };
        (void)usb_host_event_push(USB_HOST_EVENT_CONSUMER_REPORT,
                                  dev_addr, instance, consumer);
    }

    /* Re-arm every completed transfer, including zero-length failures. A
     * transient busy result is retried by the host-core task; no control
     * transfer or delay is allowed to interrupt the 1 kHz input path. */
    hid_receive_arm_or_defer(dev_addr, instance);
}

/* Core 1 consumes complete reports in arrival order. The only work on Core 0
 * in the PIO-USB callback is descriptor decoding, copying eight bytes and
 * immediately arming the next IN transfer. */
static void usb_host_event_task(void)
{
    struct usb_host_event event;

    while (usb_host_event_pop(&event)) {
        switch (event.type) {
        case USB_HOST_EVENT_DEVICE_MOUNT:
            radio_note_activity();
            blink_interval_ms = BLINK_MOUNTED;
            pending_descriptor_dev_addr = event.dev_addr;
            descriptor_dump_after_ms = board_millis() + 250u;
            break;

        case USB_HOST_EVENT_DEVICE_UMOUNT:
            radio_note_activity();
            blink_interval_ms = BLINK_NOT_MOUNTED;
            break;

        case USB_HOST_EVENT_KEYBOARD_MOUNT:
            radio_note_activity();
            remote_keyboard_led_valid = false;
            previous_consumer_usage = 0;
            previous_consumer_valid = false;
            keyboard_led_reset();
            break;

        case USB_HOST_EVENT_KEYBOARD_UMOUNT: {
            uint8_t const released[KBD_REPORT_LEN] = { 0 };
            radio_note_activity();
            (void)spi_send_keyboard_transition(released);
            forward_consumer_usage(0);
            null_movement_reset();
            keyboard_led_update_pending = false;
            remote_keyboard_led_valid = false;
            break;
        }

        case USB_HOST_EVENT_KEYBOARD_REPORT:
            radio_note_activity();
            forward_keyboard_report(event.data);
            break;

        case USB_HOST_EVENT_CONSUMER_REPORT:
            radio_note_activity();
            forward_consumer_usage((uint16_t)(event.data[0] |
                                              (event.data[1] << 8)));
            break;

        default:
            break;
        }
    }
}

static void worker_core1_main(void)
{
    /* Core 1 owns every operation downstream of USB capture: ordered event
     * consumption, SPI transmission, ADC/charging state and RGB handling.
     * It never calls TinyUSB/PIO-USB APIs. */
    while (true) {
        usb_host_event_task();
        consumer_task();
        radio_power_task();
        battery_task();
        spi_ack_poll_task();
        spi_service_task();
#if SPI_LINK_TEST_MODE
        spi_link_test_task();
#endif
        status_task();
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
#if RUNTIME_LOGGING
    stdio_init_all();
    sleep_ms(100);
    if (watchdog_caused_reboot()) {
        printf("[WATCHDOG] Previous execution reset by watchdog\n");
    }
#endif

    printf("\n\n=== RP2040 USB HID HOST + BATTERY/RGB LED (fully local) -> SPI BRIDGE ===\n");
    printf("[INIT] D+ = GP%d, D- = GP%d\n", USB_HOST_DP_PIN, USB_HOST_DP_PIN + 1);

    spi_master_init();
    led_pwm_init();
    battery_adc_init();
    watchdog_enable(RP2040_WATCHDOG_TIMEOUT_MS, true);

    printf("[INIT] Starting 5-second battery display...\n");
    battery_start_display();

    radio_last_activity_ms = board_millis();
    multicore_reset_core1();
    multicore_launch_core1(worker_core1_main);
    printf("[INIT] SPI/battery/RGB worker initialized on core 1\n");

    /* Core 0 is dedicated to PIO-USB/TinyUSB host timing and immediate
     * endpoint re-arm. No SPI, ADC, radio or battery work runs here. */
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB_HOST_DP_PIN;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION,
                  &pio_cfg);
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    tuh_init(BOARD_TUH_RHPORT);
    printf("[INIT] TinyUSB/PIO-USB host initialized on core 0\n");

    printf("[INIT] Ready. Waiting for keyboard on D+/D-...\n\n");

    while (1) {
        watchdog_update();
        tuh_task();
        hid_receive_rearm_task();
        keyboard_halt_recovery_task();
        keyboard_hid_stall_recovery_task();
        keyboard_led_task();
        usb_descriptor_dump_task();
        led_blinking_task();
    }
}
