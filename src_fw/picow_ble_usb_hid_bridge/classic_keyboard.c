#include "classic_keyboard.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "keyboard_hid_queue.h"
#include "pico/critical_section.h"
#include "usb_descriptors.h"

#define TARGET_NAME "Bluetooth keyboard 3.0"
#define TARGET_NAME_ALIAS "BKB-3G"
#define INQUIRY_DURATION_1280MS 5u
#define MAX_DISCOVERED_DEVICES 20u
#define HID_DESCRIPTOR_STORAGE_SIZE 512u
#define COMMAND_POLL_MS 50u
#define RECONNECT_DELAY_TICKS 100u
#define STARTUP_RECONNECT_DELAY_TICKS 20u
#define TLV_TAG_KB3G ((((uint32_t)'K') << 24) | (((uint32_t)'B') << 16) | (((uint32_t)'3') << 8) | 'G')

typedef enum {
    DEVICE_NAME_UNKNOWN = 0,
    DEVICE_NAME_REQUESTED,
    DEVICE_NAME_RESOLVED,
} device_name_state_t;

typedef struct {
    bd_addr_t address;
    uint8_t page_scan_repetition_mode;
    uint16_t clock_offset;
    device_name_state_t name_state;
} discovered_device_t;

typedef enum {
    APP_WAITING_FOR_BTSTACK = 0,
    APP_IDLE,
    APP_INQUIRY,
    APP_RESOLVING_NAMES,
    APP_CONNECTING,
    APP_CONNECTED,
} app_state_t;

static app_state_t g_app_state = APP_WAITING_FOR_BTSTACK;
static discovered_device_t g_devices[MAX_DISCOVERED_DEVICES];
static uint8_t g_device_count;
static bd_addr_t g_target_addr;
static uint16_t g_hid_host_cid;
static bool g_hid_descriptor_available;
static uint8_t g_hid_descriptor_storage[HID_DESCRIPTOR_STORAGE_SIZE];
static bool g_user_pairing;
static bool g_cancel_after_connect;
static uint16_t g_reconnect_delay_ticks;

static btstack_packet_callback_registration_t g_hci_event_callback_registration;
static btstack_timer_source_t g_command_timer;
static const btstack_tlv_t *g_tlv_impl;
static void *g_tlv_context;

static critical_section_t g_shared_lock;
static bool g_shared_initialized;
static classic_keyboard_snapshot_t g_snapshot;
static bool g_pair_request_pending;
static bool g_cancel_request_pending;

static void start_inquiry(void);
static void request_next_remote_name(void);
static void connect_target(const bd_addr_t address, bool user_pairing);

static void publish_snapshot(classic_keyboard_state_t state,
                             bool connected,
                             const char *message)
{
    critical_section_enter_blocking(&g_shared_lock);
    ++g_snapshot.revision;
    g_snapshot.state = state;
    g_snapshot.connected = connected;
    if (message != NULL) {
        snprintf(g_snapshot.message, sizeof(g_snapshot.message), "%s", message);
    } else {
        g_snapshot.message[0] = '\0';
    }
    critical_section_exit(&g_shared_lock);
}

static void publish_saved_peer(bool saved)
{
    critical_section_enter_blocking(&g_shared_lock);
    ++g_snapshot.revision;
    g_snapshot.saved_peer = saved;
    critical_section_exit(&g_shared_lock);
}

void classic_keyboard_shared_init(void)
{
    if (g_shared_initialized) return;
    critical_section_init(&g_shared_lock);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.revision = 1u;
    g_snapshot.state = CLASSIC_KEYBOARD_IDLE;
    g_pair_request_pending = false;
    g_cancel_request_pending = false;
    g_shared_initialized = true;
}

bool classic_keyboard_request_pair(void)
{
    if (!g_shared_initialized) return false;
    critical_section_enter_blocking(&g_shared_lock);
    if (g_pair_request_pending) {
        critical_section_exit(&g_shared_lock);
        return false;
    }
    g_pair_request_pending = true;
    g_cancel_request_pending = false;
    critical_section_exit(&g_shared_lock);
    return true;
}

