#include "BluetoothHost.h"

#include <string.h>

#include "ClassicHidHost.h"

#ifndef APP_BT_DEFAULT_CLASSIC
#define APP_BT_DEFAULT_CLASSIC 1
#endif

// Existing BLE implementation retained for future transport selection.
// These declarations deliberately avoid BTstack headers here.
extern void ble_host_main(void);
extern bool is_ble_app_state_ready(void);
extern const uint8_t *get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);

void BT_HOST_CoreMain(void)
{
#if APP_BT_DEFAULT_CLASSIC
    CLASSIC_HID_CoreMain();
#else
    ble_host_main();
#endif
}

bool BT_HOST_IsReady(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_IsReady();
#else
    return is_ble_app_state_ready();
#endif
}

const uint8_t *BT_HOST_GetReportDescriptor(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetReportDescriptor();
#else
    return get_ble_hid_report_descriptor_data();
#endif
}

uint16_t BT_HOST_GetReportDescriptorLength(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetReportDescriptorLength();
#else
    return get_ble_hid_report_descriptor_len();
#endif
}

const char *BT_HOST_GetTransportName(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return "Bluetooth Classic HID";
#else
    return "Bluetooth LE HOG";
#endif
}

bt_host_state_t BT_HOST_GetState(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetState();
#else
    return is_ble_app_state_ready() ? BT_HOST_STATE_READY : BT_HOST_STATE_DISCOVERING;
#endif
}

const char *BT_HOST_GetStateName(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetStateName();
#else
    return is_ble_app_state_ready() ? "READY" : "DISCOVERING";
#endif
}

const char *BT_HOST_GetDeviceName(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetDeviceName();
#else
    return "BLE HID device";
#endif
}

size_t BT_HOST_GetDiscoveredDeviceCount(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetDiscoveredDeviceCount();
#else
    return 0;
#endif
}

bool BT_HOST_GetDiscoveredDevice(size_t index, bt_host_device_t *out_device)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetDiscoveredDevice(index, out_device);
#else
    (void)index;
    if (out_device != NULL) {
        memset(out_device, 0, sizeof(*out_device));
    }
    return false;
#endif
}

int BT_HOST_GetSelectedDeviceIndex(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetSelectedDeviceIndex();
#else
    return -1;
#endif
}

bool BT_HOST_SelectDevice(size_t index)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_SelectDevice(index);
#else
    (void)index;
    return false;
#endif
}

bool BT_HOST_SelectNextDevice(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_SelectNextDevice();
#else
    return false;
#endif
}

bool BT_HOST_SelectPreviousDevice(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_SelectPreviousDevice();
#else
    return false;
#endif
}

bool BT_HOST_ConfirmSelectedDevice(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_ConfirmSelectedDevice();
#else
    return false;
#endif
}

bool BT_HOST_StartDiscovery(void)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_StartDiscovery();
#else
    return false;
#endif
}

bt_host_pairing_info_t BT_HOST_GetPairingInfo(void)
{
    bt_host_pairing_info_t info = {0};
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_GetPairingInfo();
#else
    return info;
#endif
}

bool BT_HOST_ConfirmPairing(bool accept)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_ConfirmPairing(accept);
#else
    (void)accept;
    return false;
#endif
}

bool BT_HOST_SubmitPasskey(uint32_t passkey)
{
#if APP_BT_DEFAULT_CLASSIC
    return CLASSIC_HID_SubmitPasskey(passkey);
#else
    (void)passkey;
    return false;
#endif
}
