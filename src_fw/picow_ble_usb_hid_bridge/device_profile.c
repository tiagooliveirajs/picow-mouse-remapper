#include "device_profile.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <btstack_tlv.h>
#include "ble/le_device_db.h"
#include "pico/stdlib.h"

#define PROFILE_STORE_MAGIC 0x35504d52u /* "RMP5" */
#define PROFILE_STORE_SCHEMA_VERSION 1u
#define PROFILE_STORE_MAX_RECORDS DEVICE_PROFILE_MAX_RECORDS
#define PROFILE_STORE_TAG ((((uint32_t)'R') << 24) | (((uint32_t)'M') << 16) | (((uint32_t)'P') << 8) | 'F')

#define PROFILE_QUIRK_BACK    (1u << 0)
#define PROFILE_QUIRK_FORWARD (1u << 1)

#define LOGITECH_USB_VID 0x046du
#define LOGITECH_LIFT_B031 0xb031u
#define LOGITECH_M650_B02A 0xb02au
#define LOGITECH_MX_ANYWHERE_3_B025 0xb025u
#define LOGITECH_MX_ANYWHERE_3S_B037 0xb037u

#define PNP_ID_UUID 0x2a50u
#define PNP_QUERY_RETRY_MS 50u
#define PNP_QUERY_MAX_RETRIES 40u

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01u
#define HID_USAGE_PAGE_KEYBOARD        0x07u
#define HID_USAGE_PAGE_CONSUMER        0x0cu
#define HID_USAGE_PAGE_VENDOR_MIN      0xff00u
#define HID_USAGE_MOUSE                0x02u
#define HID_USAGE_KEYBOARD             0x06u

#pragma pack(push, 1)
typedef struct {
    uint8_t valid;
    uint8_t identity_addr_type;
    uint8_t identity_addr[6];
    int16_t bond_index;

    uint8_t pnp_valid;
    uint8_t vendor_id_source;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t product_version;

    uint32_t report_map_fingerprint;
    uint32_t capabilities;
    uint8_t detected_quirks;

    uint8_t profile_mode;
    uint8_t drag_fix_back;
    uint8_t drag_fix_forward;
    uint8_t mappings[5];
    uint8_t reserved[7];
} profile_record_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t record_size;
    uint32_t generation;
    uint8_t record_count;
    uint8_t reserved[3];
    profile_record_v1_t records[PROFILE_STORE_MAX_RECORDS];
    uint32_t checksum;
} profile_store_v1_t;
#pragma pack(pop)

static critical_section_t g_snapshot_lock;
static bool g_initialized;
static device_profile_snapshot_t g_snapshot;
static device_profile_catalog_t g_catalog;

static profile_store_v1_t g_store;
static bool g_store_loaded;
static bool g_storage_ok = true;
static int g_current_slot = -1;
static profile_record_v1_t g_runtime_record;

static hci_con_handle_t g_connection_handle = HCI_CON_HANDLE_INVALID;
static btstack_timer_source_t g_pnp_timer;
static uint8_t g_pnp_retry_count;
static bool g_pnp_query_active;
static bool g_pnp_value_seen;

static uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t profile_store_checksum(const profile_store_v1_t *store)
{
    return fnv1a32((const uint8_t *)store, offsetof(profile_store_v1_t, checksum));
}

static void profile_store_reset(void)
{
    memset(&g_store, 0, sizeof(g_store));
    g_store.magic = PROFILE_STORE_MAGIC;
    g_store.schema_version = PROFILE_STORE_SCHEMA_VERSION;
    g_store.record_size = sizeof(profile_record_v1_t);
    g_store.generation = 0;
    g_store.record_count = 0;
    g_store.checksum = profile_store_checksum(&g_store);
}

