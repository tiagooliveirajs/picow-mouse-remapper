// PICO-07 wrapper around the existing HOGP host implementation.
//
// Core1 owns all BTstack operations. Core0 UI posts pairing/device-management
// commands through a critical-section mailbox and reads immutable snapshots.

#include "btstack.h"
#include "ble/le_device_db.h"
#include "device_profile.h"
#include "logitech_hidpp.h"
#include "pico/critical_section.h"
#include "pico07_pairing.h"
#include "remap_profile.h"
#include "remote_hid_queue.h"

static inline const uint8_t *poc_btstack_hid_report_payload(const uint8_t *event)
{
    const uint16_t len = gattservice_subevent_hid_report_get_report_len(event);
    const uint8_t *report = gattservice_subevent_hid_report_get_report(event);
    return len > 0 ? report + 1 : report;
}

static inline uint16_t poc_btstack_hid_report_payload_len(const uint8_t *event)
{
    const uint16_t len = gattservice_subevent_hid_report_get_report_len(event);
    return len > 0 ? (uint16_t)(len - 1) : 0;
}

static bool pico07_remote_enqueue(ULONG queue_kind, PVOID data);

#define CMN_Enqueue(kind, data) pico07_remote_enqueue((kind), (data))
#define ble_host_main poc_ble_host_main_original
#define gattservice_subevent_hid_report_get_report     poc_btstack_hid_report_payload
#define gattservice_subevent_hid_report_get_report_len poc_btstack_hid_report_payload_len
#include "hog_host_demo.c"
#undef gattservice_subevent_hid_report_get_report
#undef gattservice_subevent_hid_report_get_report_len
#undef ble_host_main
#undef CMN_Enqueue

#define PICO07_PROFILE_POLL_MS 50u
#define ADV_TYPE_SHORT_NAME    0x08u
#define ADV_TYPE_COMPLETE_NAME 0x09u

typedef enum {
    PAIR_CMD_NONE = 0,
    PAIR_CMD_SCAN,
    PAIR_CMD_CONNECT_RESULT,
    PAIR_CMD_CONNECT_SAVED,
    PAIR_CMD_DELETE_SAVED,
    PAIR_CMD_CANCEL,
} pair_command_t;

typedef enum {
    AFTER_DISCONNECT_NONE = 0,
    AFTER_DISCONNECT_SCAN,
    AFTER_DISCONNECT_CONNECT,
    AFTER_DISCONNECT_DELETE,
    AFTER_DISCONNECT_RESTORE_PRIMARY,
} after_disconnect_t;

typedef struct {
    pair_command_t kind;
    uint8_t result_index;
    uint8_t addr_type;
    uint8_t addr[6];
} pair_command_request_t;

static btstack_timer_source_t g_pico07_profile_timer;
static uint16_t g_pico07_attached_hids_cid;
static bool g_pico07_profile_attached;

static critical_section_t g_pair_lock;
static bool g_pair_initialized;
static pico07_pairing_snapshot_t g_pair_snapshot;
static pair_command_request_t g_pair_command;
static bool g_pair_command_pending;

static bool g_managed_connection;
static bool g_managed_is_new_pair;
static bool g_hids_transport_ready;
static after_disconnect_t g_after_disconnect;
static le_device_addr_t g_pending_target;
static uint8_t g_delete_addr_type;
static uint8_t g_delete_addr[6];

static bool pico07_remote_enqueue(ULONG queue_kind, PVOID data)
{
    if (queue_kind != CMN_QUE_KIND_HID_RPT || data == NULL) return false;
    const ST_HID_RPT *report = (const ST_HID_RPT *)data;
    if (logitech_hidpp_process_report(report->report_id,
                                      report->report,
                                      report->report_len)) {
        return true;
    }
    return remote_hid_queue_enqueue(report);
}

static void pair_publish_state(pico07_pair_state_t state, const char *message)
{
    critical_section_enter_blocking(&g_pair_lock);
    ++g_pair_snapshot.revision;
    g_pair_snapshot.state = state;
    if (message != NULL) {
        snprintf(g_pair_snapshot.message, sizeof(g_pair_snapshot.message), "%s", message);
    } else {
        g_pair_snapshot.message[0] = '\0';
    }
    critical_section_exit(&g_pair_lock);
}

