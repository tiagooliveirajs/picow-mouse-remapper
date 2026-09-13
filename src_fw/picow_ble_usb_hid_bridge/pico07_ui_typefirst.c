// PICO-07 type-first UI overlay.
//
// Reuse the physically validated PICO-06/PICO-07 renderer/remap screens, but
// replace generic Pair New Device + delayed result selection with three
// dedicated pairing flows. Entering PAIR MOUSE / PAIR KEYBOARD /
// PAIR COMPOSITE starts discovery immediately and the first untried BLE HID is
// connected at once. After Report Map classification, a mismatched peer is
// deleted and the same screen resumes scanning for the requested type.

#define pico06_ui_init pico06_ui_init_legacy
#define pico06_ui_task pico06_ui_task_legacy
#include "pico06_ui_v2.c"
#undef pico06_ui_init
#undef pico06_ui_task

typedef enum {
    TF_PHASE_IDLE = 0,
    TF_PHASE_SCAN,
    TF_PHASE_CONNECT,
    TF_PHASE_DELETE,
    TF_PHASE_ERROR,
} typefirst_phase_t;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
} typefirst_peer_t;

#define TF_MAX_TRIED PICO07_MAX_SCAN_RESULTS

static bool g_tf_active;
static pico07_device_type_t g_tf_target;
static typefirst_phase_t g_tf_phase;
static typefirst_peer_t g_tf_tried[TF_MAX_TRIED];
static uint8_t g_tf_tried_count;
static typefirst_peer_t g_tf_current;
static bool g_tf_have_current;
static uint32_t g_tf_action_revision;
static bool g_tf_dirty;
static ui_screen_t g_tf_last_screen;
static char g_tf_local_message[UI_TEXT_MAX + 1u];

static const char *tf_target_name(void)
{
    switch (g_tf_target) {
        case PICO07_TYPE_MOUSE: return "MOUSE";
        case PICO07_TYPE_KEYBOARD: return "KEYBOARD";
        case PICO07_TYPE_COMPOSITE: return "COMPOSITE";
        default: return "HID";
    }
}

static bool tf_peer_equal(const typefirst_peer_t *peer,
                          uint8_t addr_type,
                          const uint8_t addr[6])
{
    return peer != NULL && peer->addr_type == addr_type &&
           memcmp(peer->addr, addr, 6) == 0;
}

static bool tf_was_tried(uint8_t addr_type, const uint8_t addr[6])
{
    for (uint8_t i = 0; i < g_tf_tried_count; ++i) {
        if (tf_peer_equal(&g_tf_tried[i], addr_type, addr)) return true;
    }
    return false;
}

static void tf_mark_tried(const pico07_scan_result_t *result)
{
    if (result == NULL || tf_was_tried(result->addr_type, result->addr)) return;
    if (g_tf_tried_count >= TF_MAX_TRIED) return;
    g_tf_tried[g_tf_tried_count].addr_type = result->addr_type;
    memcpy(g_tf_tried[g_tf_tried_count].addr, result->addr, 6);
    ++g_tf_tried_count;
}

static void tf_refresh_snapshots(void)
{
    device_profile_snapshot_t device;
    if (device_profile_get_snapshot(&device) && device.revision != g_device_revision) {
        g_device_revision = device.revision;
        g_device = device;
        g_have_device = true;
        g_tf_dirty = true;
    }

    device_profile_catalog_t catalog;
    if (device_profile_get_catalog(&catalog) && catalog.revision != g_catalog_revision) {
        g_catalog_revision = catalog.revision;
        g_catalog = catalog;
        const uint8_t max_selection = (uint8_t)(2u + g_catalog.count);
        if (g_device_selection > max_selection) g_device_selection = max_selection;
        g_tf_dirty = true;
    }

    pico07_pairing_snapshot_t pair;
    if (pico07_pairing_get_snapshot(&pair) && pair.revision != g_pair_revision) {
        g_pair_revision = pair.revision;
        g_pair = pair;
        g_tf_dirty = true;
    }
}