static void publish_catalog(void)
{
    if (!g_initialized) return;

    device_profile_catalog_t next;
    memset(&next, 0, sizeof(next));
    next.storage_ok = g_storage_ok;

    critical_section_enter_blocking(&g_snapshot_lock);
    next.revision = g_catalog.revision + 1u;
    critical_section_exit(&g_snapshot_lock);

    for (size_t i = 0; i < PROFILE_STORE_MAX_RECORDS; ++i) {
        const profile_record_v1_t *record = &g_store.records[i];
        if (!record->valid || next.count >= DEVICE_PROFILE_MAX_RECORDS) continue;
        device_profile_saved_record_t *out = &next.records[next.count++];
        out->valid = true;
        out->identity_addr_type = record->identity_addr_type;
        memcpy(out->identity_addr, record->identity_addr, 6);
        out->bond_index = record->bond_index;
        out->pnp_valid = record->pnp_valid != 0;
        out->vendor_id = record->vendor_id;
        out->product_id = record->product_id;
        out->product_version = record->product_version;
        out->capabilities = record->capabilities;
        out->profile_mode = (device_profile_mode_t)record->profile_mode;
    }

    critical_section_enter_blocking(&g_snapshot_lock);
    memcpy(&g_catalog, &next, sizeof(g_catalog));
    critical_section_exit(&g_snapshot_lock);
}

static void profile_store_load(void)
{
    if (g_store_loaded) return;
    g_store_loaded = true;
    profile_store_reset();

    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl == NULL) {
        g_storage_ok = false;
        printf("[PICO-05] TLV unavailable; runtime passthrough only\n");
        publish_catalog();
        return;
    }

    profile_store_v1_t candidate;
    const int len = tlv_impl->get_tag(tlv_context,
                                      PROFILE_STORE_TAG,
                                      (uint8_t *)&candidate,
                                      sizeof(candidate));
    if (len == 0) {
        g_storage_ok = true;
        printf("[PICO-05] profile store empty; schema v%u\n",
               PROFILE_STORE_SCHEMA_VERSION);
        publish_catalog();
        return;
    }

    const bool valid = len == (int)sizeof(candidate) &&
                       candidate.magic == PROFILE_STORE_MAGIC &&
                       candidate.schema_version == PROFILE_STORE_SCHEMA_VERSION &&
                       candidate.record_size == sizeof(profile_record_v1_t) &&
                       candidate.checksum == profile_store_checksum(&candidate);
    if (!valid) {
        g_storage_ok = false;
        printf("[PICO-05] profile store invalid/incompatible; fail-safe passthrough\n");
        profile_store_reset();
        publish_catalog();
        return;
    }

    memcpy(&g_store, &candidate, sizeof(g_store));
    g_storage_ok = true;
    printf("[PICO-05] profile store loaded generation=%lu records=%u\n",
           (unsigned long)g_store.generation,
           g_store.record_count);
    publish_catalog();
}

static bool profile_store_commit(void)
{
    const btstack_tlv_t *tlv_impl = NULL;
    void *tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl == NULL) {
        g_storage_ok = false;
        publish_catalog();
        return false;
    }

    uint8_t count = 0;
    for (size_t i = 0; i < PROFILE_STORE_MAX_RECORDS; ++i) {
        if (g_store.records[i].valid) ++count;
    }

    g_store.magic = PROFILE_STORE_MAGIC;
    g_store.schema_version = PROFILE_STORE_SCHEMA_VERSION;
    g_store.record_size = sizeof(profile_record_v1_t);
    g_store.record_count = count;
    ++g_store.generation;
    g_store.checksum = profile_store_checksum(&g_store);

    const int result = tlv_impl->store_tag(tlv_context,
                                           PROFILE_STORE_TAG,
                                           (const uint8_t *)&g_store,
                                           sizeof(g_store));
    g_storage_ok = result == 0;
    if (!g_storage_ok) {
        printf("[PICO-05] profile store commit failed rc=%d\n", result);
    }
    publish_catalog();
    return g_storage_ok;
}

