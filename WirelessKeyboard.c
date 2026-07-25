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
#define SPI_BAUD_HZ       4000000
#define PIN_SPI_SCK       6    // SPI0 SCK  -> nRF P0.17
#define PIN_SPI_MOSI      7    // SPI0 TX   -> nRF P0.20
#define PIN_SPI_MISO      8    // unused (link is write-only); wire only -> nRF P0.08
#define PIN_SPI_CSN       9    // manual GPIO -> nRF P0.22 (SPIS CSN)

#define KBD_REPORT_LEN    8    // boot report: modifier, reserved, key1..key6

// --- Battery (read locally, RP2040 is the sole "brain" for this) --------
#define PIN_BATT_ADC      28   // GP28 = ADC2. Battery divider tap goes here.
#define BATT_ADC_INPUT    2    // adc_select_input() channel matching GP28
#define PIN_CHRG          27   // TP4056 CHRG pin (open-drain, LOW = charging)
#define BATT_DIVIDER_RATIO 3   // Vbat--200k--node--100k--GND: node=Vbat/3
#define BATT_MIN_MV        3430
#define BATT_MAX_MV        4200
#define BATT_LOW_PCT       20
#define BATT_CHECK_MS      30000   // re-check battery every 30s

// --- RGB LED, driven locally by RP2040 PWM, 3 consecutive free pins -----
#define PIN_LED_R         10
#define PIN_LED_G         11
#define PIN_LED_B         12
#define LED_PWM_WRAP      255   // 8-bit duty resolution

#ifndef SPI_LINK_TEST_MODE
#define SPI_LINK_TEST_MODE 0
#endif

/*--------------------------------------------------------------------+
 *  Shared state between the USB HID callback and the main loop
 *--------------------------------------------------------------------*/
static volatile uint8_t  kbd_report[KBD_REPORT_LEN];
static volatile bool     report_dirty = false;
static uint8_t           kbd_dev_addr = 0;
static uint8_t           kbd_instance = 0;
static bool              kbd_is_mounted = false;

static uint32_t total_hid_reports_received = 0;
static uint32_t total_spi_frames_sent      = 0;

enum { BLINK_NOT_MOUNTED = 250, BLINK_MOUNTED = 1000, BLINK_SUSPENDED = 2500 };
static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

/*--------------------------------------------------------------------+
 *  SPI master bridge to the nRF52840 (SPI slave) — plain 8-byte frames,
 *  exactly as the nRF transmitter already expects. No LED/battery data
 *  goes over this link anymore; that's local-only now.
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

static void spi_send_report(const uint8_t *report8)
{
    uint32_t t0 = time_us_32();

    gpio_put(PIN_SPI_CSN, 0);
    sleep_us(5);
    spi_write_blocking(SPI_PORT, report8, KBD_REPORT_LEN);
    sleep_us(2);
    gpio_put(PIN_SPI_CSN, 1);

    uint32_t dt_us = time_us_32() - t0;
    total_spi_frames_sent++;
    printf("[SPI TX #%lu] took %lu us | bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           (unsigned long)total_spi_frames_sent, (unsigned long)dt_us,
           report8[0], report8[1], report8[2], report8[3],
           report8[4], report8[5], report8[6], report8[7]);
}

#if SPI_LINK_TEST_MODE
static void spi_link_test_task(void)
{
    static absolute_time_t next_send;
    static uint8_t counter = 0;
    if (absolute_time_diff_us(get_absolute_time(), next_send) > 0) return;
    next_send = make_timeout_time_ms(1000);
    uint8_t test_frame[KBD_REPORT_LEN] = {
        0xAA, counter, 0xAA, counter, 0xAA, counter, 0xAA, counter
    };
    counter++;
    spi_send_report(test_frame);
}
#endif

static void spi_bridge_task(void)
{
#if SPI_LINK_TEST_MODE
    spi_link_test_task();
    return;
#endif
    if (!report_dirty) return;

    uint8_t snapshot[KBD_REPORT_LEN];
    uint32_t irq = save_and_disable_interrupts();
    memcpy(snapshot, (const void *)kbd_report, KBD_REPORT_LEN);
    report_dirty = false;
    restore_interrupts(irq);

    spi_send_report(snapshot);
}

/*--------------------------------------------------------------------+
 *  RGB LED engine — fully local to RP2040, PWM on GP10/11/12.
 *  Modes: OFF / SOLID / BLINK / BREATHE, same behaviour as before,
 *  just driven directly instead of relayed over SPI.
 *--------------------------------------------------------------------*/
enum led_mode { LED_OFF_MODE, LED_SOLID, LED_BLINK, LED_BREATHE };
static enum led_mode led_mode = LED_OFF_MODE;
static uint8_t led_r_on, led_g_on, led_b_on;   /* which channels are active for this mode */