static void tf_render_begin(void)
{
    memset(g_lines, 0, sizeof(g_lines));
    for (uint8_t i = 0; i < UI_LINE_COUNT; ++i) g_line_colors[i] = COLOR_WHITE;

    if (g_screen == SCREEN_DEVICES) {
        set_title("SAVED DEVICES");
        set_line(1, option_color(g_device_selection == 0u, false),
                 "%c PAIR MOUSE", g_device_selection == 0u ? '>' : ' ');
        set_line(2, option_color(g_device_selection == 1u, false),
                 "%c PAIR KEYBOARD", g_device_selection == 1u ? '>' : ' ');
        set_line(3, option_color(g_device_selection == 2u, false),
                 "%c PAIR COMPOSITE", g_device_selection == 2u ? '>' : ' ');

        uint8_t first_record = 0u;
        if (g_device_selection >= 6u) first_record = (uint8_t)(g_device_selection - 5u);
        for (uint8_t row = 0; row < 3u; ++row) {
            const uint8_t record_index = (uint8_t)(first_record + row);
            if (record_index >= g_catalog.count) break;
            const uint8_t selection = (uint8_t)(record_index + 3u);
            build_saved_record_line((uint8_t)(4u + row), record_index,
                                    g_device_selection == selection);
        }
        set_line(7, COLOR_GRAY, "JOY PRESS SELECT");
        set_line(8, COLOR_GRAY, "K1 HOME  K3 HELP");
    } else {
        set_title("PAIR %s", tf_target_name());
        if (g_tf_phase == TF_PHASE_ERROR) {
            set_line(1, COLOR_WHITE, "%s",
                     g_tf_local_message[0] ? g_tf_local_message : "NO MATCH FOUND");
            set_line(2, COLOR_WHITE, "JOY PRESS RETRY");
        } else if (g_tf_phase == TF_PHASE_DELETE) {
            set_line(1, COLOR_WHITE, "WRONG HID TYPE");
            set_line(2, COLOR_WHITE, "REMOVING TEMP BOND");
            set_line(3, COLOR_CYAN, "TARGET %s", tf_target_name());
        } else {
            const char *message = g_pair.message[0] ? g_pair.message : "SEARCHING BLE HID";
            set_line(1, COLOR_WHITE, "%s", message);
            set_line(2, COLOR_CYAN, "TARGET %s", tf_target_name());
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
            if (g_tf_have_current) {
                set_line(5, COLOR_GRAY, "PEER %02X%02X",
                         g_tf_current.addr[4], g_tf_current.addr[5]);
            }
        }
        set_line(7, COLOR_GRAY, "K1 CANCEL");
        set_line(8, COLOR_GRAY, "K3 HELP");
    }

    g_clear_row = 0;
    g_text_line = 0;
    g_text_char = 0;
    g_render_phase = RENDER_CLEAR;
    g_dirty = false;
    g_tf_dirty = false;
}

static void tf_render_task(void)
{
    // Suppress the legacy build_lines() while these two PICO-07 screens are
    // owned by the type-first overlay. The pixel/glyph renderer itself remains
    // unchanged and therefore retains the validated 9x21 geometry.
    g_dirty = false;
    if (g_tf_dirty) tf_render_begin();
    render_task();
}

static void tf_start_scan(bool reset_tried)
{
    if (reset_tried) {
        memset(g_tf_tried, 0, sizeof(g_tf_tried));
        g_tf_tried_count = 0u;
    }
    g_tf_have_current = false;
    g_tf_phase = TF_PHASE_SCAN;
    g_tf_local_message[0] = '\0';
    g_tf_action_revision = g_pair.revision;
    if (!pico07_pairing_request_scan()) {
        g_tf_phase = TF_PHASE_ERROR;
        snprintf(g_tf_local_message, sizeof(g_tf_local_message), "PAIR COMMAND BUSY");
    }
    g_tf_dirty = true;
}

static void tf_begin_pairing(pico07_device_type_t target)
{
    g_tf_active = true;
    g_tf_target = target;
    set_screen(SCREEN_PAIRING);
    tf_start_scan(true);
}

