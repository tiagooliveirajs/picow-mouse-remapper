/*
 * Copyright (C) 2020 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 * personal benefit and not for any commercial purpose or for
 * monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 */

#define BTSTACK_FILE__ "hog_host_demo.c"

#include <inttypes.h>
#include <stdio.h>
#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"
#include "hog_host_demo.h"
#include "picow_bt_example_common.h"
#include "pico/cyw43_arch.h"
#include "pico07_pairing.h"
#include "Common.h"

// Three seconds was marginal once PICO-07 started intentionally disconnecting
// the active mouse before a user-selected pairing attempt. Give peripherals
// enough time to wake/advertise/connect without changing the pairing timeout.
#define CONNECTION_TIMEOUT_MS 8000
#define SCAN_TIMEOUT_MS       5000

#define TLV_TAG_HOGD ((((uint32_t) 'H') << 24 ) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static enum {
    W4_WORKING,
    W4_HID_DEVICE_FOUND,
    W4_CONNECTED,
    W4_ENCRYPTED,
    W4_HID_CLIENT_CONNECTED,
    READY,
    W4_TIMEOUT_THEN_SCAN,
    W4_TIMEOUT_THEN_RECONNECT,
} app_state;

static le_device_addr_t remote_device;
static hci_con_handle_t connection_handle;
static uint16_t hids_cid;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
static uint8_t hid_descriptor_storage[500];
static btstack_timer_source_t connection_timer;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static const btstack_tlv_t * btstack_tlv_singleton_impl;
static void * btstack_tlv_singleton_context;

extern volatile bool g_usb_reinit_request;

void ble_host_main(void);
bool is_ble_app_state_ready(void);
const uint8_t* get_ble_hid_report_descriptor_data(void);
uint16_t get_ble_hid_report_descriptor_len(void);

// Product pairing hooks are implemented by the PICO-07 wrapper translation
// unit. Keeping the BTstack demo state machine as the transport lets PICO-07
// add user-selected discovery without duplicating HIDS setup.
bool pico07_pairing_scan_timeout_owned(void);
bool pico07_pairing_connection_timeout_owned(void);
bool pico07_pairing_idle_without_saved_device(void);
bool pico07_pairing_consume_advertisement(const uint8_t *packet);
bool pico07_pairing_connection_is_managed(void);
bool pico07_pairing_handle_disconnect(void);
bool pico07_pairing_handle_connection_error(void);
void pico07_pairing_hids_transport_ready(void);

// hog_host_demo.c is textually included by hog_host_demo_poc.c. The state
// publisher is defined later in that same translation unit, so it can also be
// used by the Security Manager path to surface passkeys on the local LCD.
static void pair_publish_state(pico07_pair_state_t state, const char *message);

static void hog_start_scan(void);
static void hog_start_connect(void);

static void hid_handle_input_report(uint8_t service_index, uint8_t report_id,
                                    const uint8_t * report, uint16_t report_len){
    (void)service_index;
    static ST_HID_RPT stHidRpt;
    stHidRpt.report_id  = report_id;
    stHidRpt.report_len = report_len;
    if (stHidRpt.report_len > CMN_HID_RPT_DATA_SIZE) {
        stHidRpt.report_len = CMN_HID_RPT_DATA_SIZE;
    }
    memcpy(stHidRpt.report, report, stHidRpt.report_len);
    (void)CMN_Enqueue(CMN_QUE_KIND_HID_RPT, &stHidRpt);
}

static bool adv_event_contains_hid_service(const uint8_t * packet){
    const uint8_t * ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data,
                                   ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void hog_scan_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (app_state != W4_HID_DEVICE_FOUND) return;
    if (pico07_pairing_scan_timeout_owned()) return;
    printf("Scan timeout. Switching to bonded connection attempt...\n");
    gap_stop_scan();
    hog_start_connect();
}

static void hog_start_scan(void){
    printf("Scanning for LE HID devices (Timeout %dms)...\n", SCAN_TIMEOUT_MS);
    app_state = W4_HID_DEVICE_FOUND;
    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, SCAN_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_scan_timeout);
    btstack_run_loop_add_timer(&connection_timer);
    gap_set_scan_parameters(0,48,48);
    gap_start_scan();
}

