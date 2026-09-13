#include "pico05_status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico_hat_ui.h"

#define LCD_SPI spi1
#define LCD_PIN_DC 8u
#define LCD_PIN_CS 9u
#define LCD_WIDTH 240u
#define LCD_HEIGHT 240u

#define STATUS_LINE_COUNT 7u
#define STATUS_TEXT_MAX 20u
#define GLYPH_SCALE 2u
#define GLYPH_W 5u
#define GLYPH_H 7u
#define DRAW_W (GLYPH_W * GLYPH_SCALE)
#define DRAW_H (GLYPH_H * GLYPH_SCALE)
#define CHAR_ADVANCE 12u
#define LINE_ADVANCE 28u
#define TEXT_X 8u
#define TEXT_Y 12u

typedef enum {
    STATUS_WAIT_LCD = 0,
    STATUS_CLEAR,
    STATUS_TEXT,
    STATUS_IDLE,
} status_phase_t;

static status_phase_t g_phase;
static device_profile_snapshot_t g_snapshot;
static bool g_have_snapshot;
static uint32_t g_seen_revision;
static uint16_t g_clear_row;
static uint8_t g_text_line;
static uint8_t g_text_char;
static char g_lines[STATUS_LINE_COUNT][STATUS_TEXT_MAX + 1u];

// 5x7 row font. Each row uses bits 4..0 from left to right.
static const uint8_t k_alpha[26][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, // A
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, // B
    {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f}, // C
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}, // D
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, // E
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, // F
    {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f}, // G
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, // H
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, // I
    {0x07,0x02,0x02,0x02,0x12,0x12,0x0c}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, // L
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, // O
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, // P
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, // Q
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, // R
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, // S
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, // W
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, // X
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, // Y
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, // Z
};

static const uint8_t k_digit[10][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
    {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
    {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e},
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
    {0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
};

static uint8_t glyph_row(char c, uint8_t row)
{
    if (row >= GLYPH_H) return 0;
    if (c >= 'A' && c <= 'Z') return k_alpha[(uint8_t)(c - 'A')][row];
    if (c >= '0' && c <= '9') return k_digit[(uint8_t)(c - '0')][row];
    if (c == '-') return row == 3u ? 0x1fu : 0u;
    if (c == ':') return (row == 2u || row == 5u) ? 0x04u : 0u;
    if (c == '.') return row == 6u ? 0x04u : 0u;
    return 0;
}

static void lcd_select(bool selected)
{
    gpio_put(LCD_PIN_CS, !selected);
}

static void lcd_command(uint8_t command)
{
    gpio_put(LCD_PIN_DC, false);
    lcd_select(true);
    spi_write_blocking(LCD_SPI, &command, 1);
    lcd_select(false);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0u) return;
    gpio_put(LCD_PIN_DC, true);
    lcd_select(true);
    spi_write_blocking(LCD_SPI, data, len);
    lcd_select(false);
}

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t coords[4];
    lcd_command(0x2a);
    coords[0] = (uint8_t)(x0 >> 8);
    coords[1] = (uint8_t)x0;
    coords[2] = (uint8_t)((x1 - 1u) >> 8);
    coords[3] = (uint8_t)(x1 - 1u);
    lcd_data(coords, sizeof(coords));

    lcd_command(0x2b);
    coords[0] = (uint8_t)(y0 >> 8);
    coords[1] = (uint8_t)y0;
    coords[2] = (uint8_t)((y1 - 1u) >> 8);
    coords[3] = (uint8_t)(y1 - 1u);
    lcd_data(coords, sizeof(coords));
    lcd_command(0x2c);
}

static void clear_one_row(uint16_t row)
{
    static uint8_t black[LCD_WIDTH * 2u];
    lcd_window(0u, row, LCD_WIDTH, (uint16_t)(row + 1u));
    lcd_data(black, sizeof(black));
}

static void draw_glyph(uint16_t x, uint16_t y, char c)
{
    static uint8_t pixels[DRAW_W * DRAW_H * 2u];
    size_t out = 0;

    for (uint8_t py = 0; py < DRAW_H; ++py) {
        const uint8_t bits = glyph_row(c, (uint8_t)(py / GLYPH_SCALE));
        for (uint8_t px = 0; px < DRAW_W; ++px) {
            const uint8_t source_x = (uint8_t)(px / GLYPH_SCALE);
            const bool on = (bits & (uint8_t)(1u << (4u - source_x))) != 0;
            const uint16_t color = on ? 0xffffu : 0x0000u;
            pixels[out++] = (uint8_t)(color >> 8);
            pixels[out++] = (uint8_t)color;
        }
    }

    lcd_window(x, y, (uint16_t)(x + DRAW_W), (uint16_t)(y + DRAW_H));
    lcd_data(pixels, sizeof(pixels));
}