static int tf_find_untried_result(void)
{
    for (uint8_t i = 0; i < g_pair.result_count; ++i) {
        const pico07_scan_result_t *result = &g_pair.results[i];
        if (!tf_was_tried(result->addr_type, result->addr)) return (int)i;
    }
    return -1;
}

static void tf_request_next_candidate(void)
{
    const int index = tf_find_untried_result();
    if (index < 0) return;

    const pico07_scan_result_t *result = &g_pair.results[index];
    tf_mark_tried(result);
    g_tf_current.addr_type = result->addr_type;
    memcpy(g_tf_current.addr, result->addr, 6);
    g_tf_have_current = true;

    g_tf_action_revision = g_pair.revision;
    if (pico07_pairing_request_connect_result((uint8_t)index)) {
        g_tf_phase = TF_PHASE_CONNECT;
    } else {
        g_tf_phase = TF_PHASE_ERROR;
        snprintf(g_tf_local_message, sizeof(g_tf_local_message), "CONNECT COMMAND BUSY");
    }
    g_tf_dirty = true;
}

static void tf_process_pair_state(void)
{
    if (!g_tf_active || g_screen != SCREEN_PAIRING) return;

    if (g_tf_phase == TF_PHASE_SCAN) {
        if (g_pair.state == PICO07_PAIR_SCANNING ||
            g_pair.state == PICO07_PAIR_RESULTS) {
            if (tf_find_untried_result() >= 0) {
                tf_request_next_candidate();
                return;
            }
            if (g_pair.state == PICO07_PAIR_RESULTS &&
                g_pair.revision != g_tf_action_revision) {
                g_tf_phase = TF_PHASE_ERROR;
                snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                         "NO %s FOUND", tf_target_name());
                g_tf_dirty = true;
            }
            return;
        }
        if (g_pair.state == PICO07_PAIR_ERROR &&
            g_pair.revision != g_tf_action_revision) {
            g_tf_phase = TF_PHASE_ERROR;
            snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                     "%s", g_pair.message[0] ? g_pair.message : "PAIRING FAILED");
            g_tf_dirty = true;
        }
        return;
    }

    if (g_tf_phase == TF_PHASE_CONNECT) {
        if (g_pair.revision == g_tf_action_revision) return;
        if (g_pair.state == PICO07_PAIR_SUCCESS) {
            if (g_pair.last_type == g_tf_target) {
                g_tf_active = false;
                set_screen(SCREEN_DEVICE_SUCCESS);
                return;
            }

            if (!g_tf_have_current ||
                !pico07_pairing_request_delete_saved(g_tf_current.addr_type,
                                                     g_tf_current.addr)) {
                g_tf_phase = TF_PHASE_ERROR;
                snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                         "TEMP DELETE FAILED");
                g_tf_dirty = true;
                return;
            }
            g_tf_phase = TF_PHASE_DELETE;
            g_tf_action_revision = g_pair.revision;
            snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                     "SKIP %s", pico07_device_type_name(g_pair.last_type));
            g_tf_dirty = true;
            return;
        }

        if (g_pair.state == PICO07_PAIR_ERROR) {
            g_tf_phase = TF_PHASE_ERROR;
            snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                     "%s", g_pair.message[0] ? g_pair.message : "PAIRING FAILED");
            g_tf_dirty = true;
        }
        return;
    }

    if (g_tf_phase == TF_PHASE_DELETE) {
        if (g_pair.revision == g_tf_action_revision) return;
        if (g_pair.state == PICO07_PAIR_SUCCESS) {
            tf_start_scan(false);
            return;
        }
        if (g_pair.state == PICO07_PAIR_ERROR) {
            g_tf_phase = TF_PHASE_ERROR;
            snprintf(g_tf_local_message, sizeof(g_tf_local_message),
                     "TEMP DELETE FAILED");
            g_tf_dirty = true;
        }
    }
}

static void tf_move_device_selection(bool down)
{
    const uint8_t max_selection = (uint8_t)(2u + g_catalog.count);
    if (down) {
        g_device_selection = g_device_selection >= max_selection
                               ? 0u : (uint8_t)(g_device_selection + 1u);
    } else {
        g_device_selection = g_device_selection == 0u
                               ? max_selection : (uint8_t)(g_device_selection - 1u);
    }
    g_tf_dirty = true;
}

