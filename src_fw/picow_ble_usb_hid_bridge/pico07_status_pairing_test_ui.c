// PICO-07 provisional pairing validation UI.
//
// This candidate deliberately bypasses the Saved Devices pairing navigation.
// Pairing is entered directly from STATUS so physical validation can isolate
// BLE pairing from the final product navigation:
//
//   K1 -> Mouse pairing
//   K2 -> Keyboard pairing
//   K3 -> Composite Mouse+Keyboard pairing
//
// STATUS itself never scans. Entering one of the three pairing flows starts
// discovery automatically. Report Map classification remains authoritative.

#define pico06_ui_init pico06_ui_init_legacy
#define pico06_ui_task pico06_ui_task_legacy
#include "pico06_ui_v2.c"
#undef pico06_ui_init
#undef pico06_ui_task

typedef enum {
    TEST_PAIR_IDLE = 0,
    TEST_PAIR_SCAN,
    TEST_PAIR_CONNECT,
    TEST_PAIR_DELETE_WRONG_TYPE,
    TEST_PAIR_ERROR,
} test_pair_phase_t;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
} test_peer_t;

#define TEST_MAX_TRIED PICO07_MAX_SCAN_RESULTS

static bool g_test_pair_active;
static pico07_device_type_t g_test_target;
static test_pair_phase_t g_test_phase;
static test_peer_t g_test_tried[TEST_MAX_TRIED];
static uint8_t g_test_tried_count;
static test_peer_t g_test_current;
static bool g_test_have_current;
static uint32_t g_test_action_revision;
static bool g_test_dirty;
static ui_screen_t g_test_last_screen;
static char g_test_message[UI_TEXT_MAX + 1u];

static const char *test_target_name(void)
{
    switch (g_test_target) {
        case PICO07_TYPE_MOUSE: return "MOUSE";
        case PICO07_TYPE_KEYBOARD: return "KEYBOARD";
        case PICO07_TYPE_COMPOSITE: return "COMPOSITE";
        default: return "HID";
    }
}

static bool test_peer_equal(const test_peer_t *peer,
                            uint8_t addr_type,
                            const uint8_t addr[6])
{
    return peer != NULL && peer->addr_type == addr_type &&
           memcmp(peer->addr, addr, 6) == 0;
}

static bool test_was_tried(uint8_t addr_type, const uint8_t addr[6])
{
    for (uint8_t i = 0; i < g_test_tried_count; ++i) {
        if (test_peer_equal(&g_test_tried[i], addr_type, addr)) return true;
    }
    return false;
}

static void test_mark_tried(const pico07_scan_result_t *result)
{
    if (result == NULL || test_was_tried(result->addr_type, result->addr)) return;
    if (g_test_tried_count >= TEST_MAX_TRIED) return;
    g_test_tried[g_test_tried_count].addr_type = result->addr_type;
    memcpy(g_test_tried[g_test_tried_count].addr, result->addr, 6);
    ++g_test_tried_count;
}

static void test_refresh_pair_snapshot(void)
{
    pico07_pairing_snapshot_t pair;
    if (pico07_pairing_get_snapshot(&pair) && pair.revision != g_pair_revision) {
        g_pair_revision = pair.revision;
        g_pair = pair;
        g_test_dirty = true;
    }
}

static void test_render_begin(void)
{
    memset(g_lines, 0, sizeof(g_lines));
    for (uint8_t i = 0; i < UI_LINE_COUNT; ++i) g_line_colors[i] = COLOR_WHITE;

    if (g_screen == SCREEN_STATUS) {
        set_title("STATUS");
        set_line(1, COLOR_WHITE, "K1: MOUSE OPTIONS");
        set_line(2, COLOR_WHITE, "K2: KEYBOARD OPTIONS");
        set_line(3, COLOR_WHITE, "K3: COMPOSITE OPTIONS");
        set_line(5, COLOR_CYAN, "PAIRING TEST LAYOUT");
        set_line(7, COLOR_GRAY, "NO AUTO SEARCH HERE");
        set_line(8, COLOR_GRAY, "JOY PRESS = HOME");
    } else {
        set_title("PAIR %s", test_target_name());
        if (g_test_phase == TEST_PAIR_ERROR) {
            set_line(1, COLOR_WHITE, "%s",
                     g_test_message[0] ? g_test_message : "PAIRING FAILED");
            set_line(2, COLOR_WHITE, "JOY PRESS RETRY");
        } else if (g_test_phase == TEST_PAIR_DELETE_WRONG_TYPE) {
            set_line(1, COLOR_WHITE, "WRONG HID TYPE");
            set_line(2, COLOR_WHITE, "REMOVING TEMP BOND");
            set_line(3, COLOR_CYAN, "TARGET %s", test_target_name());
        } else {
            const char *message = g_pair.message[0] ? g_pair.message : "SEARCHING BLE HID";
            set_line(1, COLOR_WHITE, "%s", message);
            set_line(2, COLOR_CYAN, "TARGET %s", test_target_name());
            if (g_pair.state == PICO07_PAIR_SCANNING ||
                g_pair.state == PICO07_PAIR_RESULTS) {
                set_line(3, COLOR_WHITE, "AUTO SEARCH ACTIVE");
                set_line(4, COLOR_WHITE, "FOUND %u HID", g_pair.result_count);
            } else if (g_pair.state == PICO07_PAIR_CONNECTING) {
                set_line(3, COLOR_WHITE, "TRYING ONE DEVICE");
            } else if (g_pair.state == PICO07_PAIR_CLASSIFYING) {
                set_line(3, COLOR_WHITE, "READING REPORT MAP");
                set_line(4, COLOR_WHITE, "CHECKING HID TYPE");
            }
            if (g_test_have_current) {
                set_line(5, COLOR_GRAY, "PEER %02X%02X",
                         g_test_current.addr[4], g_test_current.addr[5]);
            }
        }
        set_line(7, COLOR_GRAY, "K1 CANCEL TO STATUS");
        set_line(8, COLOR_GRAY, "JOY RETRY ON ERROR");
    }

    g_clear_row = 0;
    g_text_line = 0;
    g_text_char = 0;
    g_render_phase = RENDER_CLEAR;
    g_dirty = false;
    g_test_dirty = false;
}