static void pair_publish_result(pico07_device_type_t type,
                                bool was_new_pair,
                                const char *message)
{
    critical_section_enter_blocking(&g_pair_lock);
    ++g_pair_snapshot.revision;
    g_pair_snapshot.state = PICO07_PAIR_SUCCESS;
    g_pair_snapshot.last_type = type;
    g_pair_snapshot.last_was_new_pair = was_new_pair;
    snprintf(g_pair_snapshot.message, sizeof(g_pair_snapshot.message), "%s",
             message != NULL ? message : "DEVICE SAVED");
    critical_section_exit(&g_pair_lock);
}

void pico07_pairing_init(void)
{
    if (g_pair_initialized) return;
    critical_section_init(&g_pair_lock);
    memset(&g_pair_snapshot, 0, sizeof(g_pair_snapshot));
    g_pair_snapshot.revision = 1u;
    g_pair_snapshot.state = PICO07_PAIR_IDLE;
    memset(&g_pair_command, 0, sizeof(g_pair_command));
    g_pair_command_pending = false;
    g_pair_initialized = true;
}

static bool pair_post_command(const pair_command_request_t *request)
{
    if (!g_pair_initialized || request == NULL) return false;
    critical_section_enter_blocking(&g_pair_lock);
    if (g_pair_command_pending) {
        critical_section_exit(&g_pair_lock);
        return false;
    }
    g_pair_command = *request;
    g_pair_command_pending = true;
    critical_section_exit(&g_pair_lock);
    return true;
}

bool pico07_pairing_request_scan(void)
{
    pair_command_request_t request = {.kind = PAIR_CMD_SCAN};
    return pair_post_command(&request);
}

bool pico07_pairing_request_connect_result(uint8_t result_index)
{
    if (!g_pair_initialized) return false;
    critical_section_enter_blocking(&g_pair_lock);
    const bool valid = result_index < g_pair_snapshot.result_count;
    critical_section_exit(&g_pair_lock);
    if (!valid) return false;
    pair_command_request_t request = {
        .kind = PAIR_CMD_CONNECT_RESULT,
        .result_index = result_index,
    };
    return pair_post_command(&request);
}

bool pico07_pairing_request_connect_saved(uint8_t addr_type, const uint8_t addr[6])
{
    if (addr == NULL) return false;
    pair_command_request_t request;
    memset(&request, 0, sizeof(request));
    request.kind = PAIR_CMD_CONNECT_SAVED;
    request.addr_type = addr_type;
    memcpy(request.addr, addr, 6);
    return pair_post_command(&request);
}

bool pico07_pairing_request_delete_saved(uint8_t addr_type, const uint8_t addr[6])
{
    if (addr == NULL) return false;
    pair_command_request_t request;
    memset(&request, 0, sizeof(request));
    request.kind = PAIR_CMD_DELETE_SAVED;
    request.addr_type = addr_type;
    memcpy(request.addr, addr, 6);
    return pair_post_command(&request);
}

bool pico07_pairing_request_cancel(void)
{
    pair_command_request_t request = {.kind = PAIR_CMD_CANCEL};
    return pair_post_command(&request);
}

bool pico07_pairing_get_snapshot(pico07_pairing_snapshot_t *snapshot)
{
    if (!g_pair_initialized || snapshot == NULL) return false;
    critical_section_enter_blocking(&g_pair_lock);
    memcpy(snapshot, &g_pair_snapshot, sizeof(*snapshot));
    critical_section_exit(&g_pair_lock);
    return true;
}