static uint slice_r, slice_g, slice_b;
static uint chan_r,  chan_g,  chan_b;

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
    pwm_set_chan_level(slice_r, chan_r, 0);
    pwm_set_chan_level(slice_g, chan_g, 0);
    pwm_set_chan_level(slice_b, chan_b, 0);
    pwm_set_enabled(slice_r, true);
    pwm_set_enabled(slice_g, true);
    pwm_set_enabled(slice_b, true);

    printf("[LED] PWM init OK: R=GP%d G=GP%d B=GP%d\n", PIN_LED_R, PIN_LED_G, PIN_LED_B);
}

static void led_apply(uint8_t r_duty, uint8_t g_duty, uint8_t b_duty)
{
    pwm_set_chan_level(slice_r, chan_r, r_duty);
    pwm_set_chan_level(slice_g, chan_g, g_duty);
    pwm_set_chan_level(slice_b, chan_b, b_duty);
}

static void led_off(void)
{
    led_mode = LED_OFF_MODE;
    led_apply(0, 0, 0);
    printf("[LED] OFF\n");
}

static void led_solid(uint8_t r, uint8_t g, uint8_t b)
{
    led_mode = LED_SOLID;
    led_apply(r ? LED_PWM_WRAP : 0, g ? LED_PWM_WRAP : 0, b ? LED_PWM_WRAP : 0);
    printf("[LED] SOLID r=%u g=%u b=%u\n", r, g, b);
}

static void led_start(enum led_mode m, uint8_t r, uint8_t g, uint8_t b)
{
    led_mode = m; led_r_on = r; led_g_on = g; led_b_on = b;
    printf("[LED] %s r=%u g=%u b=%u\n", m == LED_BLINK ? "BLINK" : "BREATHE", r, g, b);
}

/* Non-blocking ticker: advances the active animation. Call every loop. */
static void led_tick_task(void)
{
    static uint32_t last_tick_ms = 0;
    static int phase = 0;   // 0..99

    if (led_mode != LED_BLINK && led_mode != LED_BREATHE) return;

    uint32_t now = board_millis();
    if (now - last_tick_ms < 30) return;
    last_tick_ms = now;

    phase = (phase + 4) % 100;

    if (led_mode == LED_BLINK) {
        uint8_t on = (phase < 50) ? LED_PWM_WRAP : 0;
        led_apply(led_r_on ? on : 0, led_g_on ? on : 0, led_b_on ? on : 0);
    } else { // LED_BREATHE
        int lvl = (phase < 50) ? phase : (100 - phase);       // triangle 0..50
        uint8_t duty = (uint8_t)((lvl * LED_PWM_WRAP) / 50);
        led_apply(led_r_on ? duty : 0, led_g_on ? duty : 0, led_b_on ? duty : 0);
    }
}

static void battery_color_for_pct(uint8_t pct, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (pct >= 70)      { *r = 0; *g = 1; *b = 0; }   // green  70-100
    else if (pct >= 30) { *r = 1; *g = 1; *b = 0; }   // yellow 30-70
    else                { *r = 1; *g = 0; *b = 0; }   // red    1-30
}

/*--------------------------------------------------------------------+
 *  Battery measurement — local ADC read, no devicetree, no SPI relay.
 *--------------------------------------------------------------------*/
static void battery_adc_init(void)
{
    adc_init();
    adc_gpio_init(PIN_BATT_ADC);
    adc_select_input(BATT_ADC_INPUT);

    gpio_init(PIN_CHRG);
    gpio_set_dir(PIN_CHRG, GPIO_IN);
    gpio_pull_up(PIN_CHRG);

    printf("[BATT] ADC init OK on GP%d (input %d), CHRG on GP%d\n",
           PIN_BATT_ADC, BATT_ADC_INPUT, PIN_CHRG);
}

static int battery_read(bool *charging)
{
    adc_select_input(BATT_ADC_INPUT);
    uint16_t raw = adc_read();                       // 12-bit: 0-4095
    uint32_t pin_mv = (uint32_t)raw * 3300 / 4095;    // RP2040 ADC ref = 3.3V
    uint32_t batt_mv = pin_mv * BATT_DIVIDER_RATIO;

    *charging = (gpio_get(PIN_CHRG) == 0);            // TP4056 CHRG low = charging

    int pct = ((int)batt_mv - BATT_MIN_MV) * 100 / (BATT_MAX_MV - BATT_MIN_MV);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    printf("[BATT] raw=%u pin_mv=%lu batt_mv=%lu pct=%d charging=%d\n",
           raw, (unsigned long)pin_mv, (unsigned long)batt_mv, pct, *charging);
    return pct;
}

/* Boot display (Logitech-style): solid battery color for 3s, then settle
 * into the ongoing policy (off / blink-low / breathe-charging). */
