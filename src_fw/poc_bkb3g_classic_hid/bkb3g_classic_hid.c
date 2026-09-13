#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "Common.h"

#define TARGET_NAME "Bluetooth keyboard 3.0"
#define TARGET_NAME_ALIAS "BKB-3G"
#define INQUIRY_DURATION_1280MS 5
#define MAX_DISCOVERED_DEVICES 20
#define HID_DESCRIPTOR_STORAGE_SIZE 512

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
    APP_INQUIRY,
    APP_RESOLVING_NAMES,
    APP_CONNECTING,
    APP_CONNECTED,
} app_state_t;

static volatile app_state_t app_state = APP_WAITING_FOR_BTSTACK;

static discovered_device_t devices[MAX_DISCOVERED_DEVICES];
static int device_count;

static bd_addr_t target_addr;
static volatile uint16_t hid_host_cid;
static volatile bool hid_descriptor_available;
static uint8_t hid_descriptor_storage[HID_DESCRIPTOR_STORAGE_SIZE];

static btstack_packet_callback_registration_t hci_event_callback_registration;

extern volatile bool g_usb_reinit_request;

static void start_inquiry(void);
static void request_next_remote_name(void);
static void connect_target(const bd_addr_t address);

static int device_index_for_address(const bd_addr_t address) {
    for (int i = 0; i < device_count; i++) {
        if (bd_addr_cmp(address, devices[i].address) == 0) {
            return i;
        }
    }
    return -1;
}

static bool target_name_matches(const char *name) {
    return strcmp(name, TARGET_NAME) == 0 || strcmp(name, TARGET_NAME_ALIAS) == 0;
}

static void print_target_found(const bd_addr_t address, const char *source) {
    printf("\nTarget keyboard found via %s: %s\n",
           source,
           bd_addr_to_str(address));
}

static void start_inquiry(void) {
    device_count = 0;
    hid_descriptor_available = false;
    app_state = APP_INQUIRY;

    printf("\nScanning for Bluetooth Classic devices...\n");
    printf("Target names: '%s' or '%s'.\n", TARGET_NAME, TARGET_NAME_ALIAS);
    printf("Put the keyboard in pairing mode (FN+1, FN+2 or FN+3 until the white LED blinks).\n");

    int status = gap_inquiry_start(INQUIRY_DURATION_1280MS);
    if (status != ERROR_CODE_SUCCESS) {
        printf("gap_inquiry_start failed, status 0x%02x\n", status);
    }
}

static void connect_target(const bd_addr_t address) {
    memcpy(target_addr, address, sizeof(bd_addr_t));
    app_state = APP_CONNECTING;

    // If inquiry is still running, stop it before opening the HID connection.
    gap_inquiry_stop();

    printf("Opening Classic HID connection to %s...\n", bd_addr_to_str(target_addr));
    printf("If a 6-digit passkey is printed below, type it on the keyboard and press Enter.\n");

    uint16_t new_cid = 0;
    uint8_t status = hid_host_connect(target_addr, HID_PROTOCOL_MODE_REPORT, &new_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("hid_host_connect failed immediately, status 0x%02x\n", status);
        hid_host_cid = 0;
        start_inquiry();
        return;
    }

    hid_host_cid = new_cid;
}

static void request_next_remote_name(void) {
    app_state = APP_RESOLVING_NAMES;

    for (int i = 0; i < device_count; i++) {
        if (devices[i].name_state != DEVICE_NAME_UNKNOWN) {
            continue;
        }

        devices[i].name_state = DEVICE_NAME_REQUESTED;
        printf("Resolving name for %s...\n", bd_addr_to_str(devices[i].address));

        int status = gap_remote_name_request(
            devices[i].address,
            devices[i].page_scan_repetition_mode,
            devices[i].clock_offset | 0x8000);

        if (status == ERROR_CODE_SUCCESS) {
            return;
        }

        printf("Remote-name request failed immediately, status 0x%02x\n", status);
        devices[i].name_state = DEVICE_NAME_RESOLVED;
    }

    printf("Target not found in this inquiry cycle. Restarting scan.\n");
    start_inquiry();
}