const char *pico07_device_type_name(pico07_device_type_t type)
{
    switch (type) {
        case PICO07_TYPE_MOUSE: return "MOUSE";
        case PICO07_TYPE_KEYBOARD: return "KEYBOARD";
        case PICO07_TYPE_COMPOSITE: return "COMPOSITE";
        case PICO07_TYPE_UNSUPPORTED: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}

static pico07_device_type_t type_from_capabilities(uint32_t capabilities)
{
    const bool mouse = (capabilities & DEVICE_CAP_MOUSE) != 0;
    const bool keyboard = (capabilities & DEVICE_CAP_KEYBOARD) != 0;
    if (mouse && keyboard) return PICO07_TYPE_COMPOSITE;
    if (mouse) return PICO07_TYPE_MOUSE;
    if (keyboard) return PICO07_TYPE_KEYBOARD;
    return PICO07_TYPE_UNSUPPORTED;
}

static bool same_addr(uint8_t type_a, const uint8_t a[6],
                      uint8_t type_b, const uint8_t b[6])
{
    return type_a == type_b && memcmp(a, b, 6) == 0;
}

static void parse_advertised_name(const uint8_t *packet, char out[PICO07_DEVICE_NAME_MAX + 1u])
{
    out[0] = '\0';
    const uint8_t *data = gap_event_advertising_report_get_data(packet);
    const uint8_t len = gap_event_advertising_report_get_data_length(packet);
    uint8_t offset = 0;
    while (offset < len) {
        const uint8_t field_len = data[offset];
        if (field_len == 0) break;
        if ((uint16_t)offset + field_len >= len) break;
        const uint8_t type = data[offset + 1u];
        if (type == ADV_TYPE_COMPLETE_NAME || type == ADV_TYPE_SHORT_NAME) {
            uint8_t name_len = (uint8_t)(field_len - 1u);
            if (name_len > PICO07_DEVICE_NAME_MAX) name_len = PICO07_DEVICE_NAME_MAX;
            memcpy(out, &data[offset + 2u], name_len);
            out[name_len] = '\0';
            return;
        }
        offset = (uint8_t)(offset + field_len + 1u);
    }
}

bool pico07_pairing_consume_advertisement(const uint8_t *packet)
{
    if (!g_pair_initialized || packet == NULL) return false;

    critical_section_enter_blocking(&g_pair_lock);
    const bool scanning = g_pair_snapshot.state == PICO07_PAIR_SCANNING;
    critical_section_exit(&g_pair_lock);
    if (!scanning) return false;

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    const uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);

    critical_section_enter_blocking(&g_pair_lock);
    for (uint8_t i = 0; i < g_pair_snapshot.result_count; ++i) {
        if (same_addr(g_pair_snapshot.results[i].addr_type,
                      g_pair_snapshot.results[i].addr,
                      addr_type, addr)) {
            g_pair_snapshot.results[i].rssi = gap_event_advertising_report_get_rssi(packet);
            critical_section_exit(&g_pair_lock);
            return true;
        }
    }

    if (g_pair_snapshot.result_count < PICO07_MAX_SCAN_RESULTS) {
        pico07_scan_result_t *result =
            &g_pair_snapshot.results[g_pair_snapshot.result_count++];
        memset(result, 0, sizeof(*result));
        result->addr_type = addr_type;
        memcpy(result->addr, addr, 6);
        result->rssi = gap_event_advertising_report_get_rssi(packet);
        parse_advertised_name(packet, result->name);
        if (result->name[0] == '\0') {
            snprintf(result->name, sizeof(result->name), "HID %02X%02X",
                     addr[4], addr[5]);
        }
        ++g_pair_snapshot.revision;
    }
    critical_section_exit(&g_pair_lock);
    return true;
}

bool pico07_pairing_scan_timeout_owned(void)
{
    if (!g_pair_initialized) return false;
    critical_section_enter_blocking(&g_pair_lock);
    const bool owned = g_pair_snapshot.state == PICO07_PAIR_SCANNING;
    const uint8_t count = g_pair_snapshot.result_count;
    critical_section_exit(&g_pair_lock);
    if (!owned) return false;

    gap_stop_scan();
    app_state = W4_WORKING;
    pair_publish_state(count > 0 ? PICO07_PAIR_RESULTS : PICO07_PAIR_ERROR,
                       count > 0 ? "SELECT HID DEVICE" : "NO HID FOUND");
    return true;
}

bool pico07_pairing_connection_timeout_owned(void)
{
    if (!g_managed_connection) return false;
    g_managed_connection = false;
    g_hids_transport_ready = false;
    app_state = W4_WORKING;
    pair_publish_state(PICO07_PAIR_ERROR, "CONNECTION TIMEOUT");
    hog_start_connect();
    return true;
}

bool pico07_pairing_idle_without_saved_device(void)
{
    if (!g_pair_initialized) return false;
    pair_publish_state(PICO07_PAIR_IDLE, "NO ACTIVE MOUSE");
    return true;
}

