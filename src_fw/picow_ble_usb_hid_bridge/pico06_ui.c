#include "pico06_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "logitech_hidpp.h"
#include "pico_hat_ui.h"
#include "remap_engine.h"
#include "remap_profile.h"

#define LCD_SPI spi1
#define LCD_PIN_DC 8u
#define LCD_PIN_CS 9u
#define LCD_WIDTH 240u
#define LCD_HEIGHT 240u

#define UI_LINE_COUNT 8u
#define UI_TEXT_MAX 20u
#define GLYPH_SCALE 2u
#define GLYPH_W 5u
#define GLYPH_H 7u
#define DRAW_W (GLYPH_W * GLYPH_SCALE)
#define DRAW_H (GLYPH_H * GLYPH_SCALE)
#define CHAR_ADVANCE 11u
#define LINE_ADVANCE 27u
#define TEXT_X 7u
#define TEXT_Y 8u

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_MODE_MENU,
    SCREEN_CUSTOM,
    SCREEN_CAPTURE,
    SCREEN_DRAG,
    SCREEN_CUSTOM_ACTIONS,
    SCREEN_CLEAR_CONFIRM,
    SCREEN_HELP,
    SCREEN_WAIT_APPLY,
    SCREEN_MESSAGE,
} ui_screen_t;

typedef enum {
    RENDER_WAIT_LCD = 0,
    RENDER_CLEAR,
    RENDER_TEXT,
    RENDER_IDLE,
} render_phase_t;

static ui_screen_t g_screen;
static ui_screen_t g_help_return;
static render_phase_t g_render_phase;
static bool g_dirty;
static char g_lines[UI_LINE_COUNT][UI_TEXT_MAX + 1u];
static uint16_t g_clear_row;
static uint8_t g_text_line;
static uint8_t g_text_char;

static remap_profile_snapshot_t g_profile;
static bool g_have_profile;
static uint32_t g_profile_revision;
static uint32_t g_hidpp_revision;
static remap_profile_config_t g_pending;

static uint8_t g_mode_selection;
static remap_target_t g_capture_target;
static remap_source_t g_drag_source;
static uint8_t g_drag_selection;
static uint8_t g_action_selection;
static uint32_t g_wait_apply_id;
static char g_message[UI_TEXT_MAX + 1u];

static const uint8_t k_alpha[26][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f},{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f},{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f},{0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},{0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a},{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
};

static const uint8_t k_digit[10][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e},{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
};

static uint8_t glyph_row(char c, uint8_t row)
{
    if (row >= GLYPH_H) return 0;
    if (c >= 'A' && c <= 'Z') return k_alpha[(uint8_t)(c - 'A')][row];
    if (c >= '0' && c <= '9') return k_digit[(uint8_t)(c - '0')][row];
    if (c == '-') return row == 3u ? 0x1fu : 0u;
    if (c == ':') return (row == 2u || row == 5u) ? 0x04u : 0u;
    if (c == '.') return row == 6u ? 0x04u : 0u;
    if (c == '>') {
        static const uint8_t rows[7] = {0x10,0x08,0x04,0x02,0x04,0x08,0x10};
        return rows[row];
    }
    return 0;
}

static void lcd_select(bool selected) { gpio_put(LCD_PIN_CS, !selected); }

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
    coords[0]=(uint8_t)(x0>>8); coords[1]=(uint8_t)x0;
    coords[2]=(uint8_t)((x1-1u)>>8); coords[3]=(uint8_t)(x1-1u);
    lcd_data(coords, sizeof(coords));
    lcd_command(0x2b);
    coords[0]=(uint8_t)(y0>>8); coords[1]=(uint8_t)y0;
    coords[2]=(uint8_t)((y1-1u)>>8); coords[3]=(uint8_t)(y1-1u);
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
    for (uint8_t py=0; py<DRAW_H; ++py) {
        const uint8_t bits = glyph_row(c, (uint8_t)(py / GLYPH_SCALE));
        for (uint8_t px=0; px<DRAW_W; ++px) {
            const uint8_t sx = (uint8_t)(px / GLYPH_SCALE);
            const bool on = (bits & (uint8_t)(1u << (4u - sx))) != 0;
            const uint16_t color = on ? 0xffffu : 0x0000u;
            pixels[out++] = (uint8_t)(color >> 8);
            pixels[out++] = (uint8_t)color;
        }
    }
    lcd_window(x, y, (uint16_t)(x + DRAW_W), (uint16_t)(y + DRAW_H));
    lcd_data(pixels, sizeof(pixels));
}

