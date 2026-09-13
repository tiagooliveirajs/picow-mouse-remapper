#include "logitech_hidpp.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "device_profile.h"
#include "pico/critical_section.h"
#include "remap_profile.h"

#define HIDPP_REPORT_ID_SHORT 0x10u
#define HIDPP_REPORT_ID_LONG  0x11u
#define HIDPP_LONG_PAYLOAD_LEN 19u
#define HIDPP_DEVICE_INDEX 0xffu
#define HIDPP_SOFTWARE_ID 0x02u
#define HIDPP_ROOT_FEATURE_INDEX 0x00u
#define HIDPP_REPROG_CONTROLS_V4 0x1b04u
#define HIDPP_BACK_CID 0x0053u
#define HIDPP_FORWARD_CID 0x0056u
#define HIDPP_SET_CONTROL_REPORTING_FUNCTION 3u
#define HIDPP_DIVERT_ENABLE_FLAGS 0x03u
#define HIDPP_DIVERT_DISABLE_FLAGS 0x02u
#define HIDPP_MAX_SEND_RETRIES 32u

typedef enum {
    HIDPP_STATE_IDLE = 0,
    HIDPP_STATE_NEED_FEATURE,
    HIDPP_STATE_WAIT_FEATURE,
    HIDPP_STATE_READY,
    HIDPP_STATE_WAIT_SET,
    HIDPP_STATE_FEATURE_FAILED,
} hidpp_state_t;

extern uint8_t poc_hids_send_hidpp_long(const uint8_t *payload, uint8_t payload_len);

static critical_section_t g_lock;
static bool g_initialized;
static logitech_hidpp_snapshot_t g_snapshot;

// Core1-only protocol state.
static bool g_connected;
static hidpp_state_t g_state;
static uint8_t g_feature_index;
static uint8_t g_desired_mask;
static uint8_t g_applied_mask;
static uint8_t g_held_mask;
static uint8_t g_supported_mask;
static uint8_t g_protocol_failed_mask;
static uint8_t g_external_unsupported_mask;
static uint8_t g_last_desired_mask;
static uint8_t g_pending_bit;
static uint16_t g_pending_cid;
static bool g_pending_enable;
static uint8_t g_send_retries;

static uint16_t cid_for_bit(uint8_t bit)
{
    return bit == LOGITECH_HIDPP_SOURCE_BACK ? HIDPP_BACK_CID : HIDPP_FORWARD_CID;
}

static uint8_t bit_for_cid(uint16_t cid)
{
    if (cid == HIDPP_BACK_CID) return LOGITECH_HIDPP_SOURCE_BACK;
    if (cid == HIDPP_FORWARD_CID) return LOGITECH_HIDPP_SOURCE_FORWARD;
    return 0;
}

static void publish(void)
{
    critical_section_enter_blocking(&g_lock);
    ++g_snapshot.revision;
    g_snapshot.connected = g_connected;
    g_snapshot.feature_available = g_feature_index != 0;
    g_snapshot.feature_index = g_feature_index;
    g_snapshot.desired_mask = g_desired_mask;
    g_snapshot.applied_mask = g_applied_mask;
    g_snapshot.held_mask = g_held_mask;
    g_snapshot.supported_mask = g_supported_mask;
    g_snapshot.failed_mask = (uint8_t)(g_protocol_failed_mask |
                                       g_external_unsupported_mask);
    critical_section_exit(&g_lock);
}

void logitech_hidpp_init(void)
{
    if (g_initialized) return;
    critical_section_init(&g_lock);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_initialized = true;
}

bool logitech_hidpp_get_snapshot(logitech_hidpp_snapshot_t *snapshot)
{
    if (!g_initialized || snapshot == NULL) return false;
    critical_section_enter_blocking(&g_lock);
    memcpy(snapshot, &g_snapshot, sizeof(*snapshot));
    critical_section_exit(&g_lock);
    return snapshot->revision != 0;
}

void logitech_hidpp_on_connect(void)
{
    g_connected = true;
    g_state = HIDPP_STATE_IDLE;
    g_feature_index = 0;
    g_desired_mask = 0;
    g_applied_mask = 0;
    g_held_mask = 0;
    g_supported_mask = 0;
    g_protocol_failed_mask = 0;
    g_external_unsupported_mask = 0;
    g_last_desired_mask = 0;
    g_pending_bit = 0;
    g_pending_cid = 0;
    g_pending_enable = false;
    g_send_retries = 0;
    publish();
    printf("[PICO-06] HID++ backend connected\n");
}