static bool identity_matches(const profile_record_v1_t *record,
                             uint8_t addr_type,
                             const uint8_t addr[6])
{
    return record->valid && record->identity_addr_type == addr_type &&
           memcmp(record->identity_addr, addr, 6) == 0;
}

static int find_record(uint8_t addr_type, const uint8_t addr[6])
{
    for (size_t i = 0; i < PROFILE_STORE_MAX_RECORDS; ++i) {
        if (identity_matches(&g_store.records[i], addr_type, addr)) return (int)i;
    }
    return -1;
}

static int find_free_record(void)
{
    for (size_t i = 0; i < PROFILE_STORE_MAX_RECORDS; ++i) {
        if (!g_store.records[i].valid) return (int)i;
    }
    return -1;
}

static uint32_t hid_item_value(const uint8_t *data, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; ++i) value |= ((uint32_t)data[i]) << (8u * i);
    return value;
}

static uint32_t classify_application_collections(const uint8_t *descriptor,
                                                 uint16_t descriptor_len)
{
    uint32_t capabilities = 0;
    uint32_t usage_page = 0;
    uint32_t local_usage = 0;
    bool have_local_usage = false;

    uint16_t offset = 0;
    while (offset < descriptor_len) {
        const uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfeu) {
            if (offset + 2u > descriptor_len) break;
            const uint8_t long_size = descriptor[offset];
            offset = (uint16_t)(offset + 2u + long_size);
            continue;
        }

        uint8_t item_size = prefix & 0x03u;
        if (item_size == 3u) item_size = 4u;
        if ((uint32_t)offset + item_size > descriptor_len) break;
        const uint8_t item_type = (prefix >> 2) & 0x03u;
        const uint8_t item_tag = (prefix >> 4) & 0x0fu;
        const uint32_t value = hid_item_value(&descriptor[offset], item_size);
        offset = (uint16_t)(offset + item_size);

        if (item_type == 1u && item_tag == 0u) {
            usage_page = value;
        } else if (item_type == 2u && item_tag == 0u) {
            local_usage = value;
            have_local_usage = true;
        } else if (item_type == 0u && item_tag == 10u) {
            if (value == 1u && have_local_usage && usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                if (local_usage == HID_USAGE_MOUSE) capabilities |= DEVICE_CAP_MOUSE;
                if (local_usage == HID_USAGE_KEYBOARD) capabilities |= DEVICE_CAP_KEYBOARD;
            }
            have_local_usage = false;
        } else if (item_type == 0u) {
            have_local_usage = false;
        }
    }
    return capabilities;
}

static uint32_t classify_capabilities(const uint8_t *descriptor, uint16_t descriptor_len)
{
    if (descriptor == NULL || descriptor_len == 0) return 0;

    // PICO-07 deliberately identifies Mouse/Keyboard by top-level Application
    // collection. Generic buttons/X/Y alone are not enough because a gamepad
    // also exposes them and gamepads are outside this product scope.
    uint32_t capabilities = classify_application_collections(descriptor, descriptor_len);

    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator,
                                    descriptor,
                                    descriptor_len,
                                    HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);
        if ((item.descriptor_item.item_value & 0x01u) != 0) continue;
        if (item.usage_page == HID_USAGE_PAGE_KEYBOARD) {
            capabilities |= DEVICE_CAP_KEYBOARD;
        } else if (item.usage_page == HID_USAGE_PAGE_CONSUMER) {
            capabilities |= DEVICE_CAP_CONSUMER;
        } else if (item.usage_page >= HID_USAGE_PAGE_VENDOR_MIN) {
            capabilities |= DEVICE_CAP_VENDOR_REPORTS;
        }
    }
    return capabilities;
}

static uint8_t qualified_quirk_mask(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != LOGITECH_USB_VID) return 0;
    switch (product_id) {
        case LOGITECH_LIFT_B031:
            return PROFILE_QUIRK_FORWARD;
        case LOGITECH_M650_B02A:
        case LOGITECH_MX_ANYWHERE_3_B025:
        case LOGITECH_MX_ANYWHERE_3S_B037:
            return PROFILE_QUIRK_BACK | PROFILE_QUIRK_FORWARD;
        default:
            return 0;
    }
}