static void test_render_task(void)
{
    // Prevent the legacy renderer from rebuilding STATUS/PAIRING while these
    // provisional screens are owned here.
    g_dirty = false;
    if (g_test_dirty) test_render_begin();
    render_task();
}

static void test_start_scan(bool reset_tried)
{
    if (reset_tried) {
        memset(g_test_tried, 0, sizeof(g_test_tried));
        g_test_tried_count = 0u;
    }
    g_test_have_current = false;
    g_test_phase = TEST_PAIR_SCAN;
    g_test_message[0] = '\0';
    g_test_action_revision = g_pair.revision;
    if (!pico07_pairing_request_scan()) {
        g_test_phase = TEST_PAIR_ERROR;
        snprintf(g_test_message, sizeof(g_test_message), "PAIR COMMAND BUSY");
    }
    g_test_dirty = true;
}

static void test_begin_pairing(pico07_device_type_t target)
{
    g_test_pair_active = true;
    g_test_target = target;
    set_screen(SCREEN_PAIRING);
    test_start_scan(true);
}

static int test_find_untried_result(void)
{
    for (uint8_t i = 0; i < g_pair.result_count; ++i) {
        const pico07_scan_result_t *result = &g_pair.results[i];
        if (!test_was_tried(result->addr_type, result->addr)) return (int)i;
    }
    return -1;
}

static void test_request_next_candidate(void)
{
    const int index = test_find_untried_result();
    if (index < 0) return;

    const pico07_scan_result_t *result = &g_pair.results[index];
    test_mark_tried(result);
    g_test_current.addr_type = result->addr_type;
    memcpy(g_test_current.addr, result->addr, 6);
    g_test_have_current = true;

    g_test_action_revision = g_pair.revision;
    if (pico07_pairing_request_connect_result((uint8_t)index)) {
        g_test_phase = TEST_PAIR_CONNECT;
    } else {
        g_test_phase = TEST_PAIR_ERROR;
        snprintf(g_test_message, sizeof(g_test_message), "CONNECT COMMAND BUSY");
    }
    g_test_dirty = true;
}