void logitech_hidpp_on_disconnect(void)
{
    g_connected = false;
    g_state = HIDPP_STATE_IDLE;
    g_feature_index = 0;
    g_desired_mask = 0;
    g_applied_mask = 0;
    g_held_mask = 0;
    g_supported_mask = 0;
    g_protocol_failed_mask = 0;
    g_external_unsupported_mask = 0;
    g_last_desired_mask = 0;
    g_pending_bit = 0;
    g_send_retries = 0;
    publish();
}

static device_drag_backend_t backend_for_source(device_source_button_t source,
                                                device_drag_fix_policy_t policy)
{
    return device_profile_resolve_drag_backend(source, policy);
}

static void derive_desired(uint8_t *desired, uint8_t *unsupported)
{
    *desired = 0;
    *unsupported = 0;

    remap_profile_snapshot_t profile;
    if (!remap_profile_get_snapshot(&profile) || !profile.connected) return;

    if (profile.active.mode == DEVICE_PROFILE_DEFAULT_REMAP) {
        const device_drag_backend_t backend =
            backend_for_source(DEVICE_SOURCE_FORWARD, DEVICE_DRAG_FIX_AUTO);
        if (backend == DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4 ||
            backend == DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4) {
            *desired |= LOGITECH_HIDPP_SOURCE_FORWARD;
        }
        return;
    }

    if (profile.active.mode != DEVICE_PROFILE_CUSTOM_REMAP) return;

    if (profile.active.mappings[REMAP_SOURCE_BACK] != REMAP_TARGET_PASSTHROUGH) {
        const device_drag_backend_t backend =
            backend_for_source(DEVICE_SOURCE_BACK, profile.active.drag_fix_back);
        if (backend == DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4 ||
            backend == DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4) {
            *desired |= LOGITECH_HIDPP_SOURCE_BACK;
        } else if (backend == DEVICE_DRAG_BACKEND_UNSUPPORTED &&
                   profile.active.drag_fix_back == DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED) {
            *unsupported |= LOGITECH_HIDPP_SOURCE_BACK;
        }
    }

    if (profile.active.mappings[REMAP_SOURCE_FORWARD] != REMAP_TARGET_PASSTHROUGH) {
        const device_drag_backend_t backend =
            backend_for_source(DEVICE_SOURCE_FORWARD, profile.active.drag_fix_forward);
        if (backend == DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4 ||
            backend == DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4) {
            *desired |= LOGITECH_HIDPP_SOURCE_FORWARD;
        } else if (backend == DEVICE_DRAG_BACKEND_UNSUPPORTED &&
                   profile.active.drag_fix_forward == DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED) {
            *unsupported |= LOGITECH_HIDPP_SOURCE_FORWARD;
        }
    }
}

static bool send_get_feature(void)
{
    uint8_t payload[HIDPP_LONG_PAYLOAD_LEN] = {0};
    payload[0] = HIDPP_DEVICE_INDEX;
    payload[1] = HIDPP_ROOT_FEATURE_INDEX;
    payload[2] = HIDPP_SOFTWARE_ID;
    payload[3] = (uint8_t)(HIDPP_REPROG_CONTROLS_V4 >> 8);
    payload[4] = (uint8_t)(HIDPP_REPROG_CONTROLS_V4 & 0xffu);

    const uint8_t rc = poc_hids_send_hidpp_long(payload, sizeof(payload));
    if (rc == ERROR_CODE_SUCCESS) {
        g_state = HIDPP_STATE_WAIT_FEATURE;
        g_send_retries = 0;
        printf("[PICO-06] HID++ query REPROG_CONTROLS_V4\n");
        return true;
    }
    return false;
}

