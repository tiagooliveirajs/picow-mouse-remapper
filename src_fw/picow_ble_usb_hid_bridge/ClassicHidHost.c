#include "ClassicHidHost.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "Common.h"

// Gate 02 intentionally preserves the device-selection rule proven by the POC.
// Gate 03 will replace this fixed target with UI-driven discovery/selection.
#define CLASSIC_HID_TARGET_NAME "Bluetooth keyboard 3.0"
#define CLASSIC_HID_TARGET_ALIAS "BKB-3G"
#define CLASSIC_HID_INQUIRY_DURATION_1280MS 5
#define CLASSIC_HID_MAX_DISCOVERED_DEVICES 20
#define CLASSIC_HID_DESCRIPTOR_STORAGE_SIZE 512

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

static volatile classic_hid_state_t s_state = CLASSIC_HID_STATE_BOOTING;
static discovered_device_t s_devices[CLASSIC_HID_MAX_DISCOVERED_DEVICES];
static int s_device_count;
static bd_addr_t s_target_addr;
static volatile uint16_t s_hid_host_cid;
static volatile bool s_descriptor_available;
static volatile bool s_passkey_available;
static volatile uint32_t s_pairing_passkey;
static char s_device_name[64] = CLASSIC_HID_TARGET_ALIAS;
static uint8_t s_hid_descriptor_storage[CLASSIC_HID_DESCRIPTOR_STORAGE_SIZE];
static btstack_packet_callback_registration_t s_hci_event_callback_registration;

extern volatile bool g_usb_reinit_request;

static void classic_hid_start_inquiry(void);
static void classic_hid_request_next_remote_name(void);
static void classic_hid_connect_target(const bd_addr_t address, const char *name);

static int classic_hid_device_index_for_address(const bd_addr_t address)
{
    for (int i = 0; i < s_device_count; ++i) {
        if (bd_addr_cmp(address, s_devices[i].address) == 0) {
            return i;
        }
    }
    return -1;
}

static bool classic_hid_target_name_matches(const char *name)
{
    return strcmp(name, CLASSIC_HID_TARGET_NAME) == 0 ||
           strcmp(name, CLASSIC_HID_TARGET_ALIAS) == 0;
}

static void classic_hid_set_device_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    (void)snprintf(s_device_name, sizeof(s_device_name), "%s", name);
}

static void classic_hid_clear_connection_state(void)
{
    s_hid_host_cid = 0;
    s_descriptor_available = false;
    s_passkey_available = false;
    s_pairing_passkey = 0;
}

static void classic_hid_start_inquiry(void)
{
    s_device_count = 0;
    classic_hid_clear_connection_state();
    s_state = CLASSIC_HID_STATE_INQUIRY;

    printf("[G02][ClassicHID] scanning for '%s' / '%s'\n",
           CLASSIC_HID_TARGET_NAME,
           CLASSIC_HID_TARGET_ALIAS);

    int status = gap_inquiry_start(CLASSIC_HID_INQUIRY_DURATION_1280MS);
    if (status != ERROR_CODE_SUCCESS) {
        s_state = CLASSIC_HID_STATE_ERROR;
        printf("[G02][ClassicHID] gap_inquiry_start failed: 0x%02x\n", status);
    }
}

static void classic_hid_connect_target(const bd_addr_t address, const char *name)
{
    memcpy(s_target_addr, address, sizeof(bd_addr_t));
    classic_hid_set_device_name(name);
    s_state = CLASSIC_HID_STATE_CONNECTING;
    s_passkey_available = false;
    gap_inquiry_stop();

    printf("[G02][ClassicHID] opening HID connection to %s (%s)\n",
           bd_addr_to_str(s_target_addr), s_device_name);

    uint16_t new_cid = 0;
    uint8_t status = hid_host_connect(s_target_addr, HID_PROTOCOL_MODE_REPORT, &new_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[G02][ClassicHID] hid_host_connect failed: 0x%02x\n", status);
        s_state = CLASSIC_HID_STATE_ERROR;
        classic_hid_start_inquiry();
        return;
    }

    s_hid_host_cid = new_cid;
}

