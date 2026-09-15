#include "ClassicHidHost.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "Common.h"

#define CLASSIC_HID_INQUIRY_DURATION_1280MS 5
#define CLASSIC_HID_DESCRIPTOR_STORAGE_SIZE 512
#define CLASSIC_HID_COMMAND_POLL_MS 40
#define CLASSIC_HID_COD_MAJOR_MASK 0x1F00u
#define CLASSIC_HID_COD_MAJOR_PERIPHERAL 0x0500u
#define CLASSIC_HID_RSSI_UNKNOWN INT8_MIN

static const char s_legacy_pin[] = "0000";

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
    bt_host_device_t info;
} discovered_device_t;

typedef enum {
    CLASSIC_COMMAND_NONE = 0,
    CLASSIC_COMMAND_START_DISCOVERY,
    CLASSIC_COMMAND_CONNECT_SELECTED,
    CLASSIC_COMMAND_PAIRING_ACCEPT,
    CLASSIC_COMMAND_PAIRING_REJECT,
    CLASSIC_COMMAND_SUBMIT_PASSKEY,
} classic_command_t;

static volatile bt_host_state_t s_state = BT_HOST_STATE_BOOTING;
static discovered_device_t s_devices[BT_HOST_MAX_DEVICES];
static volatile size_t s_device_count;
static volatile int s_selected_index = -1;

static bd_addr_t s_target_addr;
static volatile uint16_t s_hid_host_cid;
static volatile bool s_descriptor_available;
static char s_device_name[BT_HOST_DEVICE_NAME_MAX];
static uint8_t s_hid_descriptor_storage[CLASSIC_HID_DESCRIPTOR_STORAGE_SIZE];

static bt_host_pairing_info_t s_pairing_info;
static bd_addr_t s_pairing_addr;
static volatile bool s_pairing_addr_valid;

// UI/API calls may originate on Core 0. They only publish a tiny command here;
// the BTstack operation itself is always executed by Core 1 from its run loop.
static volatile classic_command_t s_pending_command = CLASSIC_COMMAND_NONE;
static volatile uint32_t s_pending_command_arg;
static btstack_timer_source_t s_command_timer;

static btstack_packet_callback_registration_t s_hci_event_callback_registration;

extern volatile bool g_usb_reinit_request;

static void classic_hid_start_inquiry_on_core1(void);
static void classic_hid_request_next_remote_name(void);
static void classic_hid_finish_discovery(void);
static void classic_hid_connect_selected_on_core1(void);