bool classic_keyboard_request_cancel(void)
{
    if (!g_shared_initialized) return false;
    critical_section_enter_blocking(&g_shared_lock);
    g_cancel_request_pending = true;
    g_pair_request_pending = false;
    critical_section_exit(&g_shared_lock);
    return true;
}

bool classic_keyboard_get_snapshot(classic_keyboard_snapshot_t *snapshot)
{
    if (!g_shared_initialized || snapshot == NULL) return false;
    critical_section_enter_blocking(&g_shared_lock);
    *snapshot = g_snapshot;
    critical_section_exit(&g_shared_lock);
    return true;
}

static bool consume_pair_request(void)
{
    bool pending;
    critical_section_enter_blocking(&g_shared_lock);
    pending = g_pair_request_pending;
    g_pair_request_pending = false;
    critical_section_exit(&g_shared_lock);
    return pending;
}

static bool consume_cancel_request(void)
{
    bool pending;
    critical_section_enter_blocking(&g_shared_lock);
    pending = g_cancel_request_pending;
    g_cancel_request_pending = false;
    critical_section_exit(&g_shared_lock);
    return pending;
}

static bool target_name_matches(const char *name)
{
    return name != NULL &&
           (strcmp(name, TARGET_NAME) == 0 || strcmp(name, TARGET_NAME_ALIAS) == 0);
}

static int device_index_for_address(const bd_addr_t address)
{
    for (uint8_t i = 0; i < g_device_count; ++i) {
        if (bd_addr_cmp(address, g_devices[i].address) == 0) return (int)i;
    }
    return -1;
}

static bool load_saved_peer(bd_addr_t address)
{
    btstack_tlv_get_instance(&g_tlv_impl, &g_tlv_context);
    if (g_tlv_impl == NULL) return false;
    const int len = g_tlv_impl->get_tag(g_tlv_context, TLV_TAG_KB3G,
                                        address, sizeof(bd_addr_t));
    return len == (int)sizeof(bd_addr_t);
}

static void save_peer(const bd_addr_t address)
{
    btstack_tlv_get_instance(&g_tlv_impl, &g_tlv_context);
    if (g_tlv_impl == NULL) return;
    const int status = g_tlv_impl->store_tag(g_tlv_context, TLV_TAG_KB3G,
                                             address, sizeof(bd_addr_t));
    if (status == 0) {
        publish_saved_peer(true);
        printf("[CLASSIC-KBD] saved peer %s\n", bd_addr_to_str(address));
    } else {
        printf("[CLASSIC-KBD] failed to save peer, status %d\n", status);
    }
}

static void enqueue_keyboard_report(const uint8_t payload[8])
{
    ST_HID_RPT report;
    memset(&report, 0, sizeof(report));
    report.report_id = REPORT_ID_KEYBOARD;
    report.report_len = 8u;
    memcpy(report.report, payload, 8u);

    if (!keyboard_hid_queue_enqueue(&report)) {
        // A stale keyboard queue is worse than dropping intermediate states.
        // Keep only the newest complete state so key-up is not lost forever.
        keyboard_hid_queue_clear();
        (void)keyboard_hid_queue_enqueue(&report);
    }
}

static void enqueue_neutral_keyboard_report(void)
{
    static const uint8_t neutral[8] = {0};
    enqueue_keyboard_report(neutral);
}

static bool add_key_usage(uint8_t keys[6], uint8_t *key_count, uint16_t usage)
{
    if (usage == 0u) return true;
    for (uint8_t i = 0; i < *key_count; ++i) {
        if (keys[i] == (uint8_t)usage) return true;
    }
    if (*key_count >= 6u) return false;
    keys[*key_count] = (uint8_t)usage;
    ++(*key_count);
    return true;
}

