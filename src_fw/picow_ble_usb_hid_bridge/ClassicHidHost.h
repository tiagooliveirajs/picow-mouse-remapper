#ifndef CLASSIC_HID_HOST_H
#define CLASSIC_HID_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BluetoothModel.h"

// Core 1 entry point. All BTstack/CYW43 work stays in the implementation
// translation unit so BTstack and TinyUSB HID types never collide.
void CLASSIC_HID_CoreMain(void);

bool CLASSIC_HID_IsReady(void);
const uint8_t *CLASSIC_HID_GetReportDescriptor(void);
uint16_t CLASSIC_HID_GetReportDescriptorLength(void);

// State/query API consumed by the application/UI without BTstack types.
bt_host_state_t CLASSIC_HID_GetState(void);
const char *CLASSIC_HID_GetStateName(void);
const char *CLASSIC_HID_GetDeviceName(void);

size_t CLASSIC_HID_GetDiscoveredDeviceCount(void);
bool CLASSIC_HID_GetDiscoveredDevice(size_t index, bt_host_device_t *out_device);
int CLASSIC_HID_GetSelectedDeviceIndex(void);
bool CLASSIC_HID_SelectDevice(size_t index);
bool CLASSIC_HID_SelectNextDevice(void);
bool CLASSIC_HID_SelectPreviousDevice(void);
bool CLASSIC_HID_ConfirmSelectedDevice(void);
bool CLASSIC_HID_StartDiscovery(void);

bt_host_pairing_info_t CLASSIC_HID_GetPairingInfo(void);
bool CLASSIC_HID_ConfirmPairing(bool accept);
bool CLASSIC_HID_SubmitPasskey(uint32_t passkey);

#endif
