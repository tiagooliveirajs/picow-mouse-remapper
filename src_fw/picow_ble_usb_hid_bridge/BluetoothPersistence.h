#ifndef BLUETOOTH_PERSISTENCE_H
#define BLUETOOTH_PERSISTENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "BluetoothModel.h"

#define BT_PERSISTENCE_ADDRESS_LEN 6

typedef struct {
    uint8_t address[BT_PERSISTENCE_ADDRESS_LEN];
    uint32_t class_of_device;
    bt_host_device_kind_t kind;
    char name[BT_HOST_DEVICE_NAME_MAX];
} bt_persisted_hid_device_t;

// These functions use the Pico SDK/BTstack global TLV instance. They must be
// called only after cyw43_arch_init() has initialized BTstack's flash storage.
bool BT_PERSISTENCE_LoadHidDevice(bt_persisted_hid_device_t *out_device);
bool BT_PERSISTENCE_StoreHidDevice(const bt_persisted_hid_device_t *device);
bool BT_PERSISTENCE_ClearHidDevice(void);

#endif