static void set_screen(ui_screen_t screen)
{
    g_screen = screen;
    g_dirty = true;
}

static const char *source_short(remap_source_t source)
{
    switch (source) {
        case REMAP_SOURCE_LEFT: return "LEFT";
        case REMAP_SOURCE_RIGHT: return "RIGHT";
        case REMAP_SOURCE_MIDDLE: return "MIDDLE";
        case REMAP_SOURCE_BACK: return "BACK";
        case REMAP_SOURCE_FORWARD: return "FORWARD";
        default: return "NONE";
    }
}

static remap_source_t source_for_target(remap_target_t target, bool *found)
{
    for (uint8_t i=0; i<REMAP_SOURCE_COUNT; ++i) {
        if (g_pending.mappings[i] == (uint8_t)target) {
            *found = true;
            return (remap_source_t)i;
        }
    }
    *found = false;
    return REMAP_SOURCE_LEFT;
}

static const char *policy_short(device_drag_fix_policy_t policy)
{
    switch (policy) {
        case DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED: return "FORCE";
        case DEVICE_DRAG_FIX_OFF: return "OFF";
        case DEVICE_DRAG_FIX_AUTO:
        default: return "AUTO";
    }
}

static void build_drag_line(char *line, size_t size, bool forward)
{
    const remap_source_t source = forward ? REMAP_SOURCE_FORWARD : REMAP_SOURCE_BACK;
    const char *name = forward ? "FWD" : "BACK";
    const device_drag_fix_policy_t policy = forward
        ? g_profile.active.drag_fix_forward : g_profile.active.drag_fix_back;

    if (g_profile.active.mode == DEVICE_PROFILE_CUSTOM_REMAP &&
        g_profile.active.mappings[source] == REMAP_TARGET_PASSTHROUGH) {
        snprintf(line, size, "%s NO MAPPING", name);
        return;
    }

    device_profile_snapshot_t device;
    logitech_hidpp_snapshot_t hidpp;
    const bool have_device = device_profile_get_snapshot(&device);
    const bool have_hidpp = logitech_hidpp_get_snapshot(&hidpp);
    const uint8_t bit = forward ? LOGITECH_HIDPP_SOURCE_FORWARD
                                : LOGITECH_HIDPP_SOURCE_BACK;

    if (g_profile.active.mode == DEVICE_PROFILE_DEFAULT_REMAP) {
        const device_drag_backend_t backend = have_device
            ? (forward ? device.auto_forward_backend : device.auto_back_backend)
            : DEVICE_DRAG_BACKEND_STANDARD;
        if (backend == DEVICE_DRAG_BACKEND_STANDARD) {
            snprintf(line, size, "%s AUTO STD", name);
            return;
        }
    } else if (policy == DEVICE_DRAG_FIX_OFF) {
        snprintf(line, size, "%s DRAG OFF", name);
        return;
    } else if (policy == DEVICE_DRAG_FIX_AUTO && have_device) {
        const device_drag_backend_t backend = forward
            ? device.auto_forward_backend : device.auto_back_backend;
        if (backend == DEVICE_DRAG_BACKEND_STANDARD) {
            snprintf(line, size, "%s AUTO STD", name);
            return;
        }
    }

    if (have_hidpp && (hidpp.failed_mask & bit) != 0) {
        snprintf(line, size, "%s %s UNSUP", name, policy_short(policy));
    } else if (have_hidpp && (hidpp.applied_mask & bit) != 0) {
        snprintf(line, size, "%s %s HIDPP", name, policy_short(policy));
    } else if (have_hidpp && (hidpp.desired_mask & bit) != 0) {
        snprintf(line, size, "%s %s SETUP", name, policy_short(policy));
    } else {
        snprintf(line, size, "%s %s STD", name, policy_short(policy));
    }
}

