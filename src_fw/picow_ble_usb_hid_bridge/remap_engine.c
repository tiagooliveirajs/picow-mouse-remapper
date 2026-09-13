#include "remap_engine.h"

#include <stdint.h>
#include <string.h>

#include "logitech_hidpp.h"
#include "usb_descriptors.h"

#define SOURCE_QUEUE_SIZE 8u
#define HID_KEY_ESCAPE 0x29u

static remap_profile_config_t g_active;
static bool g_have_profile;
static uint32_t g_profile_revision;
static uint32_t g_hidpp_revision;

static uint8_t g_native_buttons;
static uint8_t g_last_effective_sources;
static uint8_t g_usb_mouse_buttons;
static bool g_usb_escape_pressed;

static remap_source_t g_source_queue[SOURCE_QUEUE_SIZE];
static uint8_t g_source_head;
static uint8_t g_source_tail;

static bool config_equal(const remap_profile_config_t *a,
                         const remap_profile_config_t *b)
{
    return a->mode == b->mode &&
           a->drag_fix_back == b->drag_fix_back &&
           a->drag_fix_forward == b->drag_fix_forward &&
           memcmp(a->mappings, b->mappings, REMAP_SOURCE_COUNT) == 0;
}

static void source_queue_clear(void)
{
    g_source_head = 0;
    g_source_tail = 0;
}

static void source_queue_push(remap_source_t source)
{
    const uint8_t next = (uint8_t)((g_source_tail + 1u) % SOURCE_QUEUE_SIZE);
    if (next == g_source_head) return;
    g_source_queue[g_source_tail] = source;
    g_source_tail = next;
}

bool remap_engine_poll_source_down(remap_source_t *source)
{
    if (source == NULL || g_source_head == g_source_tail) return false;
    *source = g_source_queue[g_source_head];
    g_source_head = (uint8_t)((g_source_head + 1u) % SOURCE_QUEUE_SIZE);
    return true;
}

static void capture_source_down_edges(uint8_t effective)
{
    const uint8_t down = (uint8_t)(effective & ~g_last_effective_sources);
    for (uint8_t i = 0; i < REMAP_SOURCE_COUNT; ++i) {
        if ((down & (uint8_t)(1u << i)) != 0) {
            source_queue_push((remap_source_t)i);
        }
    }
    g_last_effective_sources = effective;
}

static uint8_t effective_source_buttons(void)
{
    uint8_t effective = g_native_buttons;
    logitech_hidpp_snapshot_t hidpp;
    if (!logitech_hidpp_get_snapshot(&hidpp) || !hidpp.connected) {
        return effective;
    }

    if ((hidpp.applied_mask & LOGITECH_HIDPP_SOURCE_BACK) != 0) {
        const uint8_t mask = (uint8_t)(1u << REMAP_SOURCE_BACK);
        if ((hidpp.held_mask & LOGITECH_HIDPP_SOURCE_BACK) != 0) effective |= mask;
        else effective &= (uint8_t)~mask;
    }
    if ((hidpp.applied_mask & LOGITECH_HIDPP_SOURCE_FORWARD) != 0) {
        const uint8_t mask = (uint8_t)(1u << REMAP_SOURCE_FORWARD);
        if ((hidpp.held_mask & LOGITECH_HIDPP_SOURCE_FORWARD) != 0) effective |= mask;
        else effective &= (uint8_t)~mask;
    }
    return effective;
}

static uint8_t target_to_button_bit(uint8_t target)
{
    if (target < REMAP_TARGET_LEFT || target > REMAP_TARGET_FORWARD) return 0;
    return (uint8_t)(1u << (target - 1u));
}

static uint8_t compute_mouse_buttons(uint8_t source_buttons, bool *escape_pressed)
{
    *escape_pressed = false;

    if (!g_have_profile || g_active.mode == DEVICE_PROFILE_PASSTHROUGH) {
        return (uint8_t)(source_buttons & 0x1fu);
    }

    if (g_active.mode == DEVICE_PROFILE_DEFAULT_REMAP) {
        uint8_t out = 0;
        *escape_pressed = (source_buttons & (1u << REMAP_SOURCE_LEFT)) != 0;
        if ((source_buttons & (1u << REMAP_SOURCE_RIGHT)) != 0)
            out |= target_to_button_bit(REMAP_TARGET_BACK);
        if ((source_buttons & (1u << REMAP_SOURCE_FORWARD)) != 0)
            out |= target_to_button_bit(REMAP_TARGET_LEFT);
        if ((source_buttons & (1u << REMAP_SOURCE_BACK)) != 0)
            out |= target_to_button_bit(REMAP_TARGET_RIGHT);
        if ((source_buttons & (1u << REMAP_SOURCE_MIDDLE)) != 0)
            out |= target_to_button_bit(REMAP_TARGET_FORWARD);
        return out;
    }

    uint8_t out = 0;
    for (uint8_t source = 0; source < REMAP_SOURCE_COUNT; ++source) {
        if ((source_buttons & (uint8_t)(1u << source)) == 0) continue;
        uint8_t target = g_active.mappings[source];
        if (target == REMAP_TARGET_PASSTHROUGH) {
            target = (uint8_t)(source + 1u);
        }
        out |= target_to_button_bit(target);
    }
    return out;
}