static device_drag_backend_t resolve_backend_for_record(const profile_record_v1_t *record,
                                                        device_source_button_t source,
                                                        device_drag_fix_policy_t policy)
{
    if (record == NULL || !record->valid || policy == DEVICE_DRAG_FIX_OFF) {
        return DEVICE_DRAG_BACKEND_STANDARD;
    }
    const uint8_t quirk_bit = source == DEVICE_SOURCE_BACK
                                ? PROFILE_QUIRK_BACK : PROFILE_QUIRK_FORWARD;
    if (policy == DEVICE_DRAG_FIX_AUTO) {
        return (record->detected_quirks & quirk_bit) != 0
                 ? DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4
                 : DEVICE_DRAG_BACKEND_STANDARD;
    }
    if (policy == DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED) {
        if (record->pnp_valid && record->vendor_id == LOGITECH_USB_VID) {
            return DEVICE_DRAG_BACKEND_PROBE_HIDPP_REPROG_V4;
        }
        return DEVICE_DRAG_BACKEND_UNSUPPORTED;
    }
    return DEVICE_DRAG_BACKEND_STANDARD;
}

static void publish_snapshot(bool connected,
                             bool restored,
                             bool pnp_query_complete)
{
    device_profile_snapshot_t next;
    memset(&next, 0, sizeof(next));

    critical_section_enter_blocking(&g_snapshot_lock);
    next.revision = g_snapshot.revision + 1u;
    critical_section_exit(&g_snapshot_lock);

    next.connected = connected;
    next.storage_ok = g_storage_ok;
    next.profile_restored = restored;
    next.pnp_query_complete = pnp_query_complete;

    if (g_runtime_record.valid) {
        next.identity_addr_type = g_runtime_record.identity_addr_type;
        memcpy(next.identity_addr, g_runtime_record.identity_addr, 6);
        next.bond_index = g_runtime_record.bond_index;
        next.pnp_valid = g_runtime_record.pnp_valid != 0;
        next.vendor_id_source = g_runtime_record.vendor_id_source;
        next.vendor_id = g_runtime_record.vendor_id;
        next.product_id = g_runtime_record.product_id;
        next.product_version = g_runtime_record.product_version;
        next.report_map_fingerprint = g_runtime_record.report_map_fingerprint;
        next.capabilities = g_runtime_record.capabilities;
        next.detected_quirks = g_runtime_record.detected_quirks;
        next.profile_mode = (device_profile_mode_t)g_runtime_record.profile_mode;
        next.drag_fix_back = (device_drag_fix_policy_t)g_runtime_record.drag_fix_back;
        next.drag_fix_forward = (device_drag_fix_policy_t)g_runtime_record.drag_fix_forward;
        next.auto_back_backend = resolve_backend_for_record(&g_runtime_record,
                                                            DEVICE_SOURCE_BACK,
                                                            DEVICE_DRAG_FIX_AUTO);
        next.auto_forward_backend = resolve_backend_for_record(&g_runtime_record,
                                                               DEVICE_SOURCE_FORWARD,
                                                               DEVICE_DRAG_FIX_AUTO);
    } else {
        next.bond_index = -1;
        next.profile_mode = DEVICE_PROFILE_PASSTHROUGH;
        next.drag_fix_back = DEVICE_DRAG_FIX_AUTO;
        next.drag_fix_forward = DEVICE_DRAG_FIX_AUTO;
        next.auto_back_backend = DEVICE_DRAG_BACKEND_STANDARD;
        next.auto_forward_backend = DEVICE_DRAG_BACKEND_STANDARD;
    }

    critical_section_enter_blocking(&g_snapshot_lock);
    memcpy(&g_snapshot, &next, sizeof(g_snapshot));
    critical_section_exit(&g_snapshot_lock);
}