static const char *profile_name(device_profile_mode_t mode)
{
    switch (mode) {
        case DEVICE_PROFILE_DEFAULT_REMAP: return "DEFAULT";
        case DEVICE_PROFILE_CUSTOM_REMAP: return "CUSTOM";
        case DEVICE_PROFILE_PASSTHROUGH:
        default: return "PASSTHRU";
    }
}

static const char *backend_name(device_drag_backend_t backend)
{
    switch (backend) {
        case DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4: return "HIDPP";
        case DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4: return "PROBE";
        case DEVICE_DRAG_BACKEND_UNSUPPORTED: return "UNSUPPORTED";
        case DEVICE_DRAG_BACKEND_STANDARD:
        default: return "STD";
    }
}

static void build_lines(void)
{
    memset(g_lines, 0, sizeof(g_lines));
    snprintf(g_lines[0], sizeof(g_lines[0]), "PICO-05");

    if (!g_have_snapshot || !g_snapshot.connected) {
        snprintf(g_lines[1], sizeof(g_lines[1]), "WAITING FOR MOUSE");
        snprintf(g_lines[2], sizeof(g_lines[2]), "PROFILE PASSTHRU");
        snprintf(g_lines[3], sizeof(g_lines[3]), "USB HID STABLE");
        return;
    }

    snprintf(g_lines[1], sizeof(g_lines[1]), "PROFILE %s", profile_name(g_snapshot.profile_mode));
    snprintf(g_lines[2], sizeof(g_lines[2]), "RECORD %s",
             g_snapshot.profile_restored ? "RESTORED" : "NEW");

    if (!g_snapshot.pnp_query_complete) {
        snprintf(g_lines[3], sizeof(g_lines[3]), "PNP QUERYING");
    } else if (g_snapshot.pnp_valid) {
        snprintf(g_lines[3], sizeof(g_lines[3]), "VID %04X PID %04X",
                 g_snapshot.vendor_id, g_snapshot.product_id);
    } else {
        snprintf(g_lines[3], sizeof(g_lines[3]), "PNP NOT FOUND");
    }

    snprintf(g_lines[4], sizeof(g_lines[4]), "FWD AUTO %s",
             backend_name(g_snapshot.auto_forward_backend));
    snprintf(g_lines[5], sizeof(g_lines[5]), "BACK AUTO %s",
             backend_name(g_snapshot.auto_back_backend));
    snprintf(g_lines[6], sizeof(g_lines[6]), "STORAGE %s",
             g_snapshot.storage_ok ? "OK" : "ERROR");
}

static void begin_render(void)
{
    build_lines();
    g_clear_row = 0;
    g_text_line = 0;
    g_text_char = 0;
    g_phase = STATUS_CLEAR;
}

void pico05_status_init(void)
{
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_have_snapshot = false;
    g_seen_revision = UINT32_MAX;
    g_clear_row = 0;
    g_text_line = 0;
    g_text_char = 0;
    g_phase = STATUS_WAIT_LCD;
}

void pico05_status_task(void)
{
    if (!pico_hat_ui_is_lcd_ready()) {
        return;
    }

    if (pico_hat_ui_is_screen_locked()) {
        return;
    }

    device_profile_snapshot_t current;
    const bool have_current = device_profile_get_snapshot(&current);
    const uint32_t current_revision = have_current ? current.revision : 0u;
    if (g_seen_revision == UINT32_MAX || current_revision != g_seen_revision) {
        g_seen_revision = current_revision;
        g_have_snapshot = have_current;
        if (have_current) memcpy(&g_snapshot, &current, sizeof(g_snapshot));
        else memset(&g_snapshot, 0, sizeof(g_snapshot));
        begin_render();
    }

    switch (g_phase) {
        case STATUS_WAIT_LCD:
            begin_render();
            break;

        case STATUS_CLEAR:
            clear_one_row(g_clear_row++);
            if (g_clear_row >= LCD_HEIGHT) {
                g_phase = STATUS_TEXT;
            }
            break;

        case STATUS_TEXT: {
            while (g_text_line < STATUS_LINE_COUNT &&
                   g_lines[g_text_line][g_text_char] == '\0') {
                ++g_text_line;
                g_text_char = 0;
            }
            if (g_text_line >= STATUS_LINE_COUNT) {
                g_phase = STATUS_IDLE;
                break;
            }

            const char c = g_lines[g_text_line][g_text_char];
            if (c != ' ') {
                const uint16_t x = (uint16_t)(TEXT_X + (uint16_t)g_text_char * CHAR_ADVANCE);
                const uint16_t y = (uint16_t)(TEXT_Y + (uint16_t)g_text_line * LINE_ADVANCE);
                if (x + DRAW_W <= LCD_WIDTH && y + DRAW_H <= LCD_HEIGHT) {
                    draw_glyph(x, y, c);
                }
            }
            ++g_text_char;
            break;
        }

        case STATUS_IDLE:
        default:
            break;
    }
}
