#include "pico_hat_diag.h"

#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define PICO_LCD_SPI spi1
#define PICO_LCD_PIN_DC 8u
#define PICO_LCD_PIN_CS 9u

#define DIAG_SIZE 30u
#define DIAG_ACTIVE_COLOR 0xF81Fu // magenta, distinct from the quadrant baseline

typedef struct {
    uint16_t x;
    uint16_t y;
} diag_pos_t;

static const diag_pos_t g_diag_positions[PICO_HAT_INPUT_COUNT] = {
    [PICO_HAT_INPUT_JOY_UP]    = {105u,   8u},
    [PICO_HAT_INPUT_JOY_DOWN]  = {105u, 202u},
    [PICO_HAT_INPUT_JOY_LEFT]  = {  8u, 105u},
    [PICO_HAT_INPUT_JOY_RIGHT] = {202u, 105u},
    [PICO_HAT_INPUT_JOY_PRESS] = {105u, 105u},
    [PICO_HAT_INPUT_KEY1]      = {158u,  22u},
    [PICO_HAT_INPUT_KEY2]      = {158u,  72u},
    [PICO_HAT_INPUT_KEY3]      = {158u, 122u},
    [PICO_HAT_INPUT_KEY4]      = {  0u,   0u},
};

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
    gpio_put(PICO_LCD_PIN_DC, true);
    lcd_select(true);
    spi_write_blocking(PICO_LCD_SPI, data, len);
    lcd_select(false);
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

static uint16_t baseline_color(uint16_t x, uint16_t y)
{
    if (y < 120u) {
        return x < 120u ? 0xF800u : 0x07E0u;
    }
    return x < 120u ? 0x001Fu : 0xFFFFu;
}

static void draw_marker(uint16_t x0, uint16_t y0, bool active)
{
    uint8_t line[DIAG_SIZE * 2u];
    const uint16_t x1 = (uint16_t)(x0 + DIAG_SIZE);
    const uint16_t y1 = (uint16_t)(y0 + DIAG_SIZE);

    lcd_set_window(x0, y0, x1, y1);
    gpio_put(PICO_LCD_PIN_DC, true);
    lcd_select(true);

    for (uint16_t y = y0; y < y1; ++y) {
        for (uint16_t x = x0; x < x1; ++x) {
            const uint16_t color = active ? DIAG_ACTIVE_COLOR : baseline_color(x, y);
            const size_t offset = (size_t)(x - x0) * 2u;
            line[offset] = (uint8_t)(color >> 8);
            line[offset + 1u] = (uint8_t)color;
        }
        spi_write_blocking(PICO_LCD_SPI, line, sizeof(line));
    }

    lcd_select(false);
}

void pico_hat_diag_handle_event(const pico_hat_event_t *event)
{
    if (event == NULL || event->input >= PICO_HAT_INPUT_COUNT) {
        return;
    }

    // KEY4 owns the screen lock/backlight path; drawing while it is toggling is
    // intentionally avoided. Also ignore very early events before LCD init has
    // had enough time to complete.
    if (event->input == PICO_HAT_INPUT_KEY4 || to_ms_since_boot(get_absolute_time()) < 1000u) {
        return;
    }

    const diag_pos_t pos = g_diag_positions[event->input];
    draw_marker(pos.x, pos.y, event->pressed);
}
