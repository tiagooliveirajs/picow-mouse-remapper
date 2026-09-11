// POC wrapper around the existing HOGP host implementation.
//
// Keeping the upstream-derived hog_host_demo.c unchanged makes it easy to
// compare/rebase the original BLE host while exposing the minimum transport
// hooks required by the Logitech Lift HID++ experiment.

#include "hog_host_demo.c"

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
    return hids_host_send_write_report(hids_cid,
                                       0x11,
                                       HID_REPORT_TYPE_OUTPUT,
                                       payload,
                                       payload_len);
}
