#include "pico_hat_ui.h"

#include <stddef.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define PICO_LCD_SPI spi1
#define PICO_LCD_SPI_BAUD_HZ (10u * 1000u * 1000u)

#define PICO_LCD_WIDTH  240u
#define PICO_LCD_HEIGHT 240u

#define PICO_LCD_PIN_DC   8u
#define PICO_LCD_PIN_CS   9u
#define PICO_LCD_PIN_SCK 10u
#define PICO_LCD_PIN_MOSI 11u
#define PICO_LCD_PIN_RST 12u
#define PICO_LCD_PIN_BL  13u

// Provisional logical KEY1..KEY4 mapping for the physical validation gate.
// Waveshare labels these buttons A/B/X/Y. PICO-03 must confirm the desired
// physical orientation before this mapping becomes release baseline.
#define PICO_LCD_PIN_KEY1 15u  // A
#define PICO_LCD_PIN_KEY2 17u  // B
#define PICO_LCD_PIN_KEY3 19u  // X
#define PICO_LCD_PIN_KEY4 21u  // Y

#define PICO_LCD_PIN_JOY_UP    2u
#define PICO_LCD_PIN_JOY_DOWN 18u
#define PICO_LCD_PIN_JOY_LEFT 16u
#define PICO_LCD_PIN_JOY_RIGHT 20u
#define PICO_LCD_PIN_JOY_PRESS 3u

#define INPUT_DEBOUNCE_MS 20u
#define INPUT_SCAN_PERIOD_MS 1u
#define LCD_RESET_STAGE_MS 100u
#define LCD_SLEEP_OUT_MS 120u
#define LCD_TEST_ROW_PERIOD_MS 2u
#define LOCAL_EVENT_QUEUE_LEN 16u

typedef struct {
    pico_hat_input_t id;
    uint gpio;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
} input_state_t;

typedef enum {
    LCD_INIT_RESET_HIGH = 0,
    LCD_INIT_RESET_LOW,
    LCD_INIT_RESET_RELEASE,
    LCD_INIT_WAIT_SLEEP_OUT,
    LCD_INIT_READY
} lcd_init_state_t;

static input_state_t g_inputs[PICO_HAT_INPUT_COUNT] = {
    { PICO_HAT_INPUT_JOY_UP,    PICO_LCD_PIN_JOY_UP,    false, false, 0 },
    { PICO_HAT_INPUT_JOY_DOWN,  PICO_LCD_PIN_JOY_DOWN,  false, false, 0 },
    { PICO_HAT_INPUT_JOY_LEFT,  PICO_LCD_PIN_JOY_LEFT,  false, false, 0 },
    { PICO_HAT_INPUT_JOY_RIGHT, PICO_LCD_PIN_JOY_RIGHT, false, false, 0 },
    { PICO_HAT_INPUT_JOY_PRESS, PICO_LCD_PIN_JOY_PRESS, false, false, 0 },
    { PICO_HAT_INPUT_KEY1,      PICO_LCD_PIN_KEY1,      false, false, 0 },
    { PICO_HAT_INPUT_KEY2,      PICO_LCD_PIN_KEY2,      false, false, 0 },
    { PICO_HAT_INPUT_KEY3,      PICO_LCD_PIN_KEY3,      false, false, 0 },
    { PICO_HAT_INPUT_KEY4,      PICO_LCD_PIN_KEY4,      false, false, 0 },
};

static pico_hat_event_t g_event_queue[LOCAL_EVENT_QUEUE_LEN];
static uint8_t g_event_read;
static uint8_t g_event_write;
static bool g_screen_locked;
static bool g_unlock_quarantine;
static uint32_t g_last_input_scan_ms;

static lcd_init_state_t g_lcd_state;
static uint32_t g_lcd_deadline_ms;
static uint16_t g_test_row;
static uint32_t g_last_test_row_ms;

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void lcd_select(bool selected)
{
    gpio_put(PICO_LCD_PIN_CS, !selected);
}