static void classic_hid_request_next_remote_name(void)
{
    s_state = CLASSIC_HID_STATE_RESOLVING_NAMES;

    for (int i = 0; i < s_device_count; ++i) {
        if (s_devices[i].name_state != DEVICE_NAME_UNKNOWN) {
            continue;
        }

        s_devices[i].name_state = DEVICE_NAME_REQUESTED;
        int status = gap_remote_name_request(
            s_devices[i].address,
            s_devices[i].page_scan_repetition_mode,
            s_devices[i].clock_offset | 0x8000);

        if (status == ERROR_CODE_SUCCESS) {
            return;
        }

        s_devices[i].name_state = DEVICE_NAME_RESOLVED;
    }

    classic_hid_start_inquiry();
}

static void classic_hid_handle_inquiry_result(uint8_t *packet)
{
    if (s_state != CLASSIC_HID_STATE_INQUIRY) {
        return;
    }

    bd_addr_t address;
    gap_event_inquiry_result_get_bd_addr(packet, address);
    if (classic_hid_device_index_for_address(address) >= 0) {
        return;
    }

    if (gap_event_inquiry_result_get_name_available(packet)) {
        char name[249];
        uint8_t name_len = gap_event_inquiry_result_get_name_len(packet);
        if (name_len >= sizeof(name)) {
            name_len = sizeof(name) - 1;
        }
        memcpy(name, gap_event_inquiry_result_get_name(packet), name_len);
        name[name_len] = '\0';

        if (classic_hid_target_name_matches(name)) {
            classic_hid_connect_target(address, name);
            return;
        }
    }

    if (s_device_count >= CLASSIC_HID_MAX_DISCOVERED_DEVICES) {
        return;
    }

    memcpy(s_devices[s_device_count].address, address, sizeof(bd_addr_t));
    s_devices[s_device_count].page_scan_repetition_mode =
        gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    s_devices[s_device_count].clock_offset =
        gap_event_inquiry_result_get_clock_offset(packet);
    s_devices[s_device_count].name_state =
        gap_event_inquiry_result_get_name_available(packet)
            ? DEVICE_NAME_RESOLVED
            : DEVICE_NAME_UNKNOWN;
    ++s_device_count;
}

static void classic_hid_handle_remote_name_complete(uint8_t *packet)
{
    if (s_state != CLASSIC_HID_STATE_RESOLVING_NAMES) {
        return;
    }

    bd_addr_t address;
    reverse_bd_addr(&packet[3], address);
    int index = classic_hid_device_index_for_address(address);
    if (index < 0) {
        classic_hid_request_next_remote_name();
        return;
    }

    s_devices[index].name_state = DEVICE_NAME_RESOLVED;
    if (packet[2] == ERROR_CODE_SUCCESS) {
        const char *name = (const char *)&packet[9];
        if (classic_hid_target_name_matches(name)) {
            classic_hid_connect_target(address, name);
            return;
        }
    }

    classic_hid_request_next_remote_name();
}

static void classic_hid_enqueue_usb_report(const uint8_t *report, uint16_t report_len)
{
    // Classic HID DATA input packets include the HIDP DATA transaction byte
    // (0xA1). USB must receive only the report payload, including Report ID.
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
        printf("[G02][ClassicHID] HID queue full; report dropped\n");
    }
}