static void test_process_pair_state(void)
{
    if (!g_test_pair_active || g_screen != SCREEN_PAIRING) return;

    if (g_test_phase == TEST_PAIR_SCAN) {
        if (g_pair.state == PICO07_PAIR_SCANNING ||
            g_pair.state == PICO07_PAIR_RESULTS) {
            if (test_find_untried_result() >= 0) {
                test_request_next_candidate();
                return;
            }
            if (g_pair.state == PICO07_PAIR_RESULTS &&
                g_pair.revision != g_test_action_revision) {
                g_test_phase = TEST_PAIR_ERROR;
                snprintf(g_test_message, sizeof(g_test_message),
                         "NO %s FOUND", test_target_name());
                g_test_dirty = true;
            }
            return;
        }
        if (g_pair.state == PICO07_PAIR_ERROR &&
            g_pair.revision != g_test_action_revision) {
            g_test_phase = TEST_PAIR_ERROR;
            snprintf(g_test_message, sizeof(g_test_message), "%s",
                     g_pair.message[0] ? g_pair.message : "PAIRING FAILED");
            g_test_dirty = true;
        }
        return;
    }

    if (g_test_phase == TEST_PAIR_CONNECT) {
        if (g_pair.revision == g_test_action_revision) return;
        if (g_pair.state == PICO07_PAIR_SUCCESS) {
            if (g_pair.last_type == g_test_target) {
                g_test_pair_active = false;
                set_screen(SCREEN_DEVICE_SUCCESS);
                return;
            }

            if (!g_test_have_current ||
                !pico07_pairing_request_delete_saved(g_test_current.addr_type,
                                                     g_test_current.addr)) {
                g_test_phase = TEST_PAIR_ERROR;
                snprintf(g_test_message, sizeof(g_test_message), "TEMP DELETE FAILED");
                g_test_dirty = true;
                return;
            }
            g_test_phase = TEST_PAIR_DELETE_WRONG_TYPE;
            g_test_action_revision = g_pair.revision;
            g_test_dirty = true;
            return;
        }

        if (g_pair.state == PICO07_PAIR_ERROR) {
            g_test_phase = TEST_PAIR_ERROR;
            snprintf(g_test_message, sizeof(g_test_message), "%s",
                     g_pair.message[0] ? g_pair.message : "PAIRING FAILED");
            g_test_dirty = true;
        }
        return;
    }

    if (g_test_phase == TEST_PAIR_DELETE_WRONG_TYPE) {
        if (g_pair.revision == g_test_action_revision) return;
        if (g_pair.state == PICO07_PAIR_SUCCESS) {
            test_start_scan(false);
            return;
        }
        if (g_pair.state == PICO07_PAIR_ERROR) {
            g_test_phase = TEST_PAIR_ERROR;
            snprintf(g_test_message, sizeof(g_test_message), "TEMP DELETE FAILED");
            g_test_dirty = true;
        }
    }
}

static void test_handle_status_pressed(pico_hat_input_t input)
{
    if (input == PICO_HAT_INPUT_KEY1) {
        test_begin_pairing(PICO07_TYPE_MOUSE);
    } else if (input == PICO_HAT_INPUT_KEY2) {
        test_begin_pairing(PICO07_TYPE_KEYBOARD);
    } else if (input == PICO_HAT_INPUT_KEY3) {
        test_begin_pairing(PICO07_TYPE_COMPOSITE);
    } else if (input == PICO_HAT_INPUT_JOY_PRESS) {
        set_screen(SCREEN_HOME);
    }
}

static void test_handle_pairing_pressed(pico_hat_input_t input)
{
    if (input == PICO_HAT_INPUT_KEY1) {
        (void)pico07_pairing_request_cancel();
        g_test_pair_active = false;
        g_test_dirty = true;
        set_screen(SCREEN_STATUS);
    } else if (input == PICO_HAT_INPUT_JOY_PRESS && g_test_phase == TEST_PAIR_ERROR) {
        test_start_scan(true);
    }
}

void pico06_ui_init(void)
{
    pico06_ui_init_legacy();
    g_test_pair_active = false;
    g_test_target = PICO07_TYPE_UNKNOWN;
    g_test_phase = TEST_PAIR_IDLE;
    g_test_tried_count = 0u;
    g_test_have_current = false;
    g_test_action_revision = 0u;
    g_test_dirty = true;
    g_test_last_screen = SCREEN_HOME;
    memset(g_test_message, 0, sizeof(g_test_message));
}

void pico06_ui_task(void)
{
    const bool own_status = g_screen == SCREEN_STATUS;
    const bool own_pairing = g_screen == SCREEN_PAIRING && g_test_pair_active;

    if (g_screen != g_test_last_screen) {
        g_test_last_screen = g_screen;
        g_test_dirty = true;
    }

    if (!own_status && !own_pairing) {
        pico06_ui_task_legacy();
        return;
    }

    if (!pico_hat_ui_is_lcd_ready()) return;
    const bool locked = pico_hat_ui_is_screen_locked();
    if (locked) {
        g_was_locked = true;
        return;
    }
    if (g_was_locked) {
        g_was_locked = false;
        drain_source_events();
        g_test_pair_active = false;
        set_screen(SCREEN_HOME);
        return;
    }

    if (own_pairing) {
        test_refresh_pair_snapshot();
        test_process_pair_state();
    }

    // Pair processing may transition to the legacy success screen.
    if (g_screen != SCREEN_STATUS &&
        !(g_screen == SCREEN_PAIRING && g_test_pair_active)) {
        return;
    }

    pico_hat_event_t event;
    if (pico_hat_ui_poll_event(&event) && event.pressed) {
        if (event.input == PICO_HAT_INPUT_KEY4) {
            // Screen lock remains owned by pico_hat_ui.
        } else if (g_screen == SCREEN_STATUS) {
            test_handle_status_pressed(event.input);
        } else {
            test_handle_pairing_pressed(event.input);
        }
    }

    if (g_screen == SCREEN_STATUS ||
        (g_screen == SCREEN_PAIRING && g_test_pair_active)) {
        test_render_task();
    }
}