static bool classic_hid_contains_case_insensitive(const char *text, const char *token)
{
    if (text == NULL || token == NULL || token[0] == '\0') {
        return false;
    }

    for (const char *start = text; *start != '\0'; ++start) {
        const char *a = start;
        const char *b = token;
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bt_host_device_kind_t classic_hid_kind_from_class_of_device(uint32_t class_of_device)
{
    if ((class_of_device & CLASSIC_HID_COD_MAJOR_MASK) != CLASSIC_HID_COD_MAJOR_PERIPHERAL) {
        return BT_HOST_DEVICE_KIND_UNKNOWN;
    }

    // Peripheral minor class bits 6..7 identify keyboard/pointing capabilities.
    switch ((class_of_device >> 6) & 0x03u) {
        case 1: return BT_HOST_DEVICE_KIND_KEYBOARD;
        case 2: return BT_HOST_DEVICE_KIND_MOUSE;
        case 3: return BT_HOST_DEVICE_KIND_KEYBOARD_MOUSE;
        default: return BT_HOST_DEVICE_KIND_OTHER_PERIPHERAL;
    }
}

static bt_host_device_kind_t classic_hid_kind_from_name(const char *name)
{
    const bool keyboard = classic_hid_contains_case_insensitive(name, "keyboard") ||
                          classic_hid_contains_case_insensitive(name, "keypad");
    const bool mouse = classic_hid_contains_case_insensitive(name, "mouse") ||
                       classic_hid_contains_case_insensitive(name, "trackball") ||
                       classic_hid_contains_case_insensitive(name, "pointer");

    if (keyboard && mouse) {
        return BT_HOST_DEVICE_KIND_KEYBOARD_MOUSE;
    }
    if (keyboard) {
        return BT_HOST_DEVICE_KIND_KEYBOARD;
    }
    if (mouse) {
        return BT_HOST_DEVICE_KIND_MOUSE;
    }
    return BT_HOST_DEVICE_KIND_UNKNOWN;
}

static void classic_hid_refresh_device_classification(discovered_device_t *device)
{
    bt_host_device_kind_t kind = classic_hid_kind_from_class_of_device(device->info.class_of_device);
    const bool cod_peripheral =
        (device->info.class_of_device & CLASSIC_HID_COD_MAJOR_MASK) == CLASSIC_HID_COD_MAJOR_PERIPHERAL;

    if (kind == BT_HOST_DEVICE_KIND_UNKNOWN) {
        kind = classic_hid_kind_from_name(device->info.name);
    }

    const bool name_suggests_hid =
        kind == BT_HOST_DEVICE_KIND_KEYBOARD ||
        kind == BT_HOST_DEVICE_KIND_MOUSE ||
        kind == BT_HOST_DEVICE_KIND_KEYBOARD_MOUSE ||
        classic_hid_contains_case_insensitive(device->info.name, "hid");

    device->info.hid_candidate = cod_peripheral || name_suggests_hid;
    if (device->info.hid_candidate && kind == BT_HOST_DEVICE_KIND_UNKNOWN) {
        kind = BT_HOST_DEVICE_KIND_OTHER_PERIPHERAL;
    }
    device->info.kind = kind;
}

static void classic_hid_copy_name(discovered_device_t *device, const char *name)
{
    if (device == NULL || name == NULL) {
        return;
    }

    (void)snprintf(device->info.name, sizeof(device->info.name), "%s", name);
    classic_hid_refresh_device_classification(device);
}

static int classic_hid_device_index_for_address(const bd_addr_t address)
{
    const size_t count = s_device_count;
    for (size_t i = 0; i < count; ++i) {
        if (bd_addr_cmp(address, s_devices[i].address) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void classic_hid_reset_pairing(void)
{
    memset(&s_pairing_info, 0, sizeof(s_pairing_info));
    memset(s_pairing_addr, 0, sizeof(s_pairing_addr));
    s_pairing_addr_valid = false;
}

static void classic_hid_clear_connection_state(void)
{
    s_hid_host_cid = 0;
    s_descriptor_available = false;
    classic_hid_reset_pairing();
}

static bool classic_hid_queue_command(classic_command_t command, uint32_t arg)
{
    if (command == CLASSIC_COMMAND_NONE || s_pending_command != CLASSIC_COMMAND_NONE) {
        return false;
    }

    // Both are volatile. Publish the argument before the command so Core 1 never
    // observes a new command with the previous argument.
    s_pending_command_arg = arg;
    s_pending_command = command;
    return true;
}

static void classic_hid_select_first_candidate(void)
{
    s_selected_index = -1;
    const size_t count = s_device_count;
    for (size_t i = 0; i < count; ++i) {
        if (s_devices[i].info.hid_candidate) {
            s_selected_index = (int)i;
            return;
        }
    }
}

static void classic_hid_finish_discovery(void)
{
    classic_hid_select_first_candidate();
    s_state = BT_HOST_STATE_DEVICE_SELECTION;

    printf("[G03][ClassicHID] discovery complete: %u device(s), selected=%d\n",
           (unsigned int)s_device_count,
           (int)s_selected_index);
}

static void classic_hid_start_inquiry_on_core1(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_selected_index = -1;
    s_device_name[0] = '\0';
    classic_hid_clear_connection_state();
    s_state = BT_HOST_STATE_DISCOVERING;

    printf("[G03][ClassicHID] starting generic Bluetooth Classic discovery\n");

    const int status = gap_inquiry_start(CLASSIC_HID_INQUIRY_DURATION_1280MS);
    if (status != ERROR_CODE_SUCCESS) {
        s_state = BT_HOST_STATE_ERROR;
        printf("[G03][ClassicHID] gap_inquiry_start failed: 0x%02x\n", status);
    }
}

static void classic_hid_request_next_remote_name(void)
{
    s_state = BT_HOST_STATE_RESOLVING_NAMES;

    const size_t count = s_device_count;
    for (size_t i = 0; i < count; ++i) {
        if (s_devices[i].name_state != DEVICE_NAME_UNKNOWN) {
            continue;
        }

        s_devices[i].name_state = DEVICE_NAME_REQUESTED;
        const int status = gap_remote_name_request(
            s_devices[i].address,
            s_devices[i].page_scan_repetition_mode,
            s_devices[i].clock_offset | 0x8000);

        if (status == ERROR_CODE_SUCCESS) {
            return;
        }

        s_devices[i].name_state = DEVICE_NAME_RESOLVED;
        s_devices[i].info.name_resolved = true;
    }

    classic_hid_finish_discovery();
}

static void classic_hid_handle_inquiry_result(uint8_t *packet)
{
    if (s_state != BT_HOST_STATE_DISCOVERING) {
        return;
    }

    bd_addr_t address;
    gap_event_inquiry_result_get_bd_addr(packet, address);

    const int existing_index = classic_hid_device_index_for_address(address);
    if (existing_index >= 0) {
        if (gap_event_inquiry_result_get_rssi_available(packet)) {
            s_devices[existing_index].info.rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
        }
        return;
    }

    if (s_device_count >= BT_HOST_MAX_DEVICES) {
        return;
    }

    const size_t index = s_device_count;
    discovered_device_t *device = &s_devices[index];
    memset(device, 0, sizeof(*device));

    memcpy(device->address, address, sizeof(bd_addr_t));
    device->page_scan_repetition_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    device->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    device->info.class_of_device = gap_event_inquiry_result_get_class_of_device(packet);
    device->info.rssi = gap_event_inquiry_result_get_rssi_available(packet)
                            ? (int8_t)gap_event_inquiry_result_get_rssi(packet)
                            : CLASSIC_HID_RSSI_UNKNOWN;
    (void)snprintf(device->info.address,
                   sizeof(device->info.address),
                   "%s",
                   bd_addr_to_str(address));

    if (gap_event_inquiry_result_get_name_available(packet)) {
        char name[249];
        uint8_t name_len = gap_event_inquiry_result_get_name_len(packet);
        if (name_len >= sizeof(name)) {
            name_len = sizeof(name) - 1;
        }
        memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
        name[name_len] = '\0';
        classic_hid_copy_name(device, name);
        device->name_state = DEVICE_NAME_RESOLVED;
        device->info.name_resolved = true;
    } else {
        device->name_state = DEVICE_NAME_UNKNOWN;
        device->info.name_resolved = false;
        classic_hid_refresh_device_classification(device);
    }

    // Publish the new element only after every field has been initialized.
    s_device_count = index + 1;
}

static void classic_hid_handle_remote_name_complete(uint8_t *packet)
{
    if (s_state != BT_HOST_STATE_RESOLVING_NAMES) {
        return;
    }

    bd_addr_t address;
    reverse_bd_addr(&packet[3], address);
    const int index = classic_hid_device_index_for_address(address);
    if (index < 0) {
        classic_hid_request_next_remote_name();
        return;
    }

    discovered_device_t *device = &s_devices[index];
    device->name_state = DEVICE_NAME_RESOLVED;
    device->info.name_resolved = true;

    if (packet[2] == ERROR_CODE_SUCCESS) {
        const char *name = (const char *)&packet[9];
        classic_hid_copy_name(device, name);
    } else {
        classic_hid_refresh_device_classification(device);
    }

    classic_hid_request_next_remote_name();
}

static void classic_hid_connect_selected_on_core1(void)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION) {
        return;
    }

    const int selected = s_selected_index;
    if (selected < 0 || (size_t)selected >= s_device_count ||
        !s_devices[selected].info.hid_candidate) {
        s_state = BT_HOST_STATE_ERROR;
        return;
    }

    discovered_device_t *device = &s_devices[selected];
    memcpy(s_target_addr, device->address, sizeof(bd_addr_t));
    if (device->info.name[0] != '\0') {
        (void)snprintf(s_device_name, sizeof(s_device_name), "%s", device->info.name);
    } else {
        (void)snprintf(s_device_name, sizeof(s_device_name), "%s", device->info.address);
    }

    classic_hid_clear_connection_state();
    s_state = BT_HOST_STATE_CONNECTING;
    gap_inquiry_stop();

    printf("[G03][ClassicHID] connecting to selected HID candidate %s (%s)\n",
           device->info.address,
           s_device_name);

    uint16_t new_cid = 0;
    const uint8_t status = hid_host_connect(s_target_addr, HID_PROTOCOL_MODE_REPORT, &new_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[G03][ClassicHID] hid_host_connect failed: 0x%02x\n", status);
        s_state = BT_HOST_STATE_ERROR;
        return;
    }

    s_hid_host_cid = new_cid;
}

static void classic_hid_prepare_pairing(const bd_addr_t address,
                                        bt_host_pairing_method_t method,
                                        uint32_t numeric_value,
                                        bool action_required)
{
    memcpy(s_pairing_addr, address, sizeof(bd_addr_t));
    s_pairing_addr_valid = true;
    memset(&s_pairing_info, 0, sizeof(s_pairing_info));
    s_pairing_info.method = method;
    s_pairing_info.numeric_value = numeric_value;
    s_pairing_info.action_required = action_required;
    if (method == BT_HOST_PAIRING_LEGACY_PIN) {
        (void)snprintf(s_pairing_info.legacy_pin,
                       sizeof(s_pairing_info.legacy_pin),
                       "%s",
                       s_legacy_pin);
    }
    s_state = BT_HOST_STATE_PAIRING;
}

static void classic_hid_enqueue_usb_report(const uint8_t *report, uint16_t report_len)
{
    // Classic HID DATA input packets include the HIDP DATA transaction byte
    // (0xA1). USB receives only the report payload, including Report ID.
    if (!s_descriptor_available || report_len < 2 || report[0] != 0xA1) {
        return;
    }

    static ST_HID_RPT usb_report;
    usb_report.report_id = report[1];
    usb_report.report_len = report_len - 1;
    if (usb_report.report_len > CMN_HID_RPT_DATA_SIZE) {
        usb_report.report_len = CMN_HID_RPT_DATA_SIZE;
    }

    memcpy(usb_report.report, &report[1], usb_report.report_len);
    if (!CMN_Enqueue(CMN_QUE_KIND_HID_RPT, &usb_report)) {
        printf("[G03][ClassicHID] HID queue full; report dropped\n");
    }
}

static void classic_hid_handle_hid_report(uint8_t *packet)
{
    classic_hid_enqueue_usb_report(
        hid_subevent_report_get_report(packet),
        hid_subevent_report_get_report_len(packet));
}

static void classic_hid_execute_pending_command(void)
{
    const classic_command_t command = s_pending_command;
    if (command == CLASSIC_COMMAND_NONE) {
        return;
    }

    const uint32_t arg = s_pending_command_arg;
    s_pending_command = CLASSIC_COMMAND_NONE;

    switch (command) {
        case CLASSIC_COMMAND_START_DISCOVERY:
            if (s_state == BT_HOST_STATE_DEVICE_SELECTION ||
                s_state == BT_HOST_STATE_ERROR) {
                classic_hid_start_inquiry_on_core1();
            }
            break;

        case CLASSIC_COMMAND_CONNECT_SELECTED:
            classic_hid_connect_selected_on_core1();
            break;

        case CLASSIC_COMMAND_PAIRING_ACCEPT:
            if (!s_pairing_addr_valid || !s_pairing_info.action_required) {
                break;
            }
            if (s_pairing_info.method == BT_HOST_PAIRING_LEGACY_PIN) {
                (void)gap_pin_code_response(s_pairing_addr, s_legacy_pin);
                s_pairing_info.action_required = false;
            } else if (s_pairing_info.method == BT_HOST_PAIRING_NUMERIC_CONFIRMATION) {
                (void)gap_ssp_confirmation_response(s_pairing_addr);
                s_pairing_info.action_required = false;
            }
            break;

        case CLASSIC_COMMAND_PAIRING_REJECT:
            if (!s_pairing_addr_valid || !s_pairing_info.action_required) {
                break;
            }
            if (s_pairing_info.method == BT_HOST_PAIRING_LEGACY_PIN) {
                (void)gap_pin_code_negative(s_pairing_addr);
            } else if (s_pairing_info.method == BT_HOST_PAIRING_NUMERIC_CONFIRMATION) {
                (void)gap_ssp_confirmation_negative(s_pairing_addr);
            } else if (s_pairing_info.method == BT_HOST_PAIRING_PASSKEY_INPUT) {
                (void)gap_ssp_passkey_negative(s_pairing_addr);
            }
            s_pairing_info.action_required = false;
            s_state = BT_HOST_STATE_ERROR;
            break;

        case CLASSIC_COMMAND_SUBMIT_PASSKEY:
            if (s_pairing_addr_valid &&
                s_pairing_info.method == BT_HOST_PAIRING_PASSKEY_INPUT &&
                s_pairing_info.action_required &&
                arg <= 999999u) {
                (void)gap_ssp_passkey_response(s_pairing_addr, arg);
                s_pairing_info.numeric_value = arg;
                s_pairing_info.action_required = false;
            }
            break;

        case CLASSIC_COMMAND_NONE:
        default:
            break;
    }
}

static void classic_hid_command_timer_handler(btstack_timer_source_t *timer)
{
    classic_hid_execute_pending_command();
    btstack_run_loop_set_timer(timer, CLASSIC_HID_COMMAND_POLL_MS);
    btstack_run_loop_add_timer(timer);
}

static void classic_hid_packet_handler(uint8_t packet_type,
                                       uint16_t channel,
                                       uint8_t *packet,
                                       uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    bd_addr_t event_addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                classic_hid_start_inquiry_on_core1();
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT:
            classic_hid_handle_inquiry_result(packet);
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (s_state == BT_HOST_STATE_DISCOVERING) {
                classic_hid_request_next_remote_name();
            }
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            classic_hid_handle_remote_name_complete(packet);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            classic_hid_prepare_pairing(
                event_addr,
                BT_HOST_PAIRING_LEGACY_PIN,
                0,
                true);
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            classic_hid_prepare_pairing(
                event_addr,
                BT_HOST_PAIRING_NUMERIC_CONFIRMATION,
                hci_event_user_confirmation_request_get_numeric_value(packet),
                true);
            break;

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            hci_event_user_passkey_notification_get_bd_addr(packet, event_addr);
            classic_hid_prepare_pairing(
                event_addr,
                BT_HOST_PAIRING_PASSKEY_DISPLAY,
                hci_event_user_passkey_notification_get_numeric_value(packet),
                false);
            break;

        case HCI_EVENT_USER_PASSKEY_REQUEST:
            hci_event_user_passkey_request_get_bd_addr(packet, event_addr);
            classic_hid_prepare_pairing(
                event_addr,
                BT_HOST_PAIRING_PASSKEY_INPUT,
                0,
                true);
            break;

        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            if (hci_event_simple_pairing_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                s_state = BT_HOST_STATE_ERROR;
            }
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION: {
                    const uint16_t incoming_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
                    const uint8_t status = hid_subevent_incoming_connection_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        break;
                    }

                    // G03 requires an explicit selected device. Only accept an
                    // incoming HID connection while that selection is being
                    // connected/paired; otherwise decline it.
                    if (s_state == BT_HOST_STATE_CONNECTING || s_state == BT_HOST_STATE_PAIRING) {
                        s_hid_host_cid = incoming_cid;
                        (void)hid_host_accept_connection(incoming_cid, HID_PROTOCOL_MODE_REPORT);
                    } else {
                        (void)hid_host_decline_connection(incoming_cid);
                    }
                    break;
                }

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    const uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        classic_hid_clear_connection_state();
                        s_state = BT_HOST_STATE_ERROR;
                        break;
                    }
                    s_hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    s_descriptor_available = false;
                    classic_hid_reset_pairing();
                    s_state = BT_HOST_STATE_CONNECTED;
                    break;
                }

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    const uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        s_state = BT_HOST_STATE_ERROR;
                        break;
                    }

                    s_descriptor_available = true;
                    s_state = BT_HOST_STATE_READY;
                    // Core 0 clears stale reports before reconnecting USB and
                    // serves this descriptor directly from BTstack storage.
                    g_usb_reinit_request = true;
                    break;
                }

                case HID_SUBEVENT_REPORT:
                    classic_hid_handle_hid_report(packet);
                    break;

                case HID_SUBEVENT_CONNECTION_CLOSED: {
                    const bool had_descriptor = s_descriptor_available;
                    classic_hid_clear_connection_state();
                    if (had_descriptor) {
                        g_usb_reinit_request = true;
                    }
                    // Persistent auto-reconnect is G04. G03 intentionally returns
                    // to a fresh discovery/selection flow after a disconnect.
                    classic_hid_start_inquiry_on_core1();
                    break;
                }

                default:
                    break;
            }
            break;

        default:
            break;
    }
}