static void normalize_and_enqueue_report(const uint8_t *report, uint16_t report_len)
{
    if (!g_hid_descriptor_available || report == NULL || report_len < 2u || report[0] != 0xA1u) {
        return;
    }

    uint8_t boot_report[8] = {0};
    uint8_t key_count = 0u;
    bool saw_keyboard_page = false;
    bool rollover = false;

    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser,
                            hid_descriptor_storage_get_descriptor_data(g_hid_host_cid),
                            hid_descriptor_storage_get_descriptor_len(g_hid_host_cid),
                            HID_REPORT_TYPE_INPUT,
                            &report[1],
                            (uint16_t)(report_len - 1u));

    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page;
        uint16_t usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);
        if (usage_page != 0x0007u) continue;

        saw_keyboard_page = true;
        if (value == 0) continue;

        if (usage >= 0x00E0u && usage <= 0x00E7u) {
            boot_report[0] |= (uint8_t)(1u << (usage - 0x00E0u));
            continue;
        }

        if (usage > 0x00FFu || !add_key_usage(&boot_report[2], &key_count, usage)) {
            rollover = true;
        }
    }

    if (!saw_keyboard_page) return;
    if (rollover) {
        for (uint8_t i = 2u; i < 8u; ++i) boot_report[i] = 0x01u;
    }
    enqueue_keyboard_report(boot_report);
}

static void schedule_reconnect(uint16_t ticks)
{
    g_reconnect_delay_ticks = ticks;
    if (g_app_state != APP_WAITING_FOR_BTSTACK) g_app_state = APP_IDLE;
}

static void connect_target(const bd_addr_t address, bool user_pairing)
{
    memcpy(g_target_addr, address, sizeof(bd_addr_t));
    gap_inquiry_stop();
    g_app_state = APP_CONNECTING;
    g_user_pairing = user_pairing;
    g_cancel_after_connect = false;
    g_hid_descriptor_available = false;

    publish_snapshot(CLASSIC_KEYBOARD_CONNECTING, false,
                     user_pairing ? "PAIRING CLASSIC KBD" : "RECONNECTING KEYBOARD");
    printf("[CLASSIC-KBD] opening HID connection to %s\n", bd_addr_to_str(g_target_addr));

    uint16_t new_cid = 0u;
    const uint8_t status = hid_host_connect(g_target_addr, HID_PROTOCOL_MODE_REPORT, &new_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[CLASSIC-KBD] hid_host_connect failed 0x%02x\n", status);
        g_hid_host_cid = 0u;
        if (user_pairing) start_inquiry();
        else schedule_reconnect(RECONNECT_DELAY_TICKS);
        return;
    }
    g_hid_host_cid = new_cid;
}

static void start_inquiry(void)
{
    g_device_count = 0u;
    g_hid_descriptor_available = false;
    g_user_pairing = true;
    g_app_state = APP_INQUIRY;
    g_reconnect_delay_ticks = 0u;
    publish_snapshot(CLASSIC_KEYBOARD_SCANNING, false, "SEARCHING CLASSIC KBD");

    printf("[CLASSIC-KBD] inquiry for '%s' / '%s'\n", TARGET_NAME, TARGET_NAME_ALIAS);
    const uint8_t status = gap_inquiry_start(INQUIRY_DURATION_1280MS);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[CLASSIC-KBD] gap_inquiry_start failed 0x%02x\n", status);
        g_app_state = APP_IDLE;
        publish_snapshot(CLASSIC_KEYBOARD_ERROR, false, "CLASSIC SCAN FAILED");
    }
}

static void request_next_remote_name(void)
{
    g_app_state = APP_RESOLVING_NAMES;
    for (uint8_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].name_state != DEVICE_NAME_UNKNOWN) continue;

        g_devices[i].name_state = DEVICE_NAME_REQUESTED;
        const uint8_t status = gap_remote_name_request(
            g_devices[i].address,
            g_devices[i].page_scan_repetition_mode,
            (uint16_t)(g_devices[i].clock_offset | 0x8000u));
        if (status == ERROR_CODE_SUCCESS) return;
        g_devices[i].name_state = DEVICE_NAME_RESOLVED;
    }

    // The POC proved that the target may omit its name from EIR. Repeat Classic
    // inquiry/name resolution until the operator cancels or the target appears.
    start_inquiry();
}

