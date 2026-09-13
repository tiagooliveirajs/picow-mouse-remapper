#ifndef DEVICE_PROFILE_H
#define DEVICE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"

typedef enum {
    DEVICE_CAP_MOUSE          = 1u << 0,
    DEVICE_CAP_KEYBOARD       = 1u << 1,
    DEVICE_CAP_CONSUMER       = 1u << 2,
    DEVICE_CAP_VENDOR_REPORTS = 1u << 3,
} device_capability_t;

typedef enum {
    DEVICE_PROFILE_PASSTHROUGH = 0,
    DEVICE_PROFILE_DEFAULT_REMAP = 1,
    DEVICE_PROFILE_CUSTOM_REMAP = 2,
} device_profile_mode_t;

typedef enum {
    DEVICE_DRAG_FIX_AUTO = 0,
    DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED = 1,
    DEVICE_DRAG_FIX_OFF = 2,
} device_drag_fix_policy_t;

typedef enum {
    DEVICE_SOURCE_BACK = 4,
    DEVICE_SOURCE_FORWARD = 5,
} device_source_button_t;

typedef enum {
    DEVICE_DRAG_BACKEND_STANDARD = 0,
    DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4 = 1,
    DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4 = 2,
    DEVICE_DRAG_BACKEND_UNSUPPORTED = 3,
} device_drag_backend_t;

typedef struct {
    uint32_t revision;
    bool connected;
    bool storage_ok;
    bool profile_restored;
    bool pnp_query_complete;
    bool pnp_valid;

    uint8_t identity_addr_type;
    uint8_t identity_addr[6];
    int16_t bond_index;

    uint8_t vendor_id_source;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t product_version;
    uint32_t report_map_fingerprint;
    uint32_t capabilities;

    uint8_t detected_quirks;
    device_profile_mode_t profile_mode;
    device_drag_fix_policy_t drag_fix_back;
    device_drag_fix_policy_t drag_fix_forward;
    device_drag_backend_t auto_back_backend;
    device_drag_backend_t auto_forward_backend;
} device_profile_snapshot_t;

// Called on Core0 before launching the BTstack Core1 runtime.
void device_profile_init(void);

// Called by the HOGP host after the HID service is ready. The module resolves
// the bonded peer identity, fingerprints/classifies the Report Map, restores or
// creates a per-bond profile, and asynchronously queries the standard PnP ID.
void device_profile_on_hids_ready(hci_con_handle_t connection_handle,
                                  const uint8_t peer_addr[6],
                                  uint8_t peer_addr_type,
                                  const uint8_t *report_descriptor,
                                  uint16_t report_descriptor_len);

// Called on HCI disconnect. Persistent records remain intact.
void device_profile_on_disconnect(void);

// Cross-core read-only snapshot for diagnostics/UI and later gates.
bool device_profile_get_snapshot(device_profile_snapshot_t *snapshot);

// Resolve drag-fix policy per source button. AUTO only returns HID++ for a
// qualified device+source pair. FORCE returns PROBE when a real HID++ probe is
// meaningful, and UNSUPPORTED otherwise.
device_drag_backend_t device_profile_resolve_drag_backend(device_source_button_t source,
                                                          device_drag_fix_policy_t policy);

// PICO-05 deliberately leaves every newly created profile in PASSTHROUGH.
// Therefore the legacy PICO-01 Forward->Left HID++ remap must stay disabled.
// PICO-06 will consume the policy resolver when Default/Custom mappings exist.
bool device_profile_forward_hidpp_remap_active(void);

#endif // DEVICE_PROFILE_H
