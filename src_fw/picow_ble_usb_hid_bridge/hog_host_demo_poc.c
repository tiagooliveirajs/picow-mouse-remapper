// POC wrapper around the existing HOGP host implementation.
//
// Keeping the upstream-derived hog_host_demo.c unchanged makes it easy to
// compare/rebase the original BLE host while exposing the minimum transport
// hooks required by the Logitech Lift HID++ experiment.

// Pico SDK 2.2.0 vendors a BTstack HIDS client that inserts the HID Report ID
// as the first byte of the GATTSERVICE_SUBEVENT_HID_REPORT data field, while it
// also exposes that same Report ID separately in the event metadata.  The
// bridge's internal ST_HID_RPT representation keeps the Report ID only in
// report_id and expects report[] to contain the characteristic payload without
// that leading byte.  Normalize the two BTstack getters used by the original
// demo before including it, so ordinary mouse coordinates are not shifted by
// one byte and the USB path does not prepend a duplicate Report ID.
#include "btstack.h"

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

#define gattservice_subevent_hid_report_get_report     poc_btstack_hid_report_payload
#define gattservice_subevent_hid_report_get_report_len poc_btstack_hid_report_payload_len
#include "hog_host_demo.c"
#undef gattservice_subevent_hid_report_get_report
#undef gattservice_subevent_hid_report_get_report_len

uint16_t poc_hids_current_cid(void)
{
    return hids_cid;
}

uint8_t poc_hids_send_hidpp_long(const uint8_t *payload, uint8_t payload_len)
{
    if (hids_cid == 0 || payload == NULL || payload_len == 0) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    // HID++ 2.0 long reports use HID Report ID 0x11. HOGP carries the report
    // ID in the Report Reference descriptor, so payload contains the 19 bytes
    // after the report ID.
    //
    // Pico SDK 2.2.0 vendors BTstack at a revision where the HIDS host API
    // still uses the hids_client_* naming. The later hids_host_* alias is not
    // available there, so use the API actually declared by hids_client.h.
    return hids_client_send_write_report(hids_cid,
                                         0x11,
                                         HID_REPORT_TYPE_OUTPUT,
                                         payload,
                                         payload_len);
}