static void battery_boot_display(void)
{
    bool charging;
    int pct = battery_read(&charging);

    if (charging) { led_start(LED_BREATHE, 0, 1, 0); return; }

    uint8_t r, g, b;
    battery_color_for_pct((uint8_t)pct, &r, &g, &b);
    led_solid(r, g, b);
    sleep_ms(3000);

    if (pct < BATT_LOW_PCT) led_start(LED_BLINK, 1, 0, 0);
    else                    led_off();
}

/* Periodic re-check: only changes the LED when the state actually flips
 * (charging started/stopped, crossed the low-battery threshold). */
static void battery_task(void)
{
    static uint32_t last_check_ms = 0;
    static bool     first_run = true;
    static bool     was_charging = false;
    static bool     was_low = false;

    uint32_t now = board_millis();
    if (!first_run && (now - last_check_ms < BATT_CHECK_MS)) return;
    last_check_ms = now;
    first_run = false;

    bool charging;
    int pct = battery_read(&charging);
    bool low = (pct < BATT_LOW_PCT);

    if (charging) {
        if (!was_charging) led_start(LED_BREATHE, 0, 1, 0);
    } else if (low) {
        if (!was_low || was_charging) led_start(LED_BLINK, 1, 0, 0);
    } else if (was_charging || was_low) {
        led_off();
    }

    was_charging = charging;
    was_low = low;
}

/*--------------------------------------------------------------------+
 *  Periodic status dump
 *--------------------------------------------------------------------*/
static void status_task(void)
{
    static uint32_t last_print_ms = 0;
    uint32_t now = board_millis();
    if (now - last_print_ms < 3000) return;
    last_print_ms = now;

    printf("[STATUS] uptime=%lus | HID mounted=%d addr=%u | HID reports rx=%lu | SPI frames tx=%lu\n",
           (unsigned long)(now / 1000), kbd_is_mounted, kbd_dev_addr,
           (unsigned long)total_hid_reports_received,
           (unsigned long)total_spi_frames_sent);
}

/*--------------------------------------------------------------------+
 *  USB HID host callbacks
 *--------------------------------------------------------------------*/
void tuh_mount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE MOUNTED: addr=%u ***\n", dev_addr);
    blink_interval_ms = BLINK_MOUNTED;
    tusb_desc_device_t desc;
    if (tuh_descriptor_get_device_sync(dev_addr, &desc, sizeof(desc)) == PICO_ERROR_NONE) {
        printf("[USB] VID=0x%04x PID=0x%04x\n", desc.idVendor, desc.idProduct);
    }
}

void tuh_umount_cb(uint8_t dev_addr)
{
    printf("\n*** [USB] DEVICE UNMOUNTED: addr=%u ***\n", dev_addr);
    blink_interval_ms = BLINK_NOT_MOUNTED;
    if (dev_addr == kbd_dev_addr) { kbd_dev_addr = 0; kbd_is_mounted = false; }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("\n[HID] MOUNTED dev=%u inst=%u protocol=%s\n", dev_addr, instance,
           itf_protocol == HID_ITF_PROTOCOL_KEYBOARD ? "KEYBOARD" :
           itf_protocol == HID_ITF_PROTOCOL_MOUSE    ? "MOUSE"    : "NONE");
    (void) desc_report; (void) desc_len;

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        kbd_dev_addr = dev_addr;
        kbd_instance = instance;
        kbd_is_mounted = true;
        if (!tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT)) {
            printf("[HID] WARNING: failed to force boot protocol\n");
        }
    }
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("[HID][ERROR] Failed to arm receive for dev=%u inst=%u\n", dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("[HID] UNMOUNTED dev=%u inst=%u\n", dev_addr, instance);
    if (dev_addr == kbd_dev_addr && instance == kbd_instance) { kbd_dev_addr = 0; kbd_is_mounted = false; }
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

    if (dev_addr == kbd_dev_addr && instance == kbd_instance && len >= KBD_REPORT_LEN) {
        uint32_t irq = save_and_disable_interrupts();
        memcpy((void *)kbd_report, report, KBD_REPORT_LEN);
        report_dirty = true;
        restore_interrupts(irq);
        printf("[HID RX #%lu] mod=%02x res=%02x keys=%02x %02x %02x %02x %02x %02x\n",
               (unsigned long)total_hid_reports_received,
               report[0], report[1], report[2], report[3],
               report[4], report[5], report[6], report[7]);
    }

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
    tuh_init(BOARD_TUH_RHPORT);
    printf("[INIT] TinyUSB host stack initialized\n");

    spi_master_init();
    led_pwm_init();
    battery_adc_init();

    printf("[INIT] Sending boot battery display...\n");
    battery_boot_display();

    printf("[INIT] Ready. Waiting for keyboard on D+/D-...\n\n");

    while (1) {
        tuh_task();
        led_blinking_task();
        led_tick_task();
        spi_bridge_task();
        battery_task();
        status_task();
    }
}