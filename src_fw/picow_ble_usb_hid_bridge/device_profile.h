#ifndef DEVICE_PROFILE_H
#define DEVICE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_PROFILE_MAX_RECORDS 8u

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

typedef struct {
    bool valid;
    uint8_t identity_addr_type;
    uint8_t identity_addr[6];
    int16_t bond_index;
    bool pnp_valid;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t product_version;
    uint32_t capabilities;
    device_profile_mode_t profile_mode;
} device_profile_saved_record_t;

typedef struct {
    uint32_t revision;
    bool storage_ok;
    uint8_t count;
    device_profile_saved_record_t records[DEVICE_PROFILE_MAX_RECORDS];
} device_profile_catalog_t;

void device_profile_init(void);

// The connection handle is represented as its 16-bit wire/runtime value here
// so this public header does not import BTstack HID types into TinyUSB code.
void device_profile_on_hids_ready(uint16_t connection_handle,
                                  const uint8_t peer_addr[6],
                                  uint8_t peer_addr_type,
                                  const uint8_t *report_descriptor,
                                  uint16_t report_descriptor_len);

void device_profile_on_disconnect(void);
bool device_profile_get_snapshot(device_profile_snapshot_t *snapshot);

// PICO-07 saved-device catalog. The catalog is a Core0-safe snapshot of the
// persistent DeviceRecord table. Delete is a Core1-only operation.
bool device_profile_get_catalog(device_profile_catalog_t *catalog);
bool device_profile_delete_saved(uint8_t addr_type,
                                 const uint8_t addr[6],
                                 int16_t *removed_bond_index);

device_drag_backend_t device_profile_resolve_drag_backend(device_source_button_t source,
                                                          device_drag_fix_policy_t policy);

bool device_profile_forward_hidpp_remap_active(void);

#endif // DEVICE_PROFILE_H
