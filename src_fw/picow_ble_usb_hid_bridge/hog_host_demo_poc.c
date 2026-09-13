// PICO-06 wrapper around the existing HOGP host implementation.
//
// The upstream-derived hog_host_demo.c remains intact. This wrapper owns the
// product boundary around it: normalize BTstack reports, consume Logitech HID++
// control traffic when a source needs real held-state, enqueue ordinary raw HOGP
// reports, and run identity/remap persistence on the BTstack Core1 loop.

#include "btstack.h"
#include "device_profile.h"
#include "logitech_hidpp.h"
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

static bool pico06_remote_enqueue(ULONG queue_kind, PVOID data);

#define CMN_Enqueue(kind, data) pico06_remote_enqueue((kind), (data))
#define ble_host_main poc_ble_host_main_original

// Pico SDK 2.2.0's HIDS client includes the Report ID as byte 0 of the event
// payload and also exposes it in metadata. ST_HID_RPT keeps it only in report_id.
#define gattservice_subevent_hid_report_get_report     poc_btstack_hid_report_payload
#define gattservice_subevent_hid_report_get_report_len poc_btstack_hid_report_payload_len
#include "hog_host_demo.c"
#undef gattservice_subevent_hid_report_get_report
#undef gattservice_subevent_hid_report_get_report_len
#undef ble_host_main
#undef CMN_Enqueue

#define PICO06_PROFILE_POLL_MS 50u

static btstack_timer_source_t g_pico06_profile_timer;
static uint16_t g_pico06_attached_hids_cid;
static bool g_pico06_profile_attached;

static bool pico06_remote_enqueue(ULONG queue_kind, PVOID data)
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

static void pico06_profile_poll(btstack_timer_source_t *timer)
{
    (void)timer;

    const bool ready = is_ble_app_state_ready();
    const uint16_t current_cid = hids_cid;

    if (!ready || current_cid == 0) {
        if (g_pico06_profile_attached) {
            logitech_hidpp_on_disconnect();
            device_profile_on_disconnect();
            g_pico06_profile_attached = false;
            g_pico06_attached_hids_cid = 0;
            printf("[PICO-06] profile detached\n");
        }
        remap_profile_core1_task();
    } else if (!g_pico06_profile_attached ||
               current_cid != g_pico06_attached_hids_cid) {
        if (g_pico06_profile_attached) {
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
            g_pico06_profile_attached = true;
            g_pico06_attached_hids_cid = current_cid;
            logitech_hidpp_on_connect();
            printf("[PICO-06] profile attached hids_cid=%u\n", current_cid);
        }
    }

    if (g_pico06_profile_attached) {
        remap_profile_core1_task();
        logitech_hidpp_core1_task();
    }

    btstack_run_loop_set_timer(&g_pico06_profile_timer, PICO06_PROFILE_POLL_MS);
    btstack_run_loop_add_timer(&g_pico06_profile_timer);
}

void ble_host_main(void)
{
    (void)picow_bt_example_init();
    picow_bt_example_main();

    g_pico06_attached_hids_cid = 0;
    g_pico06_profile_attached = false;
    btstack_run_loop_set_timer_handler(&g_pico06_profile_timer, pico06_profile_poll);
    btstack_run_loop_set_timer(&g_pico06_profile_timer, PICO06_PROFILE_POLL_MS);
    btstack_run_loop_add_timer(&g_pico06_profile_timer);

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