static void classic_hid_init(void)
{
    l2cap_init();
    hid_host_init(s_hid_descriptor_storage, sizeof(s_hid_descriptor_storage));
    hid_host_register_packet_handler(classic_hid_packet_handler);

    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // The final product has an LCD plus confirmation buttons, so advertise the
    // matching SSP capability now instead of silently auto-accepting pairing.
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_set_local_name("Pico Remapper 00:00:00:00:00:00");
    gap_discoverable_control(1);

    s_hci_event_callback_registration.callback = &classic_hid_packet_handler;
    hci_add_event_handler(&s_hci_event_callback_registration);

    btstack_run_loop_set_timer_handler(&s_command_timer, classic_hid_command_timer_handler);
    btstack_run_loop_set_timer(&s_command_timer, CLASSIC_HID_COMMAND_POLL_MS);
    btstack_run_loop_add_timer(&s_command_timer);

    hci_power_control(HCI_POWER_ON);
}

void CLASSIC_HID_CoreMain(void)
{
    if (cyw43_arch_init() != PICO_OK) {
        s_state = BT_HOST_STATE_ERROR;
        panic("cyw43_arch_init failed");
    }

    classic_hid_init();
    btstack_run_loop_execute();
    cyw43_arch_deinit();
}

bool CLASSIC_HID_IsReady(void)
{
    return s_state == BT_HOST_STATE_READY &&
           s_descriptor_available &&
           s_hid_host_cid != 0;
}