static void handle_inquiry_result(uint8_t *packet) {
    if (app_state != APP_INQUIRY) {
        return;
    }

    bd_addr_t address;
    gap_event_inquiry_result_get_bd_addr(packet, address);

    if (device_index_for_address(address) >= 0) {
        return;
    }

    printf("Device: %s", bd_addr_to_str(address));
    printf(" COD=0x%06x", (unsigned int)gap_event_inquiry_result_get_class_of_device(packet));

    if (gap_event_inquiry_result_get_rssi_available(packet)) {
        printf(" RSSI=%d dBm", (int8_t)gap_event_inquiry_result_get_rssi(packet));
    }

    if (gap_event_inquiry_result_get_name_available(packet)) {
        char name[249];
        uint8_t name_len = gap_event_inquiry_result_get_name_len(packet);
        if (name_len >= sizeof(name)) {
            name_len = sizeof(name) - 1;
        }
        memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
        name[name_len] = '\0';

        printf(" name='%s'\n", name);

        if (target_name_matches(name)) {
            print_target_found(address, "EIR");
            connect_target(address);
            return;
        }
    } else {
        printf(" name=<not in EIR>\n");
    }

    if (device_count >= MAX_DISCOVERED_DEVICES) {
        return;
    }

    memcpy(devices[device_count].address, address, sizeof(bd_addr_t));
    devices[device_count].page_scan_repetition_mode =
        gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    devices[device_count].clock_offset =
        gap_event_inquiry_result_get_clock_offset(packet);
    devices[device_count].name_state =
        gap_event_inquiry_result_get_name_available(packet)
            ? DEVICE_NAME_RESOLVED
            : DEVICE_NAME_UNKNOWN;
    device_count++;
}

static void handle_remote_name_complete(uint8_t *packet) {
    if (app_state != APP_RESOLVING_NAMES) {
        return;
    }

    bd_addr_t address;
    reverse_bd_addr(&packet[3], address);

    int index = device_index_for_address(address);
    if (index < 0) {
        request_next_remote_name();
        return;
    }

    devices[index].name_state = DEVICE_NAME_RESOLVED;

    if (packet[2] == ERROR_CODE_SUCCESS) {
        const char *name = (const char *)&packet[9];
        printf("Remote name: %s -> '%s'\n", bd_addr_to_str(address), name);

        if (target_name_matches(name)) {
            print_target_found(address, "remote-name request");
            connect_target(address);
            return;
        }
    } else {
        printf("Remote-name request failed for %s, status 0x%02x\n",
               bd_addr_to_str(address), packet[2]);
    }

    request_next_remote_name();
}

static void enqueue_usb_hid_report(const uint8_t *report, uint16_t report_len) {
    if (!hid_descriptor_available) {
        return;
    }

    // Classic HID interrupt reports include the HID DATA header 0xA1.
    // TinyUSB needs the HID report itself, beginning with the Report ID.
    if (report_len < 2 || report[0] != 0xA1) {
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
        printf("USB HID queue full; dropping report ID 0x%02x.\n", usb_report.report_id);
    }
}