static void lcd_write_command(uint8_t command)
{
    gpio_put(PICO_LCD_PIN_DC, false);
    lcd_select(true);
    spi_write_blocking(PICO_LCD_SPI, &command, 1);
    lcd_select(false);
}

static void lcd_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    gpio_put(PICO_LCD_PIN_DC, true);
    lcd_select(true);
    spi_write_blocking(PICO_LCD_SPI, data, len);
    lcd_select(false);
}

static void lcd_write_u8(uint8_t value)
{
    lcd_write_data(&value, 1);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t coords[4];

    lcd_write_command(0x2A);
    coords[0] = (uint8_t)(x0 >> 8);
    coords[1] = (uint8_t)x0;
    coords[2] = (uint8_t)((x1 - 1u) >> 8);
    coords[3] = (uint8_t)(x1 - 1u);
    lcd_write_data(coords, sizeof(coords));

    lcd_write_command(0x2B);
    coords[0] = (uint8_t)(y0 >> 8);
    coords[1] = (uint8_t)y0;
    coords[2] = (uint8_t)((y1 - 1u) >> 8);
    coords[3] = (uint8_t)(y1 - 1u);
    lcd_write_data(coords, sizeof(coords));

    lcd_write_command(0x2C);
}

static void lcd_configure_panel(void)
{
    // Match the Waveshare Pico-LCD-1.3 panel orientation and RGB565 format.
    lcd_write_command(0x36); // MADCTL
    lcd_write_u8(0x70);

    lcd_write_command(0x3A); // COLMOD: 16-bit RGB565
    lcd_write_u8(0x05);

    lcd_write_command(0xB2);
    const uint8_t porctrl[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    lcd_write_data(porctrl, sizeof(porctrl));

    lcd_write_command(0xB7);
    lcd_write_u8(0x35);

    lcd_write_command(0xBB);
    lcd_write_u8(0x19);

    lcd_write_command(0xC0);
    lcd_write_u8(0x2C);

    lcd_write_command(0xC2);
    lcd_write_u8(0x01);

    lcd_write_command(0xC3);
    lcd_write_u8(0x12);

    lcd_write_command(0xC4);
    lcd_write_u8(0x20);

    lcd_write_command(0xC6);
    lcd_write_u8(0x0F);

    lcd_write_command(0xD0);
    const uint8_t pwctrl[] = { 0xA4, 0xA1 };
    lcd_write_data(pwctrl, sizeof(pwctrl));

    lcd_write_command(0xE0);
    const uint8_t gamma_pos[] = {
        0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
        0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23
    };
    lcd_write_data(gamma_pos, sizeof(gamma_pos));

    lcd_write_command(0xE1);
    const uint8_t gamma_neg[] = {
        0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
        0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23
    };
    lcd_write_data(gamma_neg, sizeof(gamma_neg));

    lcd_write_command(0x21); // inversion on
    lcd_write_command(0x11); // sleep out
}

static void lcd_write_test_row(uint16_t row)
{
    static uint8_t row_buf[PICO_LCD_WIDTH * 2u];

    for (uint16_t x = 0; x < PICO_LCD_WIDTH; ++x) {
        uint16_t color;

        if (row < (PICO_LCD_HEIGHT / 2u)) {
            color = (x < (PICO_LCD_WIDTH / 2u)) ? 0xF800u : 0x07E0u;
        } else {
            color = (x < (PICO_LCD_WIDTH / 2u)) ? 0x001Fu : 0xFFFFu;
        }

        // Two asymmetric markers make rotation/mirroring visible during the
        // physical gate: black at top-left, yellow at bottom-right.
        if (row < 20u && x < 20u) {
            color = 0x0000u;
        } else if (row >= 220u && x >= 220u) {
            color = 0xFFE0u;
        }

        row_buf[(size_t)x * 2u] = (uint8_t)(color >> 8);
        row_buf[(size_t)x * 2u + 1u] = (uint8_t)color;
    }

    lcd_set_window(0, row, PICO_LCD_WIDTH, (uint16_t)(row + 1u));
    lcd_write_data(row_buf, sizeof(row_buf));
}

static void set_backlight(bool enabled)
{
    // PICO-03 requires a true backlight-off lock. Brightness/PWM can be added
    // later without changing the lock semantics.
    gpio_put(PICO_LCD_PIN_BL, enabled);
}

static bool enqueue_event(pico_hat_input_t input, bool pressed)
{
    const uint8_t next = (uint8_t)((g_event_write + 1u) % LOCAL_EVENT_QUEUE_LEN);
    if (next == g_event_read) {
        return false;
    }

    g_event_queue[g_event_write].input = input;
    g_event_queue[g_event_write].pressed = pressed;
    g_event_write = next;
    return true;
}

static bool all_non_lock_inputs_released(void)
{
    for (size_t i = 0; i < PICO_HAT_INPUT_COUNT; ++i) {
        if (g_inputs[i].id == PICO_HAT_INPUT_KEY4) {
            continue;
        }
        if (g_inputs[i].stable_pressed) {
            return false;
        }
    }
    return true;
}

static void handle_debounced_transition(input_state_t *input, bool pressed)
{
    if (input->id == PICO_HAT_INPUT_KEY4 && pressed) {
        g_screen_locked = !g_screen_locked;
        set_backlight(!g_screen_locked);

        if (!g_screen_locked) {
            // Ignore orphan transitions from controls that were manipulated
            // while locked until every non-lock control has been released.
            g_unlock_quarantine = true;
        }
    }

    if (input->id == PICO_HAT_INPUT_KEY4) {
        (void)enqueue_event(input->id, pressed);
        return;
    }

    if (g_screen_locked || g_unlock_quarantine) {
        return;
    }

    (void)enqueue_event(input->id, pressed);
}

static void scan_inputs(uint32_t now)
{
    if ((uint32_t)(now - g_last_input_scan_ms) < INPUT_SCAN_PERIOD_MS) {
        return;
    }
    g_last_input_scan_ms = now;

    for (size_t i = 0; i < PICO_HAT_INPUT_COUNT; ++i) {
        input_state_t *input = &g_inputs[i];
        const bool pressed = !gpio_get(input->gpio); // active-low + pull-up

        if (pressed != input->raw_pressed) {
            input->raw_pressed = pressed;
            input->raw_changed_ms = now;
        }

        if (pressed != input->stable_pressed &&
            (uint32_t)(now - input->raw_changed_ms) >= INPUT_DEBOUNCE_MS) {
            input->stable_pressed = pressed;
            handle_debounced_transition(input, pressed);
        }
    }

    if (g_unlock_quarantine && all_non_lock_inputs_released()) {
        g_unlock_quarantine = false;
    }
}

static void service_lcd(uint32_t now)
{
    switch (g_lcd_state) {
        case LCD_INIT_RESET_HIGH:
            if (deadline_reached(now, g_lcd_deadline_ms)) {
                gpio_put(PICO_LCD_PIN_RST, false);
                g_lcd_state = LCD_INIT_RESET_LOW;
                g_lcd_deadline_ms = now + LCD_RESET_STAGE_MS;
            }
            break;

        case LCD_INIT_RESET_LOW:
            if (deadline_reached(now, g_lcd_deadline_ms)) {
                gpio_put(PICO_LCD_PIN_RST, true);
                g_lcd_state = LCD_INIT_RESET_RELEASE;
                g_lcd_deadline_ms = now + LCD_RESET_STAGE_MS;
            }
            break;

        case LCD_INIT_RESET_RELEASE:
            if (deadline_reached(now, g_lcd_deadline_ms)) {
                lcd_configure_panel();
                g_lcd_state = LCD_INIT_WAIT_SLEEP_OUT;
                g_lcd_deadline_ms = now + LCD_SLEEP_OUT_MS;
            }
            break;

        case LCD_INIT_WAIT_SLEEP_OUT:
            if (deadline_reached(now, g_lcd_deadline_ms)) {
                lcd_write_command(0x29); // display on
                g_lcd_state = LCD_INIT_READY;
                g_test_row = 0;
                g_last_test_row_ms = now;
                printf("[PICO-03] LCD ready; drawing cooperative test pattern\r\n");
            }
            break;

        case LCD_INIT_READY:
            if (!g_screen_locked && g_test_row < PICO_LCD_HEIGHT &&
                (uint32_t)(now - g_last_test_row_ms) >= LCD_TEST_ROW_PERIOD_MS) {
                g_last_test_row_ms = now;
                lcd_write_test_row(g_test_row++);
                if (g_test_row == PICO_LCD_HEIGHT) {
                    printf("[PICO-03] LCD test pattern complete\r\n");
                }
            }
            break;

        default:
            break;
    }
}

void pico_hat_ui_init(void)
{
    spi_init(PICO_LCD_SPI, PICO_LCD_SPI_BAUD_HZ);
    gpio_set_function(PICO_LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PICO_LCD_PIN_MOSI, GPIO_FUNC_SPI);

    const uint output_pins[] = {
        PICO_LCD_PIN_DC,
        PICO_LCD_PIN_CS,
        PICO_LCD_PIN_RST,
        PICO_LCD_PIN_BL,
    };

    for (size_t i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        gpio_init(output_pins[i]);
        gpio_set_dir(output_pins[i], GPIO_OUT);
    }

    gpio_put(PICO_LCD_PIN_CS, true);
    gpio_put(PICO_LCD_PIN_DC, false);
    gpio_put(PICO_LCD_PIN_RST, true);
    set_backlight(true);

    const uint32_t now = now_ms();
    for (size_t i = 0; i < PICO_HAT_INPUT_COUNT; ++i) {
        input_state_t *input = &g_inputs[i];
        gpio_init(input->gpio);
        gpio_set_dir(input->gpio, GPIO_IN);
        gpio_pull_up(input->gpio);

        const bool pressed = !gpio_get(input->gpio);
        input->raw_pressed = pressed;
        input->stable_pressed = pressed;
        input->raw_changed_ms = now;
    }

    g_event_read = 0;
    g_event_write = 0;
    g_screen_locked = false;
    g_unlock_quarantine = false;
    g_last_input_scan_ms = now;

    g_lcd_state = LCD_INIT_RESET_HIGH;
    g_lcd_deadline_ms = now + LCD_RESET_STAGE_MS;
    g_test_row = 0;
    g_last_test_row_ms = now;

    printf("[PICO-03] Waveshare Pico-LCD-1.3 baseline init\r\n");
    printf("[PICO-03] provisional keys: A=KEY1 B=KEY2 X=KEY3 Y=KEY4(lock)\r\n");
}

void pico_hat_ui_task(void)
{
    const uint32_t now = now_ms();
    scan_inputs(now);
    service_lcd(now);
}

bool pico_hat_ui_poll_event(pico_hat_event_t *event)
{
    if (event == NULL || g_event_read == g_event_write) {
        return false;
    }

    *event = g_event_queue[g_event_read];
    g_event_read = (uint8_t)((g_event_read + 1u) % LOCAL_EVENT_QUEUE_LEN);
    return true;
}

bool pico_hat_ui_is_screen_locked(void)
{
    return g_screen_locked;
}

const char *pico_hat_ui_input_name(pico_hat_input_t input)
{
    static const char *const names[PICO_HAT_INPUT_COUNT] = {
        "JOY_UP",
        "JOY_DOWN",
        "JOY_LEFT",
        "JOY_RIGHT",
        "JOY_PRESS",
        "KEY1/A",
        "KEY2/B",
        "KEY3/X",
        "KEY4/Y",
    };

    if ((unsigned)input >= PICO_HAT_INPUT_COUNT) {
        return "UNKNOWN";
    }
    return names[input];
}