const uint8_t *CLASSIC_HID_GetReportDescriptor(void)
{
    if (!CLASSIC_HID_IsReady()) {
        return NULL;
    }
    return hid_descriptor_storage_get_descriptor_data((uint16_t)s_hid_host_cid);
}

uint16_t CLASSIC_HID_GetReportDescriptorLength(void)
{
    if (!CLASSIC_HID_IsReady()) {
        return 0;
    }
    return hid_descriptor_storage_get_descriptor_len((uint16_t)s_hid_host_cid);
}

bt_host_state_t CLASSIC_HID_GetState(void)
{
    return s_state;
}

const char *CLASSIC_HID_GetStateName(void)
{
    switch (s_state) {
        case BT_HOST_STATE_BOOTING: return "BOOTING";
        case BT_HOST_STATE_DISCOVERING: return "SCANNING";
        case BT_HOST_STATE_RESOLVING_NAMES: return "RESOLVING";
        case BT_HOST_STATE_DEVICE_SELECTION: return "SELECT DEVICE";
        case BT_HOST_STATE_CONNECTING: return "CONNECTING";
        case BT_HOST_STATE_PAIRING: return "PAIRING";
        case BT_HOST_STATE_CONNECTED: return "CONNECTED";
        case BT_HOST_STATE_READY: return "READY";
        case BT_HOST_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *CLASSIC_HID_GetDeviceName(void)
{
    return s_device_name;
}

size_t CLASSIC_HID_GetDiscoveredDeviceCount(void)
{
    return s_device_count;
}

bool CLASSIC_HID_GetDiscoveredDevice(size_t index, bt_host_device_t *out_device)
{
    if (out_device == NULL || index >= s_device_count) {
        return false;
    }

    // The element is fully initialized before s_device_count is increased.
    // Name resolution can update its name while scanning; a GUI refresh simply
    // receives the newest complete snapshot on its next frame.
    *out_device = s_devices[index].info;
    return true;
}

int CLASSIC_HID_GetSelectedDeviceIndex(void)
{
    return s_selected_index;
}

bool CLASSIC_HID_SelectDevice(size_t index)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION ||
        index >= s_device_count ||
        !s_devices[index].info.hid_candidate) {
        return false;
    }

    s_selected_index = (int)index;
    return true;
}

