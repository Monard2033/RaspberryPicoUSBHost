#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"

#define UART_ID uart1
#define UART_TX_PIN 4
#define UART_RX_PIN 5

// GPIO pins for RGB LED with PWM
#define LED_RED_PIN 6
#define LED_GREEN_PIN 7
#define LED_BLUE_PIN 8

// PWM configuration
#define PWM_FREQ 1000 // 1 kHz
#define STEP_DELAY_MS 10
#define MAX_BRIGHTNESS 255

// Define enum led_color globally
typedef enum {
    LED_RED,
    LED_GREEN,
    LED_BLUE,
    LED_OFF
} led_color_t;

static uint8_t a_held = 0, d_held = 0, a_scrip = 0, d_scrip = 0;
static uint8_t w_held = 0, s_held = 0, w_scrip = 0, s_scrip = 0;

static uint slice_num_red, slice_num_green, slice_num_blue;
static bool pulsing = false;
static led_color_t pulse_color = LED_OFF; // Use the typedef
static uint32_t pulse_duration_ms = 0;
static uint8_t pulse_count = 0;
static uint current_step = 0;
static uint total_steps = 0;

void init_uart() {
    uart_init(UART_ID, 1000000);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
}

void init_pwm_led() {
    gpio_set_function(LED_RED_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_GREEN_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_BLUE_PIN, GPIO_FUNC_PWM);

    slice_num_red = pwm_gpio_to_slice_num(LED_RED_PIN);
    slice_num_green = pwm_gpio_to_slice_num(LED_GREEN_PIN);
    slice_num_blue = pwm_gpio_to_slice_num(LED_BLUE_PIN);

    pwm_set_wrap(slice_num_red, MAX_BRIGHTNESS);
    pwm_set_wrap(slice_num_green, MAX_BRIGHTNESS);
    pwm_set_wrap(slice_num_blue, MAX_BRIGHTNESS);

    pwm_set_clkdiv(slice_num_red, 1.0);
    pwm_set_clkdiv(slice_num_green, 1.0);
    pwm_set_clkdiv(slice_num_blue, 1.0);

    pwm_set_enabled(slice_num_red, true);
    pwm_set_enabled(slice_num_green, true);
    pwm_set_enabled(slice_num_blue, true);

    pwm_set_gpio_level(LED_RED_PIN, 0);
    pwm_set_gpio_level(LED_GREEN_PIN, 0);
    pwm_set_gpio_level(LED_BLUE_PIN, 0);
}

void pulse_led(led_color_t color, uint32_t duration_ms, uint8_t pulse_count) { // Use typedef
    if (pulsing && pulse_count >= 1) return; // Avoid queuing if already pulsing with count
    if (pulse_count == 0) {
        pulsing = true;
        pulse_color = color;
        pulse_duration_ms = duration_ms;
        pulse_count = 0;
        current_step = 0;
        total_steps = (pulse_duration_ms + (2 * STEP_DELAY_MS - 1)) / (2 * STEP_DELAY_MS);
        if (total_steps < 10) total_steps = 10;
    } else {
        pulsing = true;
        pulse_color = color;
        pulse_duration_ms = duration_ms;
        pulse_count = pulse_count;
        current_step = 0;
        total_steps = (pulse_duration_ms + (2 * STEP_DELAY_MS - 1)) / (2 * STEP_DELAY_MS);
        if (total_steps < 10) total_steps = 10;
    }
}