bool pico07_pairing_connection_is_managed(void)
{
    return g_managed_connection;
}

void pico07_pairing_hids_transport_ready(void)
{
    if (!g_managed_connection) return;
    g_hids_transport_ready = true;
    pair_publish_state(PICO07_PAIR_CLASSIFYING, "CLASSIFYING HID");
}

bool pico07_pairing_handle_connection_error(void)
{
    if (!g_managed_connection) return false;
    g_managed_connection = false;
    g_hids_transport_ready = false;
    app_state = W4_WORKING;
    pair_publish_state(PICO07_PAIR_ERROR, "PAIRING FAILED");
    return true;
}

static void start_scan_core1(void)
{
    critical_section_enter_blocking(&g_pair_lock);
    g_pair_snapshot.result_count = 0;
    memset(g_pair_snapshot.results, 0, sizeof(g_pair_snapshot.results));
    critical_section_exit(&g_pair_lock);
    pair_publish_state(PICO07_PAIR_SCANNING, "SCANNING BLE HID");
    hog_start_scan();
}

static void start_target_core1(const le_device_addr_t *target, bool is_new_pair)
{
    if (target == NULL) return;
    remote_device = *target;
    g_managed_connection = true;
    g_managed_is_new_pair = is_new_pair;
    g_hids_transport_ready = false;
    pair_publish_state(PICO07_PAIR_CONNECTING,
                       is_new_pair ? "PAIRING DEVICE" : "CONNECTING DEVICE");
    hog_connect();
}

static void clear_preferred_if_matches(uint8_t addr_type, const uint8_t addr[6])
{
    if (btstack_tlv_singleton_impl == NULL) return;
    le_device_addr_t preferred;
    const int len = btstack_tlv_singleton_impl->get_tag(
        btstack_tlv_singleton_context, TLV_TAG_HOGD,
        (uint8_t *)&preferred, sizeof(preferred));
    if (len == sizeof(preferred) &&
        same_addr((uint8_t)preferred.addr_type, preferred.addr, addr_type, addr)) {
        btstack_tlv_singleton_impl->delete_tag(btstack_tlv_singleton_context,
                                               TLV_TAG_HOGD);
    }
}

static bool delete_saved_core1(uint8_t addr_type, const uint8_t addr[6])
{
    int16_t bond_index = -1;
    const bool remap_ok = remap_profile_delete_saved(addr_type, addr);
    const bool profile_ok = device_profile_delete_saved(addr_type, addr, &bond_index);
    if (!profile_ok || !remap_ok) return false;
    if (bond_index >= 0) le_device_db_remove(bond_index);
    clear_preferred_if_matches(addr_type, addr);
    return true;
}

bool pico07_pairing_handle_disconnect(void)
{
    g_hids_transport_ready = false;
    g_managed_connection = false;

    const after_disconnect_t action = g_after_disconnect;
    g_after_disconnect = AFTER_DISCONNECT_NONE;
    switch (action) {
        case AFTER_DISCONNECT_SCAN:
            start_scan_core1();
            return true;
        case AFTER_DISCONNECT_CONNECT:
            start_target_core1(&g_pending_target, g_managed_is_new_pair);
            return true;
        case AFTER_DISCONNECT_DELETE: {
            const bool ok = delete_saved_core1(g_delete_addr_type, g_delete_addr);
            pair_publish_state(ok ? PICO07_PAIR_SUCCESS : PICO07_PAIR_ERROR,
                               ok ? "DEVICE DELETED" : "DELETE FAILED");
            app_state = W4_WORKING;
            hog_start_connect();
            return true;
        }
        case AFTER_DISCONNECT_RESTORE_PRIMARY:
            app_state = W4_WORKING;
            hog_start_connect();
            return true;
        case AFTER_DISCONNECT_NONE:
        default:
            return false;
    }
}

static void schedule_scan_core1(void)
{
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        g_after_disconnect = AFTER_DISCONNECT_SCAN;
        gap_disconnect(connection_handle);
    } else {
        start_scan_core1();
    }
}