static void handle_hid_report(uint8_t *packet) {
    const uint8_t *report = hid_subevent_report_get_report(packet);
    uint16_t report_len = hid_subevent_report_get_report_len(packet);

    printf("\nHID REPORT (%u bytes):\n", report_len);
    printf_hexdump(report, report_len);

    if (!hid_descriptor_available) {
        return;
    }

    // Forward the complete HID report to Core0/TinyUSB first.
    enqueue_usb_hid_report(report, report_len);

    // Also parse it for UART diagnostics.
    if (report_len < 2 || report[0] != 0xA1) {
        return;
    }

    uint16_t cid = hid_host_cid;
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(
        &parser,
        hid_descriptor_storage_get_descriptor_data(cid),
        hid_descriptor_storage_get_descriptor_len(cid),
        HID_REPORT_TYPE_INPUT,
        &report[1],
        report_len - 1);

    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page;
        uint16_t usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);

        if (value == 0) {
            continue;
        }

        printf("  field: page=0x%04x usage=0x%04x value=%" PRId32 "\n",
               usage_page, usage, value);
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    bd_addr_t event_addr;
    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                printf("BTstack ready. Local address: %s\n", bd_addr_to_str(local_addr));
                start_inquiry();
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT:
            handle_inquiry_result(packet);
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (app_state == APP_INQUIRY) {
                request_next_remote_name();
            }
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            handle_remote_name_complete(packet);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            printf("Legacy PIN requested by %s. Replying with 0000.\n",
                   bd_addr_to_str(event_addr));
            printf("If the keyboard expects a PIN, type 0000 on it and press Enter.\n");
            gap_pin_code_response(event_addr, "0000");
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            uint32_t numeric_value = little_endian_read_32(packet, 8);
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            printf("SSP confirmation for %s: %06" PRIu32 " (auto-accept)\n",
                   bd_addr_to_str(event_addr), numeric_value);
            gap_ssp_confirmation_response(event_addr);
            break;
        }

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION: {
            uint32_t passkey = little_endian_read_32(packet, 8);
            printf("\nPAIRING PASSKEY: %06" PRIu32 "\n", passkey);
            printf("Type this number on the keyboard and press Enter.\n\n");
            break;
        }

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    hid_host_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
                    app_state = APP_CONNECTING;
                    gap_inquiry_stop();
                    printf("Incoming HID connection, cid=0x%04x. Accepting.\n", (uint16_t)hid_host_cid);
                    hid_host_accept_connection((uint16_t)hid_host_cid, HID_PROTOCOL_MODE_REPORT);
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("HID connection failed, status 0x%02x\n", status);
                        hid_host_cid = 0;
                        hid_descriptor_available = false;
                        start_inquiry();
                        break;
                    }

                    hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    app_state = APP_CONNECTED;
                    hid_descriptor_available = false;
                    printf("\nHID HOST CONNECTED, cid=0x%04x\n", (uint16_t)hid_host_cid);
                    break;
                }

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("HID descriptor unavailable, status 0x%02x\n", status);
                        break;
                    }

                    uint16_t cid = hid_host_cid;
                    uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(cid);
                    const uint8_t *descriptor = hid_descriptor_storage_get_descriptor_data(cid);

                    hid_descriptor_available = true;

                    printf("HID descriptor available (%u bytes).\n", descriptor_len);
                    printf_hexdump(descriptor, descriptor_len);
                    printf("\nPOC READY - USB will re-enumerate with the keyboard descriptor.\n");

                    // Core0 disconnects/reconnects TinyUSB so the computer asks for
                    // the exact report descriptor obtained from the BKB-3G.
                    g_usb_reinit_request = true;
                    break;
                }

                case HID_SUBEVENT_REPORT:
                    handle_hid_report(packet);
                    break;

                case HID_SUBEVENT_SET_PROTOCOL_RESPONSE: {
                    uint8_t status = hid_subevent_set_protocol_response_get_handshake_status(packet);
                    if (status == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        uint8_t mode = hid_subevent_set_protocol_response_get_protocol_mode(packet);
                        printf("HID protocol mode: %s\n",
                               mode == HID_PROTOCOL_MODE_BOOT ? "BOOT" : "REPORT");
                    } else {
                        printf("Set protocol failed, status 0x%02x\n", status);
                    }
                    break;
                }

                case HID_SUBEVENT_CONNECTION_CLOSED: {
                    bool had_usb_descriptor = hid_descriptor_available;
                    printf("HID connection closed. Restarting discovery.\n");
                    hid_host_cid = 0;
                    hid_descriptor_available = false;
                    if (had_usb_descriptor) {
                        // Revert USB to the built-in fallback descriptor while no
                        // Bluetooth HID device is connected.
                        g_usb_reinit_request = true;
                    }
                    start_inquiry();
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

void bkb3g_classic_hid_init(void) {
    l2cap_init();

#ifdef ENABLE_BLE
    // The Pico SDK HID Host example links BLE too; SM supports cross-transport key derivation.
    sm_init();
#endif

    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);

    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_set_local_name("Remapper BKB3G POC 00:00:00:00:00:00");

    // Also accept reconnects initiated by a previously paired keyboard.
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
}

// Compatibility hooks used by the existing dynamic TinyUSB descriptor code.
// Despite the historical BLE names, these now expose the Classic HID state.
bool is_ble_app_state_ready(void) {
    return app_state == APP_CONNECTED && hid_descriptor_available && hid_host_cid != 0;
}

const uint8_t *get_ble_hid_report_descriptor_data(void) {
    if (!is_ble_app_state_ready()) {
        return NULL;
    }
    return hid_descriptor_storage_get_descriptor_data((uint16_t)hid_host_cid);
}

uint16_t get_ble_hid_report_descriptor_len(void) {
    if (!is_ble_app_state_ready()) {
        return 0;
    }
    return hid_descriptor_storage_get_descriptor_len((uint16_t)hid_host_cid);
}