static void build_custom_binding(uint8_t line_index,
                                 const char *target_name,
                                 remap_target_t target)
{
    bool found = false;
    const remap_source_t source = source_for_target(target, &found);
    snprintf(g_lines[line_index], sizeof(g_lines[line_index]), "%s: %s",
             target_name, found ? source_short(source) : "PASS");
}

static void build_lines(void)
{
    memset(g_lines, 0, sizeof(g_lines));

    switch (g_screen) {
        case SCREEN_HOME:
            snprintf(g_lines[0], sizeof(g_lines[0]), "PICO-06");
            if (!g_have_profile || !g_profile.connected) {
                snprintf(g_lines[1], sizeof(g_lines[1]), "WAITING FOR MOUSE");
                snprintf(g_lines[2], sizeof(g_lines[2]), "PROFILE PASSTHRU");
            } else {
                snprintf(g_lines[1], sizeof(g_lines[1]), "PROFILE %s",
                         remap_profile_mode_name(g_profile.active.mode));
                build_drag_line(g_lines[2], sizeof(g_lines[2]), true);
                build_drag_line(g_lines[3], sizeof(g_lines[3]), false);
            }
            snprintf(g_lines[4], sizeof(g_lines[4]), "K2 REMAP MENU");
            snprintf(g_lines[5], sizeof(g_lines[5]), "K3 HELP");
            snprintf(g_lines[6], sizeof(g_lines[6]), "K4 SCREEN LOCK");
            snprintf(g_lines[7], sizeof(g_lines[7]), "STORAGE %s",
                     (!g_have_profile || g_profile.storage_ok) ? "OK" : "ERROR");
            break;

        case SCREEN_MODE_MENU:
            snprintf(g_lines[0], sizeof(g_lines[0]), "REMAP MODE");
            snprintf(g_lines[1], sizeof(g_lines[1]), "%c PASSTHROUGH",
                     g_mode_selection == 0 ? '>' : ' ');
            snprintf(g_lines[2], sizeof(g_lines[2]), "%c DEFAULT REMAP",
                     g_mode_selection == 1 ? '>' : ' ');
            snprintf(g_lines[3], sizeof(g_lines[3]), "%c CUSTOM REMAP",
                     g_mode_selection == 2 ? '>' : ' ');
            snprintf(g_lines[5], sizeof(g_lines[5]), "JOY PRESS SELECT");
            snprintf(g_lines[6], sizeof(g_lines[6]), "K1 CANCEL");
            snprintf(g_lines[7], sizeof(g_lines[7]), "K3 HELP");
            break;

        case SCREEN_CUSTOM:
            snprintf(g_lines[0], sizeof(g_lines[0]), "CUSTOM PENDING");
            build_custom_binding(1, "LEFT", REMAP_TARGET_LEFT);
            build_custom_binding(2, "RIGHT", REMAP_TARGET_RIGHT);
            build_custom_binding(3, "FWD", REMAP_TARGET_FORWARD);
            build_custom_binding(4, "BACK", REMAP_TARGET_BACK);
            build_custom_binding(5, "MIDDLE", REMAP_TARGET_MIDDLE);
            snprintf(g_lines[6], sizeof(g_lines[6]), "K2 ACTIONS");
            snprintf(g_lines[7], sizeof(g_lines[7]), "K1 CANCEL K3 HELP");
            break;

        case SCREEN_CAPTURE:
            snprintf(g_lines[0], sizeof(g_lines[0]), "SET TARGET %s",
                     remap_target_name(g_capture_target));
            snprintf(g_lines[2], sizeof(g_lines[2]), "PRESS MOUSE BUTTON");
            snprintf(g_lines[4], sizeof(g_lines[4]), "MOVE WHEEL IGNORED");
            snprintf(g_lines[6], sizeof(g_lines[6]), "K1 CANCEL");
            break;

        case SCREEN_DRAG:
            snprintf(g_lines[0], sizeof(g_lines[0]), "DRAG FIX %s",
                     g_drag_source == REMAP_SOURCE_FORWARD ? "FORWARD" : "BACK");
            snprintf(g_lines[1], sizeof(g_lines[1]), "%c AUTO",
                     g_drag_selection == 0 ? '>' : ' ');
            snprintf(g_lines[2], sizeof(g_lines[2]), "%c FORCE IF SUPPORTED",
                     g_drag_selection == 1 ? '>' : ' ');
            snprintf(g_lines[3], sizeof(g_lines[3]), "%c OFF",
                     g_drag_selection == 2 ? '>' : ' ');
            snprintf(g_lines[5], sizeof(g_lines[5]), "JOY PRESS SELECT");
            snprintf(g_lines[6], sizeof(g_lines[6]), "K1 CANCEL");
            break;

        case SCREEN_CUSTOM_ACTIONS:
            snprintf(g_lines[0], sizeof(g_lines[0]), "CUSTOM ACTIONS");
            snprintf(g_lines[1], sizeof(g_lines[1]), "%c APPLY",
                     g_action_selection == 0 ? '>' : ' ');
            snprintf(g_lines[2], sizeof(g_lines[2]), "%c CLEAR MAPPINGS",
                     g_action_selection == 1 ? '>' : ' ');
            snprintf(g_lines[3], sizeof(g_lines[3]), "%c CANCEL",
                     g_action_selection == 2 ? '>' : ' ');
            snprintf(g_lines[6], sizeof(g_lines[6]), "K1 BACK");
            break;

        case SCREEN_CLEAR_CONFIRM:
            snprintf(g_lines[0], sizeof(g_lines[0]), "CLEAR MAPPINGS");
            snprintf(g_lines[2], sizeof(g_lines[2]), "REMOVE SAVED CUSTOM");
            snprintf(g_lines[3], sizeof(g_lines[3]), "ALL BUTTONS PASS");
            snprintf(g_lines[5], sizeof(g_lines[5]), "JOY PRESS CONFIRM");
            snprintf(g_lines[6], sizeof(g_lines[6]), "K1 CANCEL");
            break;

        case SCREEN_HELP:
            if (g_help_return == SCREEN_CUSTOM || g_help_return == SCREEN_CAPTURE ||
                g_help_return == SCREEN_DRAG || g_help_return == SCREEN_CUSTOM_ACTIONS) {
                snprintf(g_lines[0], sizeof(g_lines[0]), "CUSTOM HELP");
                snprintf(g_lines[1], sizeof(g_lines[1]), "JOY LEFT TARGET LEFT");
                snprintf(g_lines[2], sizeof(g_lines[2]), "JOY RIGHT TARGET RGT");
                snprintf(g_lines[3], sizeof(g_lines[3]), "JOY UP TARGET FWD");
                snprintf(g_lines[4], sizeof(g_lines[4]), "JOY DOWN TARGET BACK");
                snprintf(g_lines[5], sizeof(g_lines[5]), "JOY PRESS TARGET MID");
                snprintf(g_lines[6], sizeof(g_lines[6]), "K2 ACTIONS");
                snprintf(g_lines[7], sizeof(g_lines[7]), "K1 BACK");
            } else {
                snprintf(g_lines[0], sizeof(g_lines[0]), "HELP");
                snprintf(g_lines[1], sizeof(g_lines[1]), "K2 REMAP MENU");
                snprintf(g_lines[2], sizeof(g_lines[2]), "K1 BACK CANCEL");
                snprintf(g_lines[3], sizeof(g_lines[3]), "K3 HELP");
                snprintf(g_lines[4], sizeof(g_lines[4]), "K4 LOCK SCREEN");
                snprintf(g_lines[5], sizeof(g_lines[5]), "DEFAULT LEFT ESC");
                snprintf(g_lines[6], sizeof(g_lines[6]), "CUSTOM MOUSE ONLY");
                snprintf(g_lines[7], sizeof(g_lines[7]), "K1 BACK");
            }
            break;

        case SCREEN_WAIT_APPLY:
            snprintf(g_lines[0], sizeof(g_lines[0]), "APPLYING PROFILE");
            snprintf(g_lines[2], sizeof(g_lines[2]), "NEUTRALIZING USB");
            snprintf(g_lines[4], sizeof(g_lines[4]), "SAVING TO DEVICE");
            break;

        case SCREEN_MESSAGE:
            snprintf(g_lines[0], sizeof(g_lines[0]), "PICO-06");
            snprintf(g_lines[2], sizeof(g_lines[2]), "%s", g_message);
            snprintf(g_lines[5], sizeof(g_lines[5]), "JOY PRESS OR K1");
            snprintf(g_lines[6], sizeof(g_lines[6]), "BACK TO STATUS");
            break;
    }
}