bool CLASSIC_HID_SelectNextDevice(void)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION || s_device_count == 0) {
        return false;
    }

    const size_t count = s_device_count;
    const int start = s_selected_index;
    for (size_t step = 1; step <= count; ++step) {
        const size_t index = (size_t)((start < 0 ? 0 : start) + (int)step) % count;
        if (s_devices[index].info.hid_candidate) {
            s_selected_index = (int)index;
            return true;
        }
    }
    return false;
}

bool CLASSIC_HID_SelectPreviousDevice(void)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION || s_device_count == 0) {
        return false;
    }

    const int count = (int)s_device_count;
    int index = s_selected_index >= 0 ? s_selected_index : 0;
    for (int step = 1; step <= count; ++step) {
        int candidate = index - step;
        while (candidate < 0) {
            candidate += count;
        }
        if (s_devices[candidate].info.hid_candidate) {
            s_selected_index = candidate;
            return true;
        }
    }
    return false;
}

bool CLASSIC_HID_ConfirmSelectedDevice(void)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION ||
        s_selected_index < 0 ||
        (size_t)s_selected_index >= s_device_count ||
        !s_devices[s_selected_index].info.hid_candidate) {
        return false;
    }
    return classic_hid_queue_command(CLASSIC_COMMAND_CONNECT_SELECTED, 0);
}