static void hog_connection_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
    printf("Connection timeout.\n");
    const bool managed = pico07_pairing_connection_is_managed();
    gap_connect_cancel();

    // Only UI-owned attempts are terminal PICO-07 errors. Normal preferred
    // reconnects retain the proven PICO-06 fallback: scan and reconnect a HID
    // instead of getting stuck in DEVICE UNAVAILABLE after a short wake delay.
    if (managed && pico07_pairing_connection_timeout_owned()) return;
    hog_start_scan();
}

static void hog_connect(void) {
    printf("Connecting to device %s (Timeout %dms)...\n",
           bd_addr_to_str(remote_device.addr), CONNECTION_TIMEOUT_MS);
    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_connection_timeout);
    btstack_run_loop_add_timer(&connection_timer);
    app_state = W4_CONNECTED;
    gap_connect(remote_device.addr, remote_device.addr_type);
}

static void hog_start_connect(void){
    btstack_tlv_get_instance(&btstack_tlv_singleton_impl,
                             &btstack_tlv_singleton_context);
    if (btstack_tlv_singleton_impl){
        int len = btstack_tlv_singleton_impl->get_tag(
            btstack_tlv_singleton_context, TLV_TAG_HOGD,
            (uint8_t *) &remote_device, sizeof(remote_device));
        if (len == sizeof(remote_device)){
            printf("Bonded preferred device found, trying to connect...\n");
            hog_connect();
            return;
        }
    }
    // With no preferred peer, discovery is explicitly owned by Pair New Device.
    if (pico07_pairing_idle_without_saved_device()) return;
    hog_start_scan();
}

