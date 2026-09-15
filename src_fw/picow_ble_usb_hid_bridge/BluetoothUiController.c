#include "BluetoothUiController.h"

#include <stdio.h>
#include <string.h>

#include "BluetoothHost.h"

void BT_UI_GetSnapshot(bt_ui_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->state = BT_HOST_GetState();
    out_snapshot->ready = BT_HOST_IsReady();
    out_snapshot->device_count = BT_HOST_GetDiscoveredDeviceCount();
    out_snapshot->selected_index = BT_HOST_GetSelectedDeviceIndex();
    out_snapshot->pairing = BT_HOST_GetPairingInfo();
    out_snapshot->reconnect = BT_HOST_GetReconnectInfo();
    out_snapshot->has_remembered_device = BT_HOST_GetRememberedDevice(
        &out_snapshot->remembered_device);

    (void)snprintf(out_snapshot->state_text,
                   sizeof(out_snapshot->state_text),
                   "%s",
                   BT_HOST_GetStateName());
    (void)snprintf(out_snapshot->transport,
                   sizeof(out_snapshot->transport),
                   "%s",
                   BT_HOST_GetTransportName());

    if (out_snapshot->selected_index >= 0) {
        out_snapshot->has_selected_device = BT_HOST_GetDiscoveredDevice(
            (size_t)out_snapshot->selected_index,
            &out_snapshot->selected_device);
    }
}

bool BT_UI_HandleAction(bt_ui_action_t action)
{
    const bt_host_state_t state = BT_HOST_GetState();

    switch (action) {
        case BT_UI_ACTION_UP:
            return state == BT_HOST_STATE_DEVICE_SELECTION
                       ? BT_HOST_SelectPreviousDevice()
                       : false;

        case BT_UI_ACTION_DOWN:
            return state == BT_HOST_STATE_DEVICE_SELECTION
                       ? BT_HOST_SelectNextDevice()
                       : false;

        case BT_UI_ACTION_CONFIRM:
            if (state == BT_HOST_STATE_DEVICE_SELECTION) {
                return BT_HOST_ConfirmSelectedDevice();
            }
            if (state == BT_HOST_STATE_PAIRING) {
                const bt_host_pairing_info_t pairing = BT_HOST_GetPairingInfo();
                if (pairing.method == BT_HOST_PAIRING_LEGACY_PIN ||
                    pairing.method == BT_HOST_PAIRING_NUMERIC_CONFIRMATION) {
                    return BT_HOST_ConfirmPairing(true);
                }
            }
            return false;

        case BT_UI_ACTION_CANCEL:
            if (state == BT_HOST_STATE_PAIRING) {
                const bt_host_pairing_info_t pairing = BT_HOST_GetPairingInfo();
                if (pairing.action_required) {
                    return BT_HOST_ConfirmPairing(false);
                }
            }
            if (state == BT_HOST_STATE_RECONNECTING) {
                return BT_HOST_StartDiscovery();
            }
            return false;

        case BT_UI_ACTION_RESCAN:
            return BT_HOST_StartDiscovery();

        case BT_UI_ACTION_FORGET:
            return BT_HOST_ForgetRememberedDevice();

        default:
            return false;
    }
}

bool BT_UI_SubmitPasskey(uint32_t passkey)
{
    return BT_HOST_SubmitPasskey(passkey);
}
