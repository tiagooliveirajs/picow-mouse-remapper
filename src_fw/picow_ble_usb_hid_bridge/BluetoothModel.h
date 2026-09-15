#ifndef BLUETOOTH_MODEL_H
#define BLUETOOTH_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_HOST_MAX_DEVICES 20
#define BT_HOST_DEVICE_NAME_MAX 64
#define BT_HOST_ADDRESS_TEXT_MAX 18
#define BT_HOST_LEGACY_PIN_MAX 17

typedef enum {
    BT_HOST_STATE_BOOTING = 0,
    BT_HOST_STATE_DISCOVERING,
    BT_HOST_STATE_RESOLVING_NAMES,
    BT_HOST_STATE_DEVICE_SELECTION,
    BT_HOST_STATE_CONNECTING,
    BT_HOST_STATE_PAIRING,
    BT_HOST_STATE_CONNECTED,
    BT_HOST_STATE_READY,
    BT_HOST_STATE_ERROR,
} bt_host_state_t;

typedef enum {
    BT_HOST_DEVICE_KIND_UNKNOWN = 0,
    BT_HOST_DEVICE_KIND_KEYBOARD,
    BT_HOST_DEVICE_KIND_MOUSE,
    BT_HOST_DEVICE_KIND_KEYBOARD_MOUSE,
    BT_HOST_DEVICE_KIND_OTHER_PERIPHERAL,
} bt_host_device_kind_t;

typedef enum {
    BT_HOST_PAIRING_NONE = 0,
    BT_HOST_PAIRING_LEGACY_PIN,
    BT_HOST_PAIRING_NUMERIC_CONFIRMATION,
    BT_HOST_PAIRING_PASSKEY_DISPLAY,
    BT_HOST_PAIRING_PASSKEY_INPUT,
} bt_host_pairing_method_t;

typedef struct {
    char name[BT_HOST_DEVICE_NAME_MAX];
    char address[BT_HOST_ADDRESS_TEXT_MAX];
    uint32_t class_of_device;
    int8_t rssi;
    bt_host_device_kind_t kind;
    bool hid_candidate;
    bool name_resolved;
} bt_host_device_t;

typedef struct {
    bt_host_pairing_method_t method;
    uint32_t numeric_value;
    char legacy_pin[BT_HOST_LEGACY_PIN_MAX];
    bool action_required;
} bt_host_pairing_info_t;

#endif