static bool send_set_divert(uint8_t bit, bool enable)
{
    const uint16_t cid = cid_for_bit(bit);
    uint8_t payload[HIDPP_LONG_PAYLOAD_LEN] = {0};
    payload[0] = HIDPP_DEVICE_INDEX;
    payload[1] = g_feature_index;
    payload[2] = (uint8_t)((HIDPP_SET_CONTROL_REPORTING_FUNCTION << 4) |
                           HIDPP_SOFTWARE_ID);
    payload[3] = (uint8_t)(cid >> 8);
    payload[4] = (uint8_t)(cid & 0xffu);
    payload[5] = enable ? HIDPP_DIVERT_ENABLE_FLAGS : HIDPP_DIVERT_DISABLE_FLAGS;

    const uint8_t rc = poc_hids_send_hidpp_long(payload, sizeof(payload));
    if (rc == ERROR_CODE_SUCCESS) {
        g_pending_bit = bit;
        g_pending_cid = cid;
        g_pending_enable = enable;
        g_state = HIDPP_STATE_WAIT_SET;
        g_send_retries = 0;
        printf("[PICO-06] HID++ CID %04x divert %s request\n",
               cid, enable ? "ON" : "OFF");
        return true;
    }
    return false;
}

static void mark_send_failure(void)
{
    if (++g_send_retries < HIDPP_MAX_SEND_RETRIES) return;

    if (g_state == HIDPP_STATE_NEED_FEATURE || g_state == HIDPP_STATE_IDLE) {
        g_protocol_failed_mask |= g_desired_mask;
        g_state = HIDPP_STATE_FEATURE_FAILED;
    } else if (g_state == HIDPP_STATE_READY && g_pending_bit != 0) {
        g_protocol_failed_mask |= g_pending_bit;
    }
    g_send_retries = 0;
    publish();
}

void logitech_hidpp_core1_task(void)
{
    if (!g_initialized || !g_connected) return;

    uint8_t desired = 0;
    uint8_t unsupported = 0;
    derive_desired(&desired, &unsupported);

    if (desired != g_last_desired_mask) {
        const uint8_t newly_desired = (uint8_t)(desired & ~g_last_desired_mask);
        g_protocol_failed_mask &= (uint8_t)~newly_desired;
        g_protocol_failed_mask &= desired;
        g_last_desired_mask = desired;
        g_desired_mask = desired;
        g_external_unsupported_mask = unsupported;
        if (newly_desired != 0 && g_feature_index == 0 &&
            g_state == HIDPP_STATE_FEATURE_FAILED) {
            g_state = HIDPP_STATE_NEED_FEATURE;
        }
        publish();
    } else if (unsupported != g_external_unsupported_mask) {
        g_external_unsupported_mask = unsupported;
        publish();
    }

    if (g_state == HIDPP_STATE_WAIT_FEATURE || g_state == HIDPP_STATE_WAIT_SET) {
        return;
    }

    if (g_feature_index == 0) {
        if (g_desired_mask == 0) {
            g_state = HIDPP_STATE_IDLE;
            return;
        }
        if (g_state == HIDPP_STATE_FEATURE_FAILED) return;
        g_state = HIDPP_STATE_NEED_FEATURE;
        if (!send_get_feature()) mark_send_failure();
        return;
    }

    g_state = HIDPP_STATE_READY;
    const uint8_t effective_desired = (uint8_t)(g_desired_mask &
                                                 ~g_protocol_failed_mask);
    const uint8_t diff = (uint8_t)(g_applied_mask ^ effective_desired);
    if (diff == 0) return;

    const uint8_t bit = (diff & LOGITECH_HIDPP_SOURCE_BACK) != 0
                          ? LOGITECH_HIDPP_SOURCE_BACK
                          : LOGITECH_HIDPP_SOURCE_FORWARD;
    const bool enable = (effective_desired & bit) != 0;
    g_pending_bit = bit;
    if (!send_set_divert(bit, enable)) {
        if (++g_send_retries >= HIDPP_MAX_SEND_RETRIES) {
            if (enable) g_protocol_failed_mask |= bit;
            g_send_retries = 0;
            publish();
        }
    }
}

static bool error_matches(const uint8_t *report,
                          uint16_t len,
                          uint8_t expected_feature,
                          uint8_t expected_function)
{
    if (len < 5 || report[1] != 0xffu) return false;
    const uint8_t original_feature = report[2];
    const uint8_t original_function = (uint8_t)(report[3] >> 4);
    const uint8_t original_sw = (uint8_t)(report[3] & 0x0fu);
    return original_feature == expected_feature &&
           original_function == expected_function &&
           original_sw == HIDPP_SOFTWARE_ID;
}

