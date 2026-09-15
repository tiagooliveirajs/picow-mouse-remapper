#ifndef BLUETOOTH_UI_CONTROLLER_H
#define BLUETOOTH_UI_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BluetoothModel.h"

typedef enum {
    BT_UI_ACTION_UP = 0,
    BT_UI_ACTION_DOWN,
    BT_UI_ACTION_CONFIRM,
    BT_UI_ACTION_CANCEL,
    BT_UI_ACTION_RESCAN,
} bt_ui_action_t;

typedef struct {
    bt_host_state_t state;
    char state_text[24];
    char transport[32];
    bool ready;

    size_t device_count;
    int selected_index;
    bool has_selected_device;
    bt_host_device_t selected_device;

    bt_host_pairing_info_t pairing;
} bt_ui_snapshot_t;

void BT_UI_GetSnapshot(bt_ui_snapshot_t *out_snapshot);
bool BT_UI_HandleAction(bt_ui_action_t action);
bool BT_UI_SubmitPasskey(uint32_t passkey);

#endif