static void tf_handle_devices_pressed(pico_hat_input_t input)
{
    if (input == PICO_HAT_INPUT_JOY_UP) {
        tf_move_device_selection(false);
    } else if (input == PICO_HAT_INPUT_JOY_DOWN) {
        tf_move_device_selection(true);
    } else if (input == PICO_HAT_INPUT_JOY_PRESS) {
        if (g_device_selection == 0u) {
            tf_begin_pairing(PICO07_TYPE_MOUSE);
        } else if (g_device_selection == 1u) {
            tf_begin_pairing(PICO07_TYPE_KEYBOARD);
        } else if (g_device_selection == 2u) {
            tf_begin_pairing(PICO07_TYPE_COMPOSITE);
        } else {
            const uint8_t record_index = (uint8_t)(g_device_selection - 3u);
            if (record_index < g_catalog.count) {
                g_tf_active = false;
                g_selected_record_index = record_index;
                g_detail_action = 0u;
                set_screen(SCREEN_DEVICE_DETAILS);
            }
        }
    } else if (input == PICO_HAT_INPUT_KEY1) {
        set_screen(SCREEN_HOME);
    } else if (input == PICO_HAT_INPUT_KEY3) {
        g_help_return = SCREEN_DEVICES;
        set_screen(SCREEN_HELP);
    }
}

static void tf_handle_pairing_pressed(pico_hat_input_t input)
{
    if (input == PICO_HAT_INPUT_KEY1) {
        (void)pico07_pairing_request_cancel();
        g_tf_active = false;
        g_device_selection = 0u;
        g_tf_dirty = true;
        set_screen(SCREEN_DEVICES);
    } else if (input == PICO_HAT_INPUT_KEY3) {
        g_help_return = SCREEN_PAIRING;
        set_screen(SCREEN_HELP);
    } else if (input == PICO_HAT_INPUT_JOY_PRESS && g_tf_phase == TF_PHASE_ERROR) {
        tf_start_scan(true);
    }
}

void pico06_ui_init(void)
{
    pico06_ui_init_legacy();
    g_tf_active = false;
    g_tf_target = PICO07_TYPE_UNKNOWN;
    g_tf_phase = TF_PHASE_IDLE;
    g_tf_tried_count = 0u;
    g_tf_have_current = false;
    g_tf_action_revision = 0u;
    g_tf_dirty = true;
    g_tf_last_screen = SCREEN_HOME;
    memset(g_tf_local_message, 0, sizeof(g_tf_local_message));
}

void pico06_ui_task(void)
{
    const bool own_devices = g_screen == SCREEN_DEVICES;
    const bool own_pairing = g_screen == SCREEN_PAIRING && g_tf_active;

    // The legacy task can transition HOME -> SCREEN_DEVICES and consume its
    // own dirty flag in the same tick. Track screen entry independently so the
    // type-first overlay always redraws its menu on the following tick.
    if (g_screen != g_tf_last_screen) {
        g_tf_last_screen = g_screen;
        g_tf_dirty = true;
    }

    if (!own_devices && !own_pairing) {
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
        set_screen(SCREEN_HOME);
        return;
    }

    tf_refresh_snapshots();
    if (own_pairing) tf_process_pair_state();

    // The state processor may have transitioned to a legacy success screen.
    if (g_screen != SCREEN_DEVICES && !(g_screen == SCREEN_PAIRING && g_tf_active)) {
        return;
    }

    pico_hat_event_t event;
    if (pico_hat_ui_poll_event(&event) && event.pressed) {
        if (event.input == PICO_HAT_INPUT_KEY4) {
            // KEY4 is owned by pico_hat_ui screen-lock handling.
        } else if (g_screen == SCREEN_DEVICES) {
            tf_handle_devices_pressed(event.input);
        } else {
            tf_handle_pairing_pressed(event.input);
        }
    }

    if (g_screen == SCREEN_DEVICES || (g_screen == SCREEN_PAIRING && g_tf_active)) {
        tf_render_task();
    }
}