static void schedule_connect_core1(const le_device_addr_t *target, bool is_new_pair)
{
    g_pending_target = *target;
    g_managed_is_new_pair = is_new_pair;
    if (app_state == W4_HID_DEVICE_FOUND) {
        btstack_run_loop_remove_timer(&connection_timer);
        gap_stop_scan();
        app_state = W4_WORKING;
    }
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        g_after_disconnect = AFTER_DISCONNECT_CONNECT;
        gap_disconnect(connection_handle);
    } else {
        start_target_core1(&g_pending_target, is_new_pair);
    }
}

static void process_pair_command_core1(void)
{
    pair_command_request_t request;
    bool have = false;
    critical_section_enter_blocking(&g_pair_lock);
    if (g_pair_command_pending) {
        request = g_pair_command;
        g_pair_command_pending = false;
        have = true;
    }
    critical_section_exit(&g_pair_lock);
    if (!have) return;

    switch (request.kind) {
        case PAIR_CMD_SCAN:
            schedule_scan_core1();
            break;

        case PAIR_CMD_CONNECT_RESULT: {
            pico07_scan_result_t result;
            bool valid = false;
            critical_section_enter_blocking(&g_pair_lock);
            if (request.result_index < g_pair_snapshot.result_count) {
                result = g_pair_snapshot.results[request.result_index];
                valid = true;
            }
            critical_section_exit(&g_pair_lock);
            if (!valid) {
                pair_publish_state(PICO07_PAIR_ERROR, "INVALID SELECTION");
                break;
            }
            le_device_addr_t target;
            memcpy(target.addr, result.addr, 6);
            target.addr_type = (bd_addr_type_t)result.addr_type;
            schedule_connect_core1(&target, true);
            break;
        }

        case PAIR_CMD_CONNECT_SAVED: {
            le_device_addr_t target;
            memcpy(target.addr, request.addr, 6);
            target.addr_type = (bd_addr_type_t)request.addr_type;
            schedule_connect_core1(&target, false);
            break;
        }

        case PAIR_CMD_DELETE_SAVED: {
            device_profile_snapshot_t device;
            const bool current = device_profile_get_snapshot(&device) &&
                device.connected &&
                same_addr(device.identity_addr_type, device.identity_addr,
                          request.addr_type, request.addr);
            if (current && connection_handle != HCI_CON_HANDLE_INVALID) {
                g_delete_addr_type = request.addr_type;
                memcpy(g_delete_addr, request.addr, 6);
                g_after_disconnect = AFTER_DISCONNECT_DELETE;
                gap_disconnect(connection_handle);
            } else {
                const bool ok = delete_saved_core1(request.addr_type, request.addr);
                pair_publish_state(ok ? PICO07_PAIR_SUCCESS : PICO07_PAIR_ERROR,
                                   ok ? "DEVICE DELETED" : "DELETE FAILED");
            }
            break;
        }

        case PAIR_CMD_CANCEL:
            if (app_state == W4_HID_DEVICE_FOUND) {
                btstack_run_loop_remove_timer(&connection_timer);
                gap_stop_scan();
                app_state = W4_WORKING;
            } else if (app_state == W4_CONNECTED) {
                gap_connect_cancel();
                app_state = W4_WORKING;
            }
            g_managed_connection = false;
            pair_publish_state(PICO07_PAIR_IDLE, "CANCELLED");
            hog_start_connect();
            break;

        case PAIR_CMD_NONE:
        default:
            break;
    }
}

