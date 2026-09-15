#include "BluetoothHost.h"

#include "ClassicHidHost.h"

#ifndef APP_BT_DEFAULT_CLASSIC
#define APP_BT_DEFAULT_CLASSIC 1
#endif

// Existing BLE implementation retained for the future transport-selection UI.
// These declarations deliberately avoid BTstack headers in this translation unit.
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