bool CLASSIC_HID_StartDiscovery(void)
{
    if (s_state != BT_HOST_STATE_DEVICE_SELECTION && s_state != BT_HOST_STATE_ERROR) {
        return false;
    }
    return classic_hid_queue_command(CLASSIC_COMMAND_START_DISCOVERY, 0);
}

bt_host_pairing_info_t CLASSIC_HID_GetPairingInfo(void)
{
    return s_pairing_info;
}

bool CLASSIC_HID_ConfirmPairing(bool accept)
{
    if (s_state != BT_HOST_STATE_PAIRING ||
        !s_pairing_info.action_required ||
        (s_pairing_info.method != BT_HOST_PAIRING_LEGACY_PIN &&
         s_pairing_info.method != BT_HOST_PAIRING_NUMERIC_CONFIRMATION &&
         s_pairing_info.method != BT_HOST_PAIRING_PASSKEY_INPUT)) {
        return false;
    }

    return classic_hid_queue_command(
        accept ? CLASSIC_COMMAND_PAIRING_ACCEPT : CLASSIC_COMMAND_PAIRING_REJECT,
        0);
}

bool CLASSIC_HID_SubmitPasskey(uint32_t passkey)
{
    if (s_state != BT_HOST_STATE_PAIRING ||
        s_pairing_info.method != BT_HOST_PAIRING_PASSKEY_INPUT ||
        !s_pairing_info.action_required ||
        passkey > 999999u) {
        return false;
    }

    return classic_hid_queue_command(CLASSIC_COMMAND_SUBMIT_PASSKEY, passkey);
}