void update_pwm() {
    if (!pulsing) return;

    uint brightness = (current_step < total_steps / 2) ?
        (current_step * MAX_BRIGHTNESS) / (total_steps / 2) :
        ((total_steps - current_step) * MAX_BRIGHTNESS) / (total_steps / 2);

    switch (pulse_color) {
        case LED_RED:
            pwm_set_gpio_level(LED_RED_PIN, brightness);
            pwm_set_gpio_level(LED_GREEN_PIN, 0);
            pwm_set_gpio_level(LED_BLUE_PIN, 0);
            break;
        case LED_GREEN:
            pwm_set_gpio_level(LED_RED_PIN, 0);
            pwm_set_gpio_level(LED_GREEN_PIN, brightness);
            pwm_set_gpio_level(LED_BLUE_PIN, 0);
            break;
        case LED_BLUE:
            pwm_set_gpio_level(LED_RED_PIN, 0);
            pwm_set_gpio_level(LED_GREEN_PIN, 0);
            pwm_set_gpio_level(LED_BLUE_PIN, brightness);
            break;
        case LED_OFF:
            pwm_set_gpio_level(LED_RED_PIN, 0);
            pwm_set_gpio_level(LED_GREEN_PIN, 0);
            pwm_set_gpio_level(LED_BLUE_PIN, 0);
            break;
    }

    current_step++;
    if (current_step >= total_steps) {
        if (pulse_count > 0) {
            pulse_count--;
            current_step = 0;
        } else {
            pulsing = false;
            pulse_color = LED_OFF;
        }
    }
}

void send_report_to_nice_nano(uint8_t *report, uint16_t len) {
    uart_write_blocking(UART_ID, report, len);
}

void tud_mount_cb(void) {
    printf("USB Device Connected\n");
    pulse_led(LED_GREEN, 1000, 2); // Green pulse for 1s, 2 times
}

void tud_umount_cb(void) {
    printf("USB Device Disconnected\n");
    pulse_led(LED_RED, 1000, 2); // Red pulse for 1s, 2 times
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len) {
    if (len != 8) return;
    uint8_t modified_report[8];
    memcpy(modified_report, report, 8);

    // Update held states based on current report
    for (int i = 2; i < 8; i++) {
        uint8_t key = modified_report[i];
        if (key == 0x1E) a_held = 1; // A
        else if (key == 0x20) d_held = 1; // D
        else if (key == 0x11) w_held = 1; // W
        else if (key == 0x1F) s_held = 1; // S
    }

    // A/D Logic
    if (a_held && d_scrip) {
        d_scrip = 0;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x20) modified_report[i] = 0;
    }
    if (a_held && !a_scrip) {
        a_scrip = 1;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x1E) modified_report[i] = 0x1E;
    }
    if (!a_held && d_held && !d_scrip) {
        d_scrip = 1;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x20) modified_report[i] = 0x20;
    }

    // W/S Logic
    if (w_held && s_scrip) {
        s_scrip = 0;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x1F) modified_report[i] = 0;
    }
    if (w_held && !w_scrip) {
        w_scrip = 1;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x11) modified_report[i] = 0x11;
    }
    if (!w_held && s_held && !s_scrip) {
        s_scrip = 1;
        for (int i = 2; i < 8; i++) if (modified_report[i] == 0x1F) modified_report[i] = 0x1F;
    }

    // Release logic on key up
    for (int i = 2; i < 8; i++) {
        if (modified_report[i] == 0) {
            if (modified_report[i-1] == 0x1E) a_held = 0, a_scrip = 0;
            else if (modified_report[i-1] == 0x20) d_held = 0, d_scrip = 0;
            else if (modified_report[i-1] == 0x11) w_held = 0, w_scrip = 0;
            else if (modified_report[i-1] == 0x1F) s_held = 0, s_scrip = 0;
        }
    }

    send_report_to_nice_nano(modified_report, 8);
    printf("HID Report: ");
    for (int i = 0; i < len; i++) printf("%02x ", modified_report[i]);
    printf("\n");
    pulse_led(LED_GREEN, 500, 1); // Green pulse for 0.5s, 1 time on report
}

int main() {
    stdio_init_all(); // Enable USB serial output
    init_uart();
    init_pwm_led();
    pulse_led(LED_BLUE, 1000, 2); // Blue pulse for 1s, 2 times on startup
    tusb_init();
    while (1) {
        tud_task();
        update_pwm();
        if (!tud_mounted()) pulse_led(LED_RED, 1000, 2); // Red pulse if disconnected
        sleep_ms(STEP_DELAY_MS); // Control pulse timing
    }
    return 0;
}