static void begin_render(void)
{
    build_lines();
    g_clear_row = 0;
    g_text_line = 0;
    g_text_char = 0;
    g_render_phase = RENDER_CLEAR;
    g_dirty = false;
}

static void render_task(void)
{
    if (g_dirty) begin_render();

    switch (g_render_phase) {
        case RENDER_WAIT_LCD:
            begin_render();
            break;
        case RENDER_CLEAR:
            clear_one_row(g_clear_row++);
            if (g_clear_row >= LCD_HEIGHT) g_render_phase = RENDER_TEXT;
            break;
        case RENDER_TEXT: {
            while (g_text_line < UI_LINE_COUNT &&
                   g_lines[g_text_line][g_text_char] == '\0') {
                ++g_text_line;
                g_text_char = 0;
            }
            if (g_text_line >= UI_LINE_COUNT) {
                g_render_phase = RENDER_IDLE;
                break;
            }
            const char c = g_lines[g_text_line][g_text_char];
            if (c != ' ') {
                const uint16_t x = (uint16_t)(TEXT_X + (uint16_t)g_text_char * CHAR_ADVANCE);
                const uint16_t y = (uint16_t)(TEXT_Y + (uint16_t)g_text_line * LINE_ADVANCE);
                if (x + DRAW_W <= LCD_WIDTH && y + DRAW_H <= LCD_HEIGHT) draw_glyph(x, y, c);
            }
            ++g_text_char;
            break;
        }
        case RENDER_IDLE:
        default:
            break;
    }
}