static void update_current_record_metadata(void)
{
    if (g_current_slot < 0 || g_current_slot >= (int)PROFILE_STORE_MAX_RECORDS) return;
    memcpy(&g_store.records[g_current_slot], &g_runtime_record, sizeof(g_runtime_record));
    publish_catalog();
}

static void apply_pnp_id(const uint8_t *value, uint16_t value_len)
{
    if (value == NULL || value_len < 7 || !g_runtime_record.valid) return;

    g_runtime_record.pnp_valid = 1;
    g_runtime_record.vendor_id_source = value[0];
    g_runtime_record.vendor_id = little_endian_read_16(value, 1);
    g_runtime_record.product_id = little_endian_read_16(value, 3);
    g_runtime_record.product_version = little_endian_read_16(value, 5);
    g_runtime_record.detected_quirks = qualified_quirk_mask(g_runtime_record.vendor_id,
                                                            g_runtime_record.product_id);
    update_current_record_metadata();
    if (g_current_slot >= 0) (void)profile_store_commit();

    printf("[PICO-05] PnP vid=%04x pid=%04x ver=%04x quirks=0x%02x\n",
           g_runtime_record.vendor_id,
           g_runtime_record.product_id,
           g_runtime_record.product_version,
           g_runtime_record.detected_quirks);
}

static void pnp_gatt_event_handler(uint8_t packet_type,
                                   uint16_t channel,
                                   uint8_t *packet,
                                   uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || g_connection_handle == HCI_CON_HANDLE_INVALID) return;

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            const uint16_t len = gatt_event_characteristic_value_query_result_get_value_length(packet);
            const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);
            if (len >= 7) {
                g_pnp_value_seen = true;
                apply_pnp_id(value, len);
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE: {
            const uint8_t status = gatt_event_query_complete_get_att_status(packet);
            g_pnp_query_active = false;
            printf("[PICO-05] PnP query complete status=0x%02x found=%u\n",
                   status, g_pnp_value_seen ? 1u : 0u);
            publish_snapshot(true, g_snapshot.profile_restored, true);
            break;
        }
        default:
            break;
    }
}

static void pnp_query_timer_handler(btstack_timer_source_t *timer)
{
    (void)timer;
    if (g_connection_handle == HCI_CON_HANDLE_INVALID || g_pnp_query_active) return;

    if (!gatt_client_is_ready(g_connection_handle)) {
        if (++g_pnp_retry_count < PNP_QUERY_MAX_RETRIES) {
            btstack_run_loop_set_timer(&g_pnp_timer, PNP_QUERY_RETRY_MS);
            btstack_run_loop_add_timer(&g_pnp_timer);
        } else {
            printf("[PICO-05] PnP query timed out waiting for GATT client\n");
            publish_snapshot(true, g_snapshot.profile_restored, true);
        }
        return;
    }

    const uint8_t status = gatt_client_read_value_of_characteristics_by_uuid16(
        pnp_gatt_event_handler, g_connection_handle, 0x0001u, 0xffffu, PNP_ID_UUID);
    if (status == ERROR_CODE_SUCCESS) {
        g_pnp_query_active = true;
        g_pnp_value_seen = false;
        printf("[PICO-05] querying Device Information PnP ID\n");
        return;
    }

    if (++g_pnp_retry_count < PNP_QUERY_MAX_RETRIES) {
        btstack_run_loop_set_timer(&g_pnp_timer, PNP_QUERY_RETRY_MS);
        btstack_run_loop_add_timer(&g_pnp_timer);
    } else {
        printf("[PICO-05] PnP query start failed status=0x%02x\n", status);
        publish_snapshot(true, g_snapshot.profile_restored, true);
    }
}