static void write_mouse(ST_HID_RPT *out,
                        uint8_t buttons,
                        int16_t x,
                        int16_t y,
                        int8_t wheel,
                        int8_t pan)
{
    memset(out, 0, sizeof(*out));
    out->report_id = REPORT_ID_MOUSE;
    out->report_len = 7u;
    out->report[0] = buttons;
    out->report[1] = (uint8_t)(x & 0xff);
    out->report[2] = (uint8_t)(((uint16_t)x >> 8) & 0xffu);
    out->report[3] = (uint8_t)(y & 0xff);
    out->report[4] = (uint8_t)(((uint16_t)y >> 8) & 0xffu);
    out->report[5] = (uint8_t)wheel;
    out->report[6] = (uint8_t)pan;
}

static void write_keyboard(ST_HID_RPT *out, bool escape_pressed)
{
    memset(out, 0, sizeof(*out));
    out->report_id = REPORT_ID_KEYBOARD;
    out->report_len = 8u;
    if (escape_pressed) out->report[2] = HID_KEY_ESCAPE;
}

static bool sync_profile(bool *changed)
{
    *changed = false;
    remap_profile_snapshot_t snapshot;
    if (!remap_profile_get_snapshot(&snapshot)) return false;

    if (snapshot.revision == g_profile_revision) return true;
    g_profile_revision = snapshot.revision;

    remap_profile_config_t next;
    remap_profile_make_passthrough(&next);
    if (snapshot.connected) next = snapshot.active;

    if (!g_have_profile) {
        g_active = next;
        g_have_profile = true;
        return true;
    }

    if (!config_equal(&g_active, &next)) {
        g_active = next;
        *changed = true;
    }
    return true;
}

void remap_engine_init(void)
{
    remap_profile_make_passthrough(&g_active);
    g_have_profile = false;
    g_profile_revision = 0;
    g_hidpp_revision = 0;
    g_native_buttons = 0;
    g_last_effective_sources = 0;
    g_usb_mouse_buttons = 0;
    g_usb_escape_pressed = false;
    source_queue_clear();
}

void remap_engine_reset_device(void)
{
    g_native_buttons = 0;
    g_last_effective_sources = 0;
    g_hidpp_revision = 0;
    source_queue_clear();
}

size_t remap_engine_neutralize(ST_HID_RPT *outputs, size_t output_capacity)
{
    size_t count = 0;
    if (outputs != NULL && g_usb_escape_pressed && count < output_capacity) {
        write_keyboard(&outputs[count++], false);
    }
    if (outputs != NULL && g_usb_mouse_buttons != 0 && count < output_capacity) {
        write_mouse(&outputs[count++], 0, 0, 0, 0, 0);
    }

    g_usb_escape_pressed = false;
    g_usb_mouse_buttons = 0;
    g_native_buttons = 0;
    g_last_effective_sources = 0;
    source_queue_clear();
    return count;
}

size_t remap_engine_poll_async(ST_HID_RPT *outputs, size_t output_capacity)
{
    if (outputs == NULL || output_capacity == 0) return 0;

    bool profile_changed = false;
    (void)sync_profile(&profile_changed);
    if (profile_changed) {
        const size_t neutral = remap_engine_neutralize(outputs, output_capacity);
        // Apply starts from a clean source state. A held physical button must be
        // released/re-pressed after a profile transition instead of being
        // synthesized retroactively under a new mapping.
        return neutral;
    }

    logitech_hidpp_snapshot_t hidpp;
    if (!logitech_hidpp_get_snapshot(&hidpp)) return 0;
    if (hidpp.revision == g_hidpp_revision) return 0;
    g_hidpp_revision = hidpp.revision;

    const uint8_t effective = effective_source_buttons();
    capture_source_down_edges(effective);

    bool escape = false;
    const uint8_t mouse = compute_mouse_buttons(effective, &escape);
    size_t count = 0;
    if (escape != g_usb_escape_pressed && count < output_capacity) {
        write_keyboard(&outputs[count++], escape);
        g_usb_escape_pressed = escape;
    }
    if (mouse != g_usb_mouse_buttons && count < output_capacity) {
        write_mouse(&outputs[count++], mouse, 0, 0, 0, 0);
        g_usb_mouse_buttons = mouse;
    }
    return count;
}

size_t remap_engine_process_canonical(const ST_HID_RPT *input,
                                      ST_HID_RPT *outputs,
                                      size_t output_capacity)
{
    if (input == NULL || outputs == NULL || output_capacity == 0) return 0;
    if (input->report_id != REPORT_ID_MOUSE || input->report_len < 7u) return 0;

    bool profile_changed = false;
    (void)sync_profile(&profile_changed);
    if (profile_changed) {
        // Caller polls async before consuming raw traffic, so this path is only
        // a race fallback. Neutralize and intentionally drop this one packet.
        return remap_engine_neutralize(outputs, output_capacity);
    }

    g_native_buttons = (uint8_t)(input->report[0] & 0x1fu);
    const uint8_t effective = effective_source_buttons();
    capture_source_down_edges(effective);

    const int16_t x = (int16_t)((uint16_t)input->report[1] |
                                ((uint16_t)input->report[2] << 8));
    const int16_t y = (int16_t)((uint16_t)input->report[3] |
                                ((uint16_t)input->report[4] << 8));
    const int8_t wheel = (int8_t)input->report[5];
    const int8_t pan = (int8_t)input->report[6];

    bool escape = false;
    const uint8_t mouse = compute_mouse_buttons(effective, &escape);

    size_t count = 0;
    if (escape != g_usb_escape_pressed && count < output_capacity) {
        write_keyboard(&outputs[count++], escape);
        g_usb_escape_pressed = escape;
    }

    if (count < output_capacity) {
        write_mouse(&outputs[count++], mouse, x, y, wheel, pan);
        g_usb_mouse_buttons = mouse;
    }
    return count;
}