static void drain_source_events(void)
{
    remap_source_t source;
    while (remap_engine_poll_source_down(&source)) { (void)source; }
}

static void start_capture(remap_target_t target)
{
    drain_source_events();
    g_capture_target = target;
    set_screen(SCREEN_CAPTURE);
}

static device_drag_fix_policy_t selection_to_policy(uint8_t selection)
{
    if (selection == 1) return DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED;
    if (selection == 2) return DEVICE_DRAG_FIX_OFF;
    return DEVICE_DRAG_FIX_AUTO;
}

static uint8_t policy_to_selection(device_drag_fix_policy_t policy)
{
    if (policy == DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED) return 1;
    if (policy == DEVICE_DRAG_FIX_OFF) return 2;
    return 0;
}

static void request_apply(const remap_profile_config_t *config)
{
    uint32_t id = 0;
    if (!remap_profile_request_apply(config, &id)) {
        snprintf(g_message, sizeof(g_message), "APPLY BUSY OR INVALID");
        set_screen(SCREEN_MESSAGE);
        return;
    }
    g_wait_apply_id = id;
    set_screen(SCREEN_WAIT_APPLY);
}

static void select_mode(void)
{
    remap_profile_config_t config;
    if (g_have_profile) config = g_profile.active;
    else remap_profile_make_passthrough(&config);

    if (g_mode_selection == 0) {
        config.mode = DEVICE_PROFILE_PASSTHROUGH;
        request_apply(&config);
    } else if (g_mode_selection == 1) {
        config.mode = DEVICE_PROFILE_DEFAULT_REMAP;
        config.drag_fix_back = DEVICE_DRAG_FIX_AUTO;
        config.drag_fix_forward = DEVICE_DRAG_FIX_AUTO;
        request_apply(&config);
    } else {
        g_pending = config;
        g_pending.mode = DEVICE_PROFILE_CUSTOM_REMAP;
        set_screen(SCREEN_CUSTOM);
    }
}