void device_profile_init(void)
{
    if (g_initialized) return;
    critical_section_init(&g_snapshot_lock);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    memset(&g_catalog, 0, sizeof(g_catalog));
    g_snapshot.bond_index = -1;
    g_snapshot.profile_mode = DEVICE_PROFILE_PASSTHROUGH;
    g_snapshot.drag_fix_back = DEVICE_DRAG_FIX_AUTO;
    g_snapshot.drag_fix_forward = DEVICE_DRAG_FIX_AUTO;
    g_snapshot.auto_back_backend = DEVICE_DRAG_BACKEND_STANDARD;
    g_snapshot.auto_forward_backend = DEVICE_DRAG_BACKEND_STANDARD;
    g_initialized = true;
}

void device_profile_core1_prepare(void)
{
    if (!g_initialized) return;
    profile_store_load();
}

void device_profile_on_hids_ready(hci_con_handle_t connection_handle,
                                  const uint8_t peer_addr[6],
                                  uint8_t peer_addr_type,
                                  const uint8_t *report_descriptor,
                                  uint16_t report_descriptor_len)
{
    if (!g_initialized) return;

    profile_store_load();
    g_connection_handle = connection_handle;
    g_current_slot = -1;
    memset(&g_runtime_record, 0, sizeof(g_runtime_record));

    uint8_t identity_addr[6];
    uint8_t identity_addr_type = peer_addr_type;
    memcpy(identity_addr, peer_addr, 6);
    int bond_index = sm_le_device_index(connection_handle);
    if (bond_index >= 0) {
        int resolved_addr_type = 0;
        bd_addr_t resolved_addr;
        le_device_db_info(bond_index, &resolved_addr_type, resolved_addr, NULL);
        identity_addr_type = (uint8_t)resolved_addr_type;
        memcpy(identity_addr, resolved_addr, 6);
    }

    const uint32_t fingerprint = report_descriptor != NULL && report_descriptor_len > 0
                                   ? fnv1a32(report_descriptor, report_descriptor_len) : 0;
    const uint32_t capabilities = classify_capabilities(report_descriptor,
                                                        report_descriptor_len);

    const int existing = find_record(identity_addr_type, identity_addr);
    const bool restored = existing >= 0;
    if (restored) {
        g_current_slot = existing;
        memcpy(&g_runtime_record, &g_store.records[existing], sizeof(g_runtime_record));
        g_runtime_record.bond_index = (int16_t)bond_index;
        g_runtime_record.report_map_fingerprint = fingerprint;
        g_runtime_record.capabilities = capabilities;
        update_current_record_metadata();
        printf("[PICO-05] restored profile slot=%d mode=%u\n",
               existing, g_runtime_record.profile_mode);
    } else {
        const int free_slot = find_free_record();
        g_runtime_record.valid = 1;
        g_runtime_record.identity_addr_type = identity_addr_type;
        memcpy(g_runtime_record.identity_addr, identity_addr, 6);
        g_runtime_record.bond_index = (int16_t)bond_index;
        g_runtime_record.report_map_fingerprint = fingerprint;
        g_runtime_record.capabilities = capabilities;
        g_runtime_record.profile_mode = DEVICE_PROFILE_PASSTHROUGH;
        g_runtime_record.drag_fix_back = DEVICE_DRAG_FIX_AUTO;
        g_runtime_record.drag_fix_forward = DEVICE_DRAG_FIX_AUTO;
        memset(g_runtime_record.mappings, 0, sizeof(g_runtime_record.mappings));

        if (free_slot >= 0) {
            g_current_slot = free_slot;
            memcpy(&g_store.records[free_slot], &g_runtime_record, sizeof(g_runtime_record));
            (void)profile_store_commit();
            printf("[PICO-05] created passthrough profile slot=%d\n", free_slot);
        } else {
            g_storage_ok = false;
            printf("[PICO-05] profile table full; using volatile passthrough\n");
            publish_catalog();
        }
    }

    printf("[PICO-05] identity bond=%d addr=%s type=%u caps=0x%08lx map=%08lx restored=%u\n",
           bond_index, bd_addr_to_str(identity_addr), identity_addr_type,
           (unsigned long)capabilities, (unsigned long)fingerprint,
           restored ? 1u : 0u);

    publish_snapshot(true, restored, false);
    publish_catalog();

    btstack_run_loop_remove_timer(&g_pnp_timer);
    g_pnp_retry_count = 0;
    g_pnp_query_active = false;
    g_pnp_value_seen = false;
    btstack_run_loop_set_timer_handler(&g_pnp_timer, pnp_query_timer_handler);
    btstack_run_loop_set_timer(&g_pnp_timer, PNP_QUERY_RETRY_MS);
    btstack_run_loop_add_timer(&g_pnp_timer);
}

