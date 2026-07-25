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
#define MAX_BRIGHTNESS 255 // For finer control

// Define MIN_PERIOD and MAX_PERIOD
#define PWM_SEC(us) (us * 1000000U)
#define PWM_MSEC(ms) (ms * 1000U)
#define MIN_PERIOD PWM_MSEC(1U) / 128U // 1 second divided by 128 for 128 steps
#define MAX_PERIOD PWM_MSEC(50U) // 20 ms for 50 Hz PWM

// Enum for LED colors
typedef enum {
    LED_RED,
    LED_GREEN,
    LED_BLUE,
    LED_YELLOW,
    LED_ORANGE,
    LED_MAGENTA,
    LED_WHITE,
    LED_OFF
} led_color_t;

// Pulse queue entry
struct pulse_queue_entry {
    led_color_t color;
    uint32_t duration_ms;
    uint8_t count;
    bool active;
};

static struct pulse_queue_entry pulse_queue[7];
static uint8_t queue_index = 0;
static bool pulsing = false;
static led_color_t pulse_color = LED_OFF;
static uint32_t pulse_duration_ms = 0;
static uint8_t pulses = 0;
static uint8_t remaining_pulses = 0;
static uint32_t current_step = 0;
static uint32_t total_steps = 0;
static uint32_t total_execution_ms = 0;
static uint32_t grace_period_ms = 100; // Assume 100ms grace period
static uint32_t step_delay = STEP_DELAY_MS;

static uint8_t a_held = 0, d_held = 0, a_scrip = 0, d_scrip = 0;
static uint8_t w_held = 0, s_held = 0, w_scrip = 0, s_scrip = 0;
static uint slice_num_red, slice_num_green, slice_num_blue;

// Repeating timer for pulse
static repeating_timer_t pulse_timer;

static bool pulse_timer_handler(repeating_timer_t *rt) {
    if (!pulsing) {
        return false; // Stop timer
    }

    uint32_t red_pulse = 0, green_pulse = 0, blue_pulse = 0;
    uint32_t total_pulse_steps = total_steps * 2;
    uint32_t current_pulse = current_step / total_pulse_steps;
    uint32_t step_in_pulse = current_step % total_pulse_steps;
    bool is_infinite_pulse = (pulses == 0);

    if (is_infinite_pulse || (!is_infinite_pulse && current_pulse < pulses)) {
        if (step_in_pulse < total_steps) {
            uint32_t duty = (step_in_pulse * MAX_PERIOD) / total_steps;
            switch (pulse_color) {
                case LED_RED: red_pulse = duty; break;
                case LED_GREEN: green_pulse = duty; break;
                case LED_BLUE: blue_pulse = duty; break;
                case LED_YELLOW: red_pulse = duty; green_pulse = duty; break;
                case LED_ORANGE: red_pulse = duty; green_pulse = duty / 2; break;
                case LED_MAGENTA: red_pulse = duty; blue_pulse = duty; break;
                case LED_WHITE: red_pulse = duty; green_pulse = duty; blue_pulse = duty; break;
                case LED_OFF:
                    red_pulse = 0; green_pulse = 0; blue_pulse = 0;
                    if (is_infinite_pulse) {
                        pulsing = false;
                        current_step = 0;
                        return false; // Stop timer
                    }
                    break;
                default: break;
            }
        } else if (step_in_pulse < total_pulse_steps) {
            uint32_t duty = MAX_PERIOD - ((step_in_pulse - total_steps) * MAX_PERIOD) / total_steps;
            // Fix dim issue: Set to 0 if below MIN_PERIOD
            if (duty < MIN_PERIOD) duty = 0;
            switch (pulse_color) {
                case LED_RED: red_pulse = duty; break;
                case LED_GREEN: green_pulse = duty; break;
                case LED_BLUE: blue_pulse = duty; break;
                case LED_YELLOW: red_pulse = duty; green_pulse = duty; break;
                case LED_ORANGE: red_pulse = duty; green_pulse = duty / 2; break;
                case LED_MAGENTA: red_pulse = duty; blue_pulse = duty; break;
                case LED_WHITE: red_pulse = duty; green_pulse = duty; blue_pulse = duty; break;
                case LED_OFF:
                    red_pulse = 0; green_pulse = 0; blue_pulse = 0;
                    if (is_infinite_pulse) {
                        pulsing = false;
                        current_step = 0;
                        return false; // Stop timer
                    }
                    break;
                default: break;
            }
        }
    } else if (!is_infinite_pulse && (current_step - (pulses * total_pulse_steps)) * step_delay < grace_period_ms) {
        red_pulse = 0; green_pulse = 0; blue_pulse = 0;
    } else {
        pulsing = false;
        current_step = 0;
        pwm_set_gpio_level(LED_RED_PIN, 0);
        pwm_set_gpio_level(LED_GREEN_PIN, 0);
        pwm_set_gpio_level(LED_BLUE_PIN, 0);
        if (queue_index > 0) {
            for (uint8_t i = 0; i < queue_index - 1; i++) {
                pulse_queue[i] = pulse_queue[i + 1];
            }
            queue_index--;
            if (queue_index > 0) {
                pulsing = true;
                pulse_color = pulse_queue[0].color;
                pulse_duration_ms = pulse_queue[0].duration_ms;
                pulses = pulse_queue[0].count;
                remaining_pulses = pulses;
                current_step = 0;
                total_steps = (pulse_duration_ms + (2 * step_delay - 1)) / (2 * step_delay);
                if (total_steps < 10) total_steps = 10;
                total_execution_ms = pulse_duration_ms * pulses + grace_period_ms;
                pulse_queue[0].active = true;
                return true; // Continue timer
            }
        }
        return false; // Stop timer
    }

    pwm_set_gpio_level(LED_RED_PIN, red_pulse);
    pwm_set_gpio_level(LED_GREEN_PIN, green_pulse);
    pwm_set_gpio_level(LED_BLUE_PIN, blue_pulse);
    current_step++;

    return true; // Continue timer
}
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

    pwm_set_clkdiv(slice_num_red, 128.0f); // Adjust for freq
    pwm_set_clkdiv(slice_num_green, 128.0f);
    pwm_set_clkdiv(slice_num_blue, 128.0f);

    pwm_set_enabled(slice_num_red, true);
    pwm_set_enabled(slice_num_green, true);
    pwm_set_enabled(slice_num_blue, true);
