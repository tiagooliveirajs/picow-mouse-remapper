#ifndef CANONICAL_HID_H
#define CANONICAL_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Common.h"

// PICO-04 canonical BLE-HID -> fixed USB-HID adapter.
//
// Remote Report IDs/layouts never escape this module. Outputs always use the
// firmware-owned report IDs declared in usb_descriptors.h.
void canonical_hid_init(void);
void canonical_hid_reset_device(void);

// Convert one post-quirk remote HID report into zero or more fixed USB reports.
// The input may contain an in-band remote Report ID (legacy POC path) or expose
// it through input->report_id. Returns the number of outputs written.
size_t canonical_hid_process_remote_report(const ST_HID_RPT *input,
                                           const uint8_t *descriptor,
                                           uint16_t descriptor_len,
                                           ST_HID_RPT *outputs,
                                           size_t output_capacity);

// Force all active canonical outputs to released/neutral state. Used on BLE
// disconnect and before state-changing operations to prevent stuck inputs.
size_t canonical_hid_neutralize(ST_HID_RPT *outputs, size_t output_capacity);

#endif // CANONICAL_HID_H