static void classic_hid_handle_hid_report(uint8_t *packet)
{
    const uint8_t *report = hid_subevent_report_get_report(packet);
    uint16_t report_len = hid_subevent_report_get_report_len(packet);
    classic_hid_enqueue_usb_report(report, report_len);
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
                classic_hid_start_inquiry();
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT:
            classic_hid_handle_inquiry_result(packet);
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (s_state == CLASSIC_HID_STATE_INQUIRY) {
                classic_hid_request_next_remote_name();
            }
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            classic_hid_handle_remote_name_complete(packet);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            s_state = CLASSIC_HID_STATE_PAIRING;
            // Preserve the proven POC legacy fallback. The future GUI pairing
            // flow can replace this with an explicit user-driven PIN policy.
            gap_pin_code_response(event_addr, "0000");
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            s_state = CLASSIC_HID_STATE_PAIRING;
            gap_ssp_confirmation_response(event_addr);
            break;

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            s_pairing_passkey = little_endian_read_32(packet, 8);
            s_passkey_available = true;
            s_state = CLASSIC_HID_STATE_PAIRING;
            printf("[G02][ClassicHID] pairing passkey available: %06" PRIu32 "\n",
                   (uint32_t)s_pairing_passkey);
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    s_hid_host_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
                    s_state = CLASSIC_HID_STATE_CONNECTING;
                    gap_inquiry_stop();
                    hid_host_accept_connection((uint16_t)s_hid_host_cid,
                                               HID_PROTOCOL_MODE_REPORT);
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        s_state = CLASSIC_HID_STATE_ERROR;
                        classic_hid_start_inquiry();
                        break;
                    }
                    s_hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    s_descriptor_available = false;
                    s_passkey_available = false;
                    s_state = CLASSIC_HID_STATE_CONNECTED;
                    break;
                }

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        s_state = CLASSIC_HID_STATE_ERROR;
                        break;
                    }

                    s_descriptor_available = true;
                    s_state = CLASSIC_HID_STATE_READY;
                    // Core 0 clears stale reports before reconnecting USB and
                    // then serves this descriptor directly from BTstack storage.
                    g_usb_reinit_request = true;
                    break;
                }

                case HID_SUBEVENT_REPORT:
                    classic_hid_handle_hid_report(packet);
                    break;

                case HID_SUBEVENT_CONNECTION_CLOSED: {
                    bool had_descriptor = s_descriptor_available;
                    classic_hid_clear_connection_state();
                    if (had_descriptor) {
                        g_usb_reinit_request = true;
                    }
                    classic_hid_start_inquiry();
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
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_set_local_name("Pico Remapper 00:00:00:00:00:00");
    gap_discoverable_control(1);

    s_hci_event_callback_registration.callback = &classic_hid_packet_handler;
    hci_add_event_handler(&s_hci_event_callback_registration);
    hci_power_control(HCI_POWER_ON);
}

void CLASSIC_HID_CoreMain(void)
{
    if (cyw43_arch_init() != PICO_OK) {
        s_state = CLASSIC_HID_STATE_ERROR;
        panic("cyw43_arch_init failed");
    }

    classic_hid_init();
    btstack_run_loop_execute();
    cyw43_arch_deinit();
}

bool CLASSIC_HID_IsReady(void)
{
    return s_state == CLASSIC_HID_STATE_READY &&
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

classic_hid_state_t CLASSIC_HID_GetState(void)
{
    return s_state;
}

const char *CLASSIC_HID_GetStateName(void)
{
    switch (s_state) {
        case CLASSIC_HID_STATE_BOOTING: return "BOOTING";
        case CLASSIC_HID_STATE_INQUIRY: return "SCANNING";
        case CLASSIC_HID_STATE_RESOLVING_NAMES: return "RESOLVING";
        case CLASSIC_HID_STATE_CONNECTING: return "CONNECTING";
        case CLASSIC_HID_STATE_PAIRING: return "PAIRING";
        case CLASSIC_HID_STATE_CONNECTED: return "CONNECTED";
        case CLASSIC_HID_STATE_READY: return "READY";
        case CLASSIC_HID_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *CLASSIC_HID_GetDeviceName(void)
{
    return s_device_name;
}

bool CLASSIC_HID_HasPairingPasskey(void)
{
    return s_passkey_available;
}

uint32_t CLASSIC_HID_GetPairingPasskey(void)
{
    return s_pairing_passkey;
}