static void handle_inquiry_result(uint8_t *packet)
{
    if (g_app_state != APP_INQUIRY) return;

    bd_addr_t address;
    gap_event_inquiry_result_get_bd_addr(packet, address);
    if (device_index_for_address(address) >= 0) return;

    if (gap_event_inquiry_result_get_name_available(packet)) {
        char name[249];
        uint8_t name_len = gap_event_inquiry_result_get_name_len(packet);
        if (name_len >= sizeof(name)) name_len = (uint8_t)(sizeof(name) - 1u);
        memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
        name[name_len] = '\0';
        if (target_name_matches(name)) {
            printf("[CLASSIC-KBD] target found via EIR: %s\n", bd_addr_to_str(address));
            connect_target(address, true);
            return;
        }
    }

    if (g_device_count >= MAX_DISCOVERED_DEVICES) return;
    memcpy(g_devices[g_device_count].address, address, sizeof(bd_addr_t));
    g_devices[g_device_count].page_scan_repetition_mode =
        gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    g_devices[g_device_count].clock_offset =
        gap_event_inquiry_result_get_clock_offset(packet);
    g_devices[g_device_count].name_state =
        gap_event_inquiry_result_get_name_available(packet)
            ? DEVICE_NAME_RESOLVED
            : DEVICE_NAME_UNKNOWN;
    ++g_device_count;
}

static void handle_remote_name_complete(uint8_t *packet)
{
    if (g_app_state != APP_RESOLVING_NAMES) return;

    bd_addr_t address;
    reverse_bd_addr(&packet[3], address);
    const int index = device_index_for_address(address);
    if (index < 0) {
        request_next_remote_name();
        return;
    }

    g_devices[index].name_state = DEVICE_NAME_RESOLVED;
    if (packet[2] == ERROR_CODE_SUCCESS) {
        const char *name = (const char *)&packet[9];
        if (target_name_matches(name)) {
            printf("[CLASSIC-KBD] target found via remote name: %s\n",
                   bd_addr_to_str(address));
            connect_target(address, true);
            return;
        }
    }
    request_next_remote_name();
}

static void handle_hid_meta(uint8_t *packet)
{
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            g_hid_host_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            g_app_state = APP_CONNECTING;
            gap_inquiry_stop();
            hid_host_accept_connection(g_hid_host_cid, HID_PROTOCOL_MODE_REPORT);
            break;

        case HID_SUBEVENT_CONNECTION_OPENED: {
            const uint8_t status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[CLASSIC-KBD] HID connection failed 0x%02x\n", status);
                g_hid_host_cid = 0u;
                g_hid_descriptor_available = false;
                if (g_user_pairing) start_inquiry();
                else schedule_reconnect(RECONNECT_DELAY_TICKS);
                break;
            }
            g_hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            g_app_state = APP_CONNECTED;
            g_hid_descriptor_available = false;
            printf("[CLASSIC-KBD] HID link open cid=0x%04x\n", g_hid_host_cid);
            if (g_cancel_after_connect) hid_host_disconnect(g_hid_host_cid);
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            const uint8_t status = hid_subevent_descriptor_available_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                publish_snapshot(CLASSIC_KEYBOARD_ERROR, false, "HID DESCRIPTOR FAILED");
                hid_host_disconnect(g_hid_host_cid);
                break;
            }

            g_hid_descriptor_available = true;
            g_app_state = APP_CONNECTED;
            g_user_pairing = false;
            g_reconnect_delay_ticks = 0u;
            save_peer(g_target_addr);
            publish_snapshot(CLASSIC_KEYBOARD_READY, true, "KEYBOARD CONNECTED");
            printf("[CLASSIC-KBD] ready, descriptor=%u bytes\n",
                   hid_descriptor_storage_get_descriptor_len(g_hid_host_cid));
            break;
        }

        case HID_SUBEVENT_REPORT:
            normalize_and_enqueue_report(hid_subevent_report_get_report(packet),
                                         hid_subevent_report_get_report_len(packet));
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            printf("[CLASSIC-KBD] HID connection closed\n");
            enqueue_neutral_keyboard_report();
            g_hid_host_cid = 0u;
            g_hid_descriptor_available = false;
            if (g_cancel_after_connect) {
                g_cancel_after_connect = false;
                g_user_pairing = false;
                g_app_state = APP_IDLE;
                publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD PAIR CANCEL");
            } else if (g_user_pairing) {
                start_inquiry();
            } else {
                publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD OFFLINE");
                schedule_reconnect(RECONNECT_DELAY_TICKS);
            }
            break;

        default:
            break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t event_addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            g_app_state = APP_IDLE;
            if (load_saved_peer(g_target_addr)) {
                publish_saved_peer(true);
                g_reconnect_delay_ticks = STARTUP_RECONNECT_DELAY_TICKS;
                publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD SAVED");
            } else {
                publish_saved_peer(false);
                publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD NOT PAIRED");
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT:
            handle_inquiry_result(packet);
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (g_app_state == APP_INQUIRY) request_next_remote_name();
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            handle_remote_name_complete(packet);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            printf("[CLASSIC-KBD] legacy PIN requested by %s; using 0000\n",
                   bd_addr_to_str(event_addr));
            gap_pin_code_response(event_addr, "0000");
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            printf("[CLASSIC-KBD] SSP confirm %s value=%06" PRIu32 "\n",
                   bd_addr_to_str(event_addr), little_endian_read_32(packet, 8));
            gap_ssp_confirmation_response(event_addr);
            break;

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION: {
            const uint32_t passkey = little_endian_read_32(packet, 8);
            char message[CLASSIC_KEYBOARD_MESSAGE_MAX + 1u];
            snprintf(message, sizeof(message), "TYPE %06" PRIu32 " ON KBD", passkey);
            publish_snapshot(CLASSIC_KEYBOARD_CONNECTING, false, message);
            printf("[CLASSIC-KBD] passkey %06" PRIu32 "\n", passkey);
            break;
        }

        case HCI_EVENT_HID_META:
            handle_hid_meta(packet);
            break;

        default:
            break;
    }
}