static void classify_managed_connection_core1(void)
{
    if (!g_managed_connection || !g_hids_transport_ready || !g_pico07_profile_attached) return;

    device_profile_snapshot_t device;
    if (!device_profile_get_snapshot(&device) || !device.connected) return;
    const pico07_device_type_t type = type_from_capabilities(device.capabilities);

    if (type == PICO07_TYPE_UNSUPPORTED) {
        // A newly paired unsupported HID (e.g. gamepad) is not retained as a
        // supported product peer. Remove the bond/profile immediately.
        if (g_managed_is_new_pair) {
            (void)delete_saved_core1(device.identity_addr_type, device.identity_addr);
        }
        pair_publish_state(PICO07_PAIR_ERROR, "UNSUPPORTED HID");
        g_after_disconnect = AFTER_DISCONNECT_RESTORE_PRIMARY;
        gap_disconnect(connection_handle);
        return;
    }

    if (type == PICO07_TYPE_MOUSE || type == PICO07_TYPE_COMPOSITE) {
        if (g_managed_is_new_pair && btstack_tlv_singleton_impl != NULL) {
            btstack_tlv_singleton_impl->store_tag(
                btstack_tlv_singleton_context, TLV_TAG_HOGD,
                (const uint8_t *)&remote_device, sizeof(remote_device));
        }
        pair_publish_result(type, g_managed_is_new_pair,
                            type == PICO07_TYPE_COMPOSITE ? "COMPOSITE SAVED" : "MOUSE SAVED");
        g_managed_connection = false;
        g_managed_is_new_pair = false;
        return;
    }

    // Keyboard pairing is persisted now, but simultaneous keyboard forwarding
    // belongs to PICO-08. For a *new* keyboard, restore the preferred mouse so
    // PICO-07 pairing validation does not strand the remapper on the keyboard.
    pair_publish_result(type, g_managed_is_new_pair,
                        g_managed_is_new_pair ? "KEYBOARD SAVED" : "KEYBOARD CONNECTED");
    if (g_managed_is_new_pair) {
        g_after_disconnect = AFTER_DISCONNECT_RESTORE_PRIMARY;
        gap_disconnect(connection_handle);
    } else {
        g_managed_connection = false;
    }
    g_managed_is_new_pair = false;
}

static void pico07_profile_poll(btstack_timer_source_t *timer)
{
    (void)timer;

    process_pair_command_core1();

    const bool ready = is_ble_app_state_ready();
    const uint16_t current_cid = hids_cid;

    if (!ready || current_cid == 0) {
        if (g_pico07_profile_attached) {
            logitech_hidpp_on_disconnect();
            device_profile_on_disconnect();
            g_pico07_profile_attached = false;
            g_pico07_attached_hids_cid = 0;
            printf("[PICO-07] profile detached\n");
        }
        remap_profile_core1_task();
    } else if (!g_pico07_profile_attached ||
               current_cid != g_pico07_attached_hids_cid) {
        if (g_pico07_profile_attached) {
            logitech_hidpp_on_disconnect();
            device_profile_on_disconnect();
        }

        const uint8_t *descriptor = get_ble_hid_report_descriptor_data();
        const uint16_t descriptor_len = get_ble_hid_report_descriptor_len();
        if (descriptor != NULL && descriptor_len > 0) {
            device_profile_on_hids_ready(connection_handle,
                                         remote_device.addr,
                                         (uint8_t)remote_device.addr_type,
                                         descriptor,
                                         descriptor_len);
            g_pico07_profile_attached = true;
            g_pico07_attached_hids_cid = current_cid;
            logitech_hidpp_on_connect();
            printf("[PICO-07] profile attached hids_cid=%u\n", current_cid);
        }
    }

    if (g_pico07_profile_attached) {
        remap_profile_core1_task();
        logitech_hidpp_core1_task();
    }

    classify_managed_connection_core1();

    btstack_run_loop_set_timer(&g_pico07_profile_timer, PICO07_PROFILE_POLL_MS);
    btstack_run_loop_add_timer(&g_pico07_profile_timer);
}

void ble_host_main(void)
{
    (void)picow_bt_example_init();
    picow_bt_example_main();

    if (!g_pair_initialized) pico07_pairing_init();
    g_pico07_attached_hids_cid = 0;
    g_pico07_profile_attached = false;
    g_managed_connection = false;
    g_hids_transport_ready = false;
    g_after_disconnect = AFTER_DISCONNECT_NONE;

    btstack_run_loop_set_timer_handler(&g_pico07_profile_timer, pico07_profile_poll);
    btstack_run_loop_set_timer(&g_pico07_profile_timer, PICO07_PROFILE_POLL_MS);
    btstack_run_loop_add_timer(&g_pico07_profile_timer);

    btstack_run_loop_execute();
}

uint16_t poc_hids_current_cid(void)
{
    return hids_cid;
}

uint8_t poc_hids_send_hidpp_long(const uint8_t *payload, uint8_t payload_len)
{
    if (hids_cid == 0 || payload == NULL || payload_len == 0) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    return hids_client_send_write_report(hids_cid,
                                         0x11,
                                         HID_REPORT_TYPE_OUTPUT,
                                         payload,
                                         payload_len);
}