static void handle_outgoing_connection_error(void){
    printf("Outgoing connection/pairing error\n");
    const bool managed = pico07_pairing_connection_is_managed();
    if (connection_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(connection_handle);

    // Keep PICO-07 errors local to explicit UI attempts. A normal reconnect
    // falls back to the PICO-06 scan loop rather than becoming permanently
    // unavailable after a transient HIDS/security failure.
    if (managed && pico07_pairing_handle_connection_error()) return;
    hog_start_scan();
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel,
                                     uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t status;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)){
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            switch (status){
                case ERROR_CODE_SUCCESS:
                    printf("HID service client connected, found %d services\n",
                        gattservice_subevent_hid_service_connected_get_num_instances(packet));

                    // Legacy reconnects keep the preferred peer tag. A PICO-07
                    // user-selected peer is only promoted to preferred after its
                    // Report Map has been classified as a supported mouse.
                    if (btstack_tlv_singleton_impl &&
                        !pico07_pairing_connection_is_managed()) {
                        btstack_tlv_singleton_impl->store_tag(
                            btstack_tlv_singleton_context, TLV_TAG_HOGD,
                            (const uint8_t *) &remote_device,
                            sizeof(remote_device));
                    }
                    printf("Ready - HID transport active.\n");
                    app_state = READY;
                    g_usb_reinit_request = true;
                    pico07_pairing_hids_transport_ready();
                    break;
                default:
                    printf("HID service client connection failed, status 0x%02x.\n", status);
                    handle_outgoing_connection_error();
                    break;
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            printf("HID service client disconnected\n");
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            hid_handle_input_report(
                gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report_id(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            break;
    }
}

static void packet_handler (uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t event;
    switch (packet_type) {
        case HCI_EVENT_PACKET:
            event = hci_event_packet_get_type(packet);
            switch (event) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
                    btstack_assert(app_state == W4_WORKING);
                    hog_start_connect();
                    break;

                case GAP_EVENT_ADVERTISING_REPORT:
                    if (app_state != W4_HID_DEVICE_FOUND) break;
                    if (!adv_event_contains_hid_service(packet)) break;
                    if (pico07_pairing_consume_advertisement(packet)) break;

                    btstack_run_loop_remove_timer(&connection_timer);
                    gap_stop_scan();
                    gap_event_advertising_report_get_address(packet, remote_device.addr);
                    remote_device.addr_type = gap_event_advertising_report_get_address_type(packet);
                    printf("Found HID device, connecting...\n");
                    hog_connect();
                    break;

                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    connection_handle = HCI_CON_HANDLE_INVALID;
                    printf("\nDisconnected.\n");
                    btstack_run_loop_remove_timer(&connection_timer);
                    app_state = W4_WORKING;
                    if (pico07_pairing_handle_disconnect()) break;
                    hog_start_connect();
                    break;

                case HCI_EVENT_META_GAP:
                    if (hci_event_gap_meta_get_subevent_code(packet) !=
                        GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
                    if (app_state != W4_CONNECTED) return;
                    btstack_run_loop_remove_timer(&connection_timer);
                    connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                    app_state = W4_ENCRYPTED;
                    sm_request_pairing(connection_handle);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bool connect_to_service = false;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("Just works requested\n");
            pair_publish_state(PICO07_PAIR_CONNECTING, "CONFIRMING PAIRING");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
            const uint32_t passkey =
                sm_event_numeric_comparison_request_get_passkey(packet);
            char message[21];
            snprintf(message, sizeof(message), "VERIFY %06" PRIu32, passkey);
            pair_publish_state(PICO07_PAIR_CONNECTING, message);
            printf("Confirming numeric comparison: %06" PRIu32 "\n", passkey);
            sm_numeric_comparison_confirm(
                sm_event_numeric_comparison_request_get_handle(packet));
            break;
        }

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER: {
            const uint32_t passkey =
                sm_event_passkey_display_number_get_passkey(packet);
            char message[21];
            // Exactly 20 visible characters for a six-digit passkey. It fits
            // the current screen and remains intact after UI_TEXT_MAX -> 21.
            snprintf(message, sizeof(message), "TYPE %06" PRIu32 " ON KEYBD", passkey);
            pair_publish_state(PICO07_PAIR_CONNECTING, message);
            printf("Display Passkey: %06" PRIu32 "\n", passkey);
            break;
        }

        case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
            pair_publish_state(PICO07_PAIR_CONNECTING, "PAIRING CONTINUES");
            break;

        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)){
                case ERROR_CODE_SUCCESS:
                    printf("Pairing complete, success\n");
                    pair_publish_state(PICO07_PAIR_CONNECTING, "PAIRING COMPLETE");
                    connect_to_service = true;
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("Pairing failed, timeout\n");
                    handle_outgoing_connection_error();
                    break;
                default:
                    printf("Pairing failed, status 0x%02x reason 0x%02x\n",
                           sm_event_pairing_complete_get_status(packet),
                           sm_event_pairing_complete_get_reason(packet));
                    handle_outgoing_connection_error();
                    break;
            }
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            const uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("Re-encryption complete, success\n");
                pair_publish_state(PICO07_PAIR_CONNECTING, "BOND RESTORED");
                connect_to_service = true;
                break;
            }

            if (status == ERROR_CODE_PIN_OR_KEY_MISSING) {
                // BTstack's documented recovery for a peer that lost its side
                // of a bond: remove the stale local bond and start fresh pairing
                // on the current connection. This is important for mice/keyboards
                // that were factory-reset or re-paired elsewhere.
                bd_addr_t addr;
                sm_event_reencryption_complete_get_address(packet, addr);
                const bd_addr_type_t addr_type =
                    (bd_addr_type_t)sm_event_reencryption_complete_get_addr_type(packet);
                printf("Re-encryption failed: peer lost bond, resetting local bond\n");
                gap_delete_bonding(addr_type, addr);
                pair_publish_state(PICO07_PAIR_CONNECTING, "BOND RESET - PAIRING");
                sm_request_pairing(sm_event_reencryption_complete_get_handle(packet));
                break;
            }

            printf("Re-encryption failed, status 0x%02x\n", status);
            handle_outgoing_connection_error();
            break;
        }

        default:
            break;
    }

    if (connect_to_service){
        printf("Search for HID service.\n");
        app_state = W4_HID_CLIENT_CONNECTED;
        hids_client_connect(connection_handle, handle_gatt_client_event,
                            protocol_mode, &hids_cid);
    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void)argc;
    (void)argv;

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    gatt_client_init();
    att_server_init(profile_data, NULL, NULL);
    hids_client_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    setvbuf(stdin, NULL, _IONBF, 0);
    app_state = W4_WORKING;
    hci_power_control(HCI_POWER_ON);
    return 0;
}

void ble_host_main(void)
{
    (void)picow_bt_example_init();
    picow_bt_example_main();
    btstack_run_loop_execute();
}

bool is_ble_app_state_ready(void)
{
    return READY == app_state;
}

const uint8_t* get_ble_hid_report_descriptor_data(void)
{
    return hids_client_descriptor_storage_get_descriptor_data(hids_cid, 0);
}

uint16_t get_ble_hid_report_descriptor_len(void)
{
    return hids_client_descriptor_storage_get_descriptor_len(hids_cid, 0);
}