static void process_commands_and_reconnect(btstack_timer_source_t *timer)
{
    (void)timer;

    if (consume_cancel_request()) {
        g_user_pairing = false;
        g_reconnect_delay_ticks = 0u;
        if (g_app_state == APP_INQUIRY || g_app_state == APP_RESOLVING_NAMES) {
            gap_inquiry_stop();
            g_app_state = APP_IDLE;
            publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD PAIR CANCEL");
        } else if (g_app_state == APP_CONNECTING && g_hid_host_cid != 0u) {
            g_cancel_after_connect = true;
            hid_host_disconnect(g_hid_host_cid);
        } else if (g_app_state != APP_CONNECTED) {
            g_app_state = APP_IDLE;
            publish_snapshot(CLASSIC_KEYBOARD_IDLE, false, "KEYBOARD PAIR CANCEL");
        }
    }

    if (consume_pair_request()) {
        if (g_app_state == APP_CONNECTED && g_hid_descriptor_available) {
            publish_snapshot(CLASSIC_KEYBOARD_READY, true, "KEYBOARD CONNECTED");
        } else {
            if (g_app_state == APP_CONNECTING && g_hid_host_cid != 0u) {
                hid_host_disconnect(g_hid_host_cid);
            }
            start_inquiry();
        }
    } else if (g_app_state == APP_IDLE && g_reconnect_delay_ticks > 0u) {
        --g_reconnect_delay_ticks;
        if (g_reconnect_delay_ticks == 0u && load_saved_peer(g_target_addr)) {
            connect_target(g_target_addr, false);
        }
    }

    btstack_run_loop_set_timer(&g_command_timer, COMMAND_POLL_MS);
    btstack_run_loop_add_timer(&g_command_timer);
}

void classic_keyboard_core1_init(void)
{
    if (!g_shared_initialized) classic_keyboard_shared_init();

    hid_host_init(g_hid_descriptor_storage, sizeof(g_hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_set_local_name("Remapper Pico 2 W 00:00:00:00:00:00");
    gap_discoverable_control(1);

    g_hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&g_hci_event_callback_registration);

    g_app_state = APP_WAITING_FOR_BTSTACK;
    g_hid_host_cid = 0u;
    g_hid_descriptor_available = false;
    g_user_pairing = false;
    g_cancel_after_connect = false;
    g_reconnect_delay_ticks = 0u;

    btstack_run_loop_set_timer_handler(&g_command_timer, process_commands_and_reconnect);
    btstack_run_loop_set_timer(&g_command_timer, COMMAND_POLL_MS);
    btstack_run_loop_add_timer(&g_command_timer);
}
