#include "BluetoothPersistence.h"

#include <string.h>

#include "btstack_tlv.h"

// Application-owned TLV tag: 'RHID' (Remapper HID device).
#define BT_PERSISTENCE_TAG_RHID \
    ((((uint32_t)'R') << 24) | (((uint32_t)'H') << 16) | (((uint32_t)'I') << 8) | (uint32_t)'D')
#define BT_PERSISTENCE_RECORD_VERSION 1u
#define BT_PERSISTENCE_RECORD_SIZE \
    (1u + BT_PERSISTENCE_ADDRESS_LEN + 4u + 1u + BT_HOST_DEVICE_NAME_MAX)

static bool persistence_get_tlv(const btstack_tlv_t **out_impl, void **out_context)
{
    if (out_impl == NULL || out_context == NULL) {
        return false;
    }

    *out_impl = NULL;
    *out_context = NULL;
    btstack_tlv_get_instance(out_impl, out_context);
    return *out_impl != NULL && *out_context != NULL;
}

static bool persistence_address_is_valid(const uint8_t address[BT_PERSISTENCE_ADDRESS_LEN])
{
    bool any_nonzero = false;
    bool any_non_ff = false;
    for (size_t i = 0; i < BT_PERSISTENCE_ADDRESS_LEN; ++i) {
        any_nonzero |= address[i] != 0u;
        any_non_ff |= address[i] != 0xffu;
    }
    return any_nonzero && any_non_ff;
}

static void persistence_encode_record(const bt_persisted_hid_device_t *device,
                                      uint8_t record[BT_PERSISTENCE_RECORD_SIZE])
{
    size_t offset = 0;
    record[offset++] = BT_PERSISTENCE_RECORD_VERSION;

    memcpy(&record[offset], device->address, BT_PERSISTENCE_ADDRESS_LEN);
    offset += BT_PERSISTENCE_ADDRESS_LEN;

    record[offset++] = (uint8_t)(device->class_of_device & 0xffu);
    record[offset++] = (uint8_t)((device->class_of_device >> 8) & 0xffu);
    record[offset++] = (uint8_t)((device->class_of_device >> 16) & 0xffu);
    record[offset++] = (uint8_t)((device->class_of_device >> 24) & 0xffu);

    record[offset++] = (uint8_t)device->kind;

    memset(&record[offset], 0, BT_HOST_DEVICE_NAME_MAX);
    if (device->name[0] != '\0') {
        (void)strncpy((char *)&record[offset], device->name, BT_HOST_DEVICE_NAME_MAX - 1u);
    }
}

static bool persistence_decode_record(const uint8_t record[BT_PERSISTENCE_RECORD_SIZE],
                                      bt_persisted_hid_device_t *out_device)
{
    if (record[0] != BT_PERSISTENCE_RECORD_VERSION) {
        return false;
    }

    size_t offset = 1;
    memset(out_device, 0, sizeof(*out_device));

    memcpy(out_device->address, &record[offset], BT_PERSISTENCE_ADDRESS_LEN);
    offset += BT_PERSISTENCE_ADDRESS_LEN;
    if (!persistence_address_is_valid(out_device->address)) {
        return false;
    }

    out_device->class_of_device =
        ((uint32_t)record[offset]) |
        ((uint32_t)record[offset + 1u] << 8) |
        ((uint32_t)record[offset + 2u] << 16) |
        ((uint32_t)record[offset + 3u] << 24);
    offset += 4u;

    if (record[offset] > (uint8_t)BT_HOST_DEVICE_KIND_OTHER_PERIPHERAL) {
        return false;
    }
    out_device->kind = (bt_host_device_kind_t)record[offset++];

    memcpy(out_device->name, &record[offset], BT_HOST_DEVICE_NAME_MAX);
    out_device->name[BT_HOST_DEVICE_NAME_MAX - 1u] = '\0';
    return true;
}

bool BT_PERSISTENCE_LoadHidDevice(bt_persisted_hid_device_t *out_device)
{
    if (out_device == NULL) {
        return false;
    }

    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    if (!persistence_get_tlv(&tlv, &context) || tlv->get_tag == NULL) {
        return false;
    }

    uint8_t record[BT_PERSISTENCE_RECORD_SIZE];
    const int size = tlv->get_tag(context,
                                  BT_PERSISTENCE_TAG_RHID,
                                  record,
                                  sizeof(record));
    if (size != (int)sizeof(record)) {
        return false;
    }

    return persistence_decode_record(record, out_device);
}

bool BT_PERSISTENCE_StoreHidDevice(const bt_persisted_hid_device_t *device)
{
    if (device == NULL || !persistence_address_is_valid(device->address)) {
        return false;
    }

    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    if (!persistence_get_tlv(&tlv, &context) ||
        tlv->get_tag == NULL ||
        tlv->store_tag == NULL) {
        return false;
    }

    uint8_t record[BT_PERSISTENCE_RECORD_SIZE];
    persistence_encode_record(device, record);

    // Avoid unnecessary flash writes on every successful auto-reconnect.
    uint8_t existing[BT_PERSISTENCE_RECORD_SIZE];
    const int existing_size = tlv->get_tag(context,
                                            BT_PERSISTENCE_TAG_RHID,
                                            existing,
                                            sizeof(existing));
    if (existing_size == (int)sizeof(existing) &&
        memcmp(existing, record, sizeof(record)) == 0) {
        return true;
    }

    return tlv->store_tag(context,
                          BT_PERSISTENCE_TAG_RHID,
                          record,
                          sizeof(record)) == 0;
}

bool BT_PERSISTENCE_ClearHidDevice(void)
{
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    if (!persistence_get_tlv(&tlv, &context) || tlv->delete_tag == NULL) {
        return false;
    }

    tlv->delete_tag(context, BT_PERSISTENCE_TAG_RHID);
    return true;
}