bool logitech_hidpp_process_report(uint8_t report_id,
                                   const uint8_t *report,
                                   uint16_t report_len)
{
    if (!g_initialized || !g_connected || report == NULL) return false;
    if (report_id != HIDPP_REPORT_ID_SHORT && report_id != HIDPP_REPORT_ID_LONG) {
        return false;
    }

    const bool backend_interested = g_desired_mask != 0 || g_applied_mask != 0 ||
                                    g_state == HIDPP_STATE_WAIT_FEATURE ||
                                    g_state == HIDPP_STATE_WAIT_SET ||
                                    g_feature_index != 0;
    if (!backend_interested) return false;
    if (report_len < 3) return true;

    if (g_state == HIDPP_STATE_WAIT_FEATURE &&
        error_matches(report, report_len, HIDPP_ROOT_FEATURE_INDEX, 0)) {
        printf("[PICO-06] HID++ feature 1B04 unsupported error=%02x\n", report[4]);
        g_protocol_failed_mask |= g_desired_mask;
        g_state = HIDPP_STATE_FEATURE_FAILED;
        publish();
        return true;
    }

    if (g_state == HIDPP_STATE_WAIT_SET &&
        error_matches(report, report_len, g_feature_index,
                      HIDPP_SET_CONTROL_REPORTING_FUNCTION)) {
        printf("[PICO-06] HID++ CID %04x unsupported error=%02x\n",
               g_pending_cid, report[4]);
        if (g_pending_enable) g_protocol_failed_mask |= g_pending_bit;
        else g_applied_mask &= (uint8_t)~g_pending_bit;
        g_held_mask &= (uint8_t)~g_pending_bit;
        g_state = HIDPP_STATE_READY;
        g_pending_bit = 0;
        publish();
        return true;
    }

    const uint8_t feature = report[1];
    const uint8_t function = (uint8_t)(report[2] >> 4);
    const uint8_t sw = (uint8_t)(report[2] & 0x0fu);

    if (g_state == HIDPP_STATE_WAIT_FEATURE &&
        feature == HIDPP_ROOT_FEATURE_INDEX && function == 0 &&
        sw == HIDPP_SOFTWARE_ID && report_len >= 4) {
        g_feature_index = report[3];
        if (g_feature_index == 0) {
            g_protocol_failed_mask |= g_desired_mask;
            g_state = HIDPP_STATE_FEATURE_FAILED;
        } else {
            g_state = HIDPP_STATE_READY;
            printf("[PICO-06] HID++ feature 1B04 index=%02x\n", g_feature_index);
        }
        publish();
        return true;
    }

    if (g_state == HIDPP_STATE_WAIT_SET &&
        feature == g_feature_index &&
        function == HIDPP_SET_CONTROL_REPORTING_FUNCTION &&
        sw == HIDPP_SOFTWARE_ID) {
        if (g_pending_enable) {
            g_applied_mask |= g_pending_bit;
            g_supported_mask |= g_pending_bit;
            g_protocol_failed_mask &= (uint8_t)~g_pending_bit;
        } else {
            g_applied_mask &= (uint8_t)~g_pending_bit;
            g_held_mask &= (uint8_t)~g_pending_bit;
        }
        printf("[PICO-06] HID++ CID %04x divert %s active=%02x\n",
               g_pending_cid,
               g_pending_enable ? "ON" : "OFF",
               g_applied_mask);
        g_pending_bit = 0;
        g_state = HIDPP_STATE_READY;
        publish();
        return true;
    }

    if (g_feature_index != 0 && feature == g_feature_index && function == 0) {
        uint8_t held = 0;
        for (uint16_t pos = 3; pos + 1 < report_len; pos += 2) {
            const uint16_t cid = (uint16_t)(((uint16_t)report[pos] << 8) |
                                            report[pos + 1]);
            if (cid == 0) break;
            held |= bit_for_cid(cid);
        }
        held &= g_applied_mask;
        if (held != g_held_mask) {
            g_held_mask = held;
            publish();
        }
        return true;
    }

    // HID++ control traffic for this feature/query is not USB input.
    return true;
}
