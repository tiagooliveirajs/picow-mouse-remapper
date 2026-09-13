#pragma once

// PICO-07 physical keyboard-pairing validation overrides.
//
// The inherited BTstack HOGP demo uses a 5,000 ms passive scan and accepts an
// advertising report only when that single report already contains the HID
// Service UUID (0x1812). That is sufficient for the validated Logitech Lift,
// but it can miss keyboards that put relevant discovery data in the scan
// response or advertise themselves via the standard GAP HID Appearance.
//
// For this validation candidate:
//   * extend the 5 s discovery timer to 200 s;
//   * use active scanning so scan-response PDUs are requested;
//   * accept either HID Service UUID or standard HID GAP Appearance category;
//   * enlarge HIDS Report Map storage from the demo's 500 bytes to 1024 bytes;
//   * leave the 8 s connection timeout and Security Manager policy unchanged.

#include <stdbool.h>
#include <stdint.h>
#include "btstack.h"

#define PICO07_HID_APPEARANCE_AD_TYPE   0x19u
#define PICO07_HID_APPEARANCE_CATEGORY  0x03c0u
#define PICO07_HID_APPEARANCE_MASK      0xffc0u

__attribute__((used)) static const char pico07_keyboard_pairing_build_marker[] =
    "PICO07 KBD ACTIVE SCAN 200S";

static uint8_t pico07_hid_descriptor_storage_expanded[1024];

static inline void pico07_btstack_set_timer_with_long_scan(
    btstack_timer_source_t *timer,
    uint32_t timeout_ms)
{
    if (timeout_ms == 5000u) {
        timeout_ms = 200000u;
    }
    btstack_run_loop_set_timer(timer, timeout_ms);
}

static inline void pico07_gap_set_active_scan(
    uint8_t scan_type,
    uint16_t scan_interval,
    uint16_t scan_window)
{
    (void)scan_type;
    gap_set_scan_parameters(1u, scan_interval, scan_window);
}

static inline bool pico07_ad_data_contains_hid_compatible(
    uint8_t ad_len,
    const uint8_t *ad_data,
    uint16_t uuid16)
{
    if (ad_data_contains_uuid16(ad_len, ad_data, uuid16)) return true;
    if (uuid16 != ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE ||
        ad_data == NULL) {
        return false;
    }

    uint16_t offset = 0u;
    while (offset < ad_len) {
        const uint8_t field_len = ad_data[offset];
        if (field_len == 0u) break;

        const uint16_t next = (uint16_t)(offset + 1u + field_len);
        if (next > ad_len) break;

        const uint8_t ad_type = ad_data[offset + 1u];
        if (ad_type == PICO07_HID_APPEARANCE_AD_TYPE && field_len >= 3u) {
            const uint16_t appearance = little_endian_read_16(ad_data, offset + 2u);
            if ((appearance & PICO07_HID_APPEARANCE_MASK) ==
                PICO07_HID_APPEARANCE_CATEGORY) {
                return true;
            }
        }
        offset = next;
    }
    return false;
}

static inline void pico07_hids_client_init_expanded(
    uint8_t *storage,
    uint16_t storage_len)
{
    (void)storage;
    (void)storage_len;
    hids_client_init(pico07_hid_descriptor_storage_expanded,
                     (uint16_t)sizeof(pico07_hid_descriptor_storage_expanded));
}

#define btstack_run_loop_set_timer(timer, timeout_ms) \
    pico07_btstack_set_timer_with_long_scan((timer), (timeout_ms))

#define gap_set_scan_parameters(scan_type, scan_interval, scan_window) \
    pico07_gap_set_active_scan((scan_type), (scan_interval), (scan_window))

#define ad_data_contains_uuid16(ad_len, ad_data, uuid16) \
    pico07_ad_data_contains_hid_compatible((ad_len), (ad_data), (uuid16))

#define hids_client_init(storage, storage_len) \
    pico07_hids_client_init_expanded((storage), (storage_len))