/* 
    pwm_set_gpio_level(LED_RED_PIN, 0);
    pwm_set_gpio_level(LED_GREEN_PIN, 0);
    pwm_set_gpio_level(LED_BLUE_PIN, 0); */
}

void pulse_led(led_color_t color, uint32_t duration_ms, uint8_t pulse_count) {
    if (pulsing && queue_index >= 7 && pulse_count >= 1) return;
    if (pulse_count == 0) {
        if (pulsing && pulse_color == LED_OFF) return;
        pulsing = true;
        pulse_color = color;
        pulse_duration_ms = duration_ms;
        pulses = 0;
        current_step = 0;
        total_steps = (pulse_duration_ms + (2 * step_delay - 1)) / (2 * step_delay);
        if (total_steps < 10) total_steps = 10;
        add_repeating_timer_ms(step_delay, pulse_timer_handler, NULL, &pulse_timer);
        return;
    }

    pulse_queue[queue_index].color = color;
    pulse_queue[queue_index].duration_ms = duration_ms;
    pulse_queue[queue_index].count = pulse_count;
    pulse_queue[queue_index].active = false;
    queue_index++;

    if (!pulsing) {
        pulsing = true;
        pulse_color = pulse_queue[0].color;
        pulse_duration_ms = pulse_queue[0].duration_ms;
        pulses = pulse_queue[0].count;
        remaining_pulses = pulses;
        current_step = 0;
        total_steps = (pulse_duration_ms + (2 * step_delay - 1)) / (2 * step_delay);
        if (total_steps < 10) total_steps = 10;
        total_execution_ms = pulse_duration_ms * pulses + grace_period_ms;
        pulse_queue[0].active = true;
        add_repeating_timer_ms(step_delay, pulse_timer_handler, NULL, &pulse_timer);
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

    // W/S logic
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
    uart_init(UART_ID, 115200);
    init_pwm_led();
    pulse_led(LED_BLUE, 1000, 2); // Blue pulse for 1s, 2 times on startup
    tusb_init();
    while (1) {
        tud_task();
        if (!tud_mounted()) pulse_led(LED_RED, 1000, 2); // Red pulse if disconnected
    }
    return 0;
}