void device_profile_on_disconnect(void)
{
    if (!g_initialized) return;
    btstack_run_loop_remove_timer(&g_pnp_timer);
    g_connection_handle = HCI_CON_HANDLE_INVALID;
    g_pnp_query_active = false;
    publish_snapshot(false, g_snapshot.profile_restored, g_snapshot.pnp_query_complete);
}

bool device_profile_get_snapshot(device_profile_snapshot_t *snapshot)
{
    if (!g_initialized || snapshot == NULL) return false;
    critical_section_enter_blocking(&g_snapshot_lock);
    memcpy(snapshot, &g_snapshot, sizeof(*snapshot));
    critical_section_exit(&g_snapshot_lock);
    return snapshot->revision != 0;
}

bool device_profile_get_catalog(device_profile_catalog_t *catalog)
{
    if (!g_initialized || catalog == NULL) return false;
    critical_section_enter_blocking(&g_snapshot_lock);
    memcpy(catalog, &g_catalog, sizeof(*catalog));
    critical_section_exit(&g_snapshot_lock);
    return catalog->revision != 0;
}

bool device_profile_delete_saved(uint8_t addr_type,
                                 const uint8_t addr[6],
                                 int16_t *removed_bond_index)
{
    if (!g_initialized || addr == NULL) return false;
    profile_store_load();
    const int slot = find_record(addr_type, addr);
    if (slot < 0) return false;

    const int16_t bond_index = g_store.records[slot].bond_index;
    profile_store_v1_t previous;
    memcpy(&previous, &g_store, sizeof(previous));
    memset(&g_store.records[slot], 0, sizeof(g_store.records[slot]));

    if (!profile_store_commit()) {
        memcpy(&g_store, &previous, sizeof(g_store));
        publish_catalog();
        return false;
    }

    if (g_current_slot == slot || identity_matches(&g_runtime_record, addr_type, addr)) {
        g_current_slot = -1;
        memset(&g_runtime_record, 0, sizeof(g_runtime_record));
    }
    if (removed_bond_index != NULL) *removed_bond_index = bond_index;
    printf("[PICO-07] DeviceRecord deleted addr=%s bond=%d\n",
           bd_addr_to_str(addr), bond_index);
    return true;
}

device_drag_backend_t device_profile_resolve_drag_backend(device_source_button_t source,
                                                          device_drag_fix_policy_t policy)
{
    if (!g_initialized || !g_runtime_record.valid) {
        return policy == DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED
                 ? DEVICE_DRAG_BACKEND_UNSUPPORTED
                 : DEVICE_DRAG_BACKEND_STANDARD;
    }
    return resolve_backend_for_record(&g_runtime_record, source, policy);
}

bool device_profile_forward_hidpp_remap_active(void)
{
    if (!g_initialized || !g_runtime_record.valid) return false;
    if (g_runtime_record.profile_mode != DEVICE_PROFILE_DEFAULT_REMAP) return false;
    return resolve_backend_for_record(&g_runtime_record,
                                      DEVICE_SOURCE_FORWARD,
                                      (device_drag_fix_policy_t)g_runtime_record.drag_fix_forward) ==
           DEVICE_DRAG_BACKEND_HIDPP_REPROG_V4;
}