static void capture_source(remap_source_t source)
{
    // A target is unique. Re-selecting it replaces the old source immediately
    // in the pending config, while the old source returns to passthrough.
    for (uint8_t i=0; i<REMAP_SOURCE_COUNT; ++i) {
        if (g_pending.mappings[i] == (uint8_t)g_capture_target) {
            g_pending.mappings[i] = REMAP_TARGET_PASSTHROUGH;
        }
    }
    g_pending.mappings[source] = (uint8_t)g_capture_target;

    if (source == REMAP_SOURCE_BACK || source == REMAP_SOURCE_FORWARD) {
        g_drag_source = source;
        const device_drag_fix_policy_t policy = source == REMAP_SOURCE_BACK
            ? g_pending.drag_fix_back : g_pending.drag_fix_forward;
        g_drag_selection = policy_to_selection(policy);
        set_screen(SCREEN_DRAG);
    } else {
        set_screen(SCREEN_CUSTOM);
    }
}

static void handle_pressed(pico_hat_input_t input)
{
    if (input == PICO_HAT_INPUT_KEY4) return; // lock is owned by pico_hat_ui

    if (input == PICO_HAT_INPUT_KEY3 && g_screen != SCREEN_HELP &&
        g_screen != SCREEN_WAIT_APPLY && g_screen != SCREEN_MESSAGE) {
        g_help_return = g_screen;
        set_screen(SCREEN_HELP);
        return;
    }

    switch (g_screen) {
        case SCREEN_HOME:
            if (input == PICO_HAT_INPUT_KEY2) {
                g_mode_selection = g_have_profile ? (uint8_t)g_profile.active.mode : 0;
                if (g_mode_selection > 2) g_mode_selection = 0;
                set_screen(SCREEN_MODE_MENU);
            }
            break;

        case SCREEN_MODE_MENU:
            if (input == PICO_HAT_INPUT_JOY_UP) {
                g_mode_selection = g_mode_selection == 0 ? 2 : (uint8_t)(g_mode_selection - 1u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_DOWN) {
                g_mode_selection = (uint8_t)((g_mode_selection + 1u) % 3u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_PRESS) {
                select_mode();
            } else if (input == PICO_HAT_INPUT_KEY1) {
                set_screen(SCREEN_HOME);
            }
            break;

        case SCREEN_CUSTOM:
            if (input == PICO_HAT_INPUT_JOY_LEFT) start_capture(REMAP_TARGET_LEFT);
            else if (input == PICO_HAT_INPUT_JOY_RIGHT) start_capture(REMAP_TARGET_RIGHT);
            else if (input == PICO_HAT_INPUT_JOY_UP) start_capture(REMAP_TARGET_FORWARD);
            else if (input == PICO_HAT_INPUT_JOY_DOWN) start_capture(REMAP_TARGET_BACK);
            else if (input == PICO_HAT_INPUT_JOY_PRESS) start_capture(REMAP_TARGET_MIDDLE);
            else if (input == PICO_HAT_INPUT_KEY2) {
                g_action_selection = 0;
                set_screen(SCREEN_CUSTOM_ACTIONS);
            } else if (input == PICO_HAT_INPUT_KEY1) {
                set_screen(SCREEN_HOME);
            }
            break;

        case SCREEN_CAPTURE:
            if (input == PICO_HAT_INPUT_KEY1) set_screen(SCREEN_CUSTOM);
            break;

        case SCREEN_DRAG:
            if (input == PICO_HAT_INPUT_JOY_UP) {
                g_drag_selection = g_drag_selection == 0 ? 2 : (uint8_t)(g_drag_selection - 1u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_DOWN) {
                g_drag_selection = (uint8_t)((g_drag_selection + 1u) % 3u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_PRESS) {
                const device_drag_fix_policy_t policy = selection_to_policy(g_drag_selection);
                if (g_drag_source == REMAP_SOURCE_BACK) g_pending.drag_fix_back = policy;
                else g_pending.drag_fix_forward = policy;
                set_screen(SCREEN_CUSTOM);
            } else if (input == PICO_HAT_INPUT_KEY1) {
                set_screen(SCREEN_CUSTOM);
            }
            break;

        case SCREEN_CUSTOM_ACTIONS:
            if (input == PICO_HAT_INPUT_JOY_UP) {
                g_action_selection = g_action_selection == 0 ? 2 : (uint8_t)(g_action_selection - 1u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_DOWN) {
                g_action_selection = (uint8_t)((g_action_selection + 1u) % 3u);
                g_dirty = true;
            } else if (input == PICO_HAT_INPUT_JOY_PRESS) {
                if (g_action_selection == 0) {
                    g_pending.mode = DEVICE_PROFILE_CUSTOM_REMAP;
                    request_apply(&g_pending);
                } else if (g_action_selection == 1) {
                    set_screen(SCREEN_CLEAR_CONFIRM);
                } else {
                    set_screen(SCREEN_HOME);
                }
            } else if (input == PICO_HAT_INPUT_KEY1) {
                set_screen(SCREEN_CUSTOM);
            }
            break;

        case SCREEN_CLEAR_CONFIRM:
            if (input == PICO_HAT_INPUT_JOY_PRESS) {
                memset(g_pending.mappings, 0, sizeof(g_pending.mappings));
                g_pending.mode = DEVICE_PROFILE_CUSTOM_REMAP;
                g_pending.drag_fix_back = DEVICE_DRAG_FIX_AUTO;
                g_pending.drag_fix_forward = DEVICE_DRAG_FIX_AUTO;
                request_apply(&g_pending);
            } else if (input == PICO_HAT_INPUT_KEY1) {
                set_screen(SCREEN_CUSTOM_ACTIONS);
            }
            break;

        case SCREEN_HELP:
            if (input == PICO_HAT_INPUT_KEY1 || input == PICO_HAT_INPUT_KEY3 ||
                input == PICO_HAT_INPUT_JOY_PRESS) {
                set_screen(g_help_return);
            }
            break;

        case SCREEN_MESSAGE:
            if (input == PICO_HAT_INPUT_KEY1 || input == PICO_HAT_INPUT_JOY_PRESS) {
                set_screen(SCREEN_HOME);
            }
            break;

        case SCREEN_WAIT_APPLY:
        default:
            break;
    }
}

static void refresh_snapshots(void)
{
    remap_profile_snapshot_t profile;
    if (remap_profile_get_snapshot(&profile) && profile.revision != g_profile_revision) {
        g_profile_revision = profile.revision;
        g_profile = profile;
        g_have_profile = true;
        if (g_screen == SCREEN_HOME) g_dirty = true;

        if (g_screen == SCREEN_WAIT_APPLY && g_wait_apply_id != 0 &&
            profile.last_apply_id == g_wait_apply_id) {
            snprintf(g_message, sizeof(g_message), "%s",
                     profile.last_apply_ok ? "PROFILE APPLIED" : "APPLY FAILED");
            g_wait_apply_id = 0;
            set_screen(SCREEN_MESSAGE);
        }
    }

    logitech_hidpp_snapshot_t hidpp;
    if (logitech_hidpp_get_snapshot(&hidpp) && hidpp.revision != g_hidpp_revision) {
        g_hidpp_revision = hidpp.revision;
        if (g_screen == SCREEN_HOME) g_dirty = true;
    }
}

void pico06_ui_init(void)
{
    memset(&g_profile, 0, sizeof(g_profile));
    remap_profile_make_passthrough(&g_pending);
    g_have_profile = false;
    g_profile_revision = 0;
    g_hidpp_revision = 0;
    g_screen = SCREEN_HOME;
    g_help_return = SCREEN_HOME;
    g_render_phase = RENDER_WAIT_LCD;
    g_dirty = true;
    g_wait_apply_id = 0;
    memset(g_message, 0, sizeof(g_message));
}

void pico06_ui_task(void)
{
    if (!pico_hat_ui_is_lcd_ready()) return;
    if (pico_hat_ui_is_screen_locked()) return;

    refresh_snapshots();

    if (g_screen == SCREEN_CAPTURE) {
        remap_source_t source;
        if (remap_engine_poll_source_down(&source)) capture_source(source);
    }

    pico_hat_event_t event;
    if (pico_hat_ui_poll_event(&event) && event.pressed) {
        handle_pressed(event.input);
    }

    render_task();
}
