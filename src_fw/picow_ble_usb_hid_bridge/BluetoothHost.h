#ifndef BLUETOOTH_HOST_H
#define BLUETOOTH_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BluetoothModel.h"

void BT_HOST_CoreMain(void);
bool BT_HOST_IsReady(void);
const uint8_t *BT_HOST_GetReportDescriptor(void);
uint16_t BT_HOST_GetReportDescriptorLength(void);
const char *BT_HOST_GetTransportName(void);

bt_host_state_t BT_HOST_GetState(void);
const char *BT_HOST_GetStateName(void);
const char *BT_HOST_GetDeviceName(void);

size_t BT_HOST_GetDiscoveredDeviceCount(void);
bool BT_HOST_GetDiscoveredDevice(size_t index, bt_host_device_t *out_device);
int BT_HOST_GetSelectedDeviceIndex(void);
bool BT_HOST_SelectDevice(size_t index);
bool BT_HOST_SelectNextDevice(void);
bool BT_HOST_SelectPreviousDevice(void);
bool BT_HOST_ConfirmSelectedDevice(void);
bool BT_HOST_StartDiscovery(void);

bt_host_pairing_info_t BT_HOST_GetPairingInfo(void);
bool BT_HOST_ConfirmPairing(bool accept);
bool BT_HOST_SubmitPasskey(uint32_t passkey);

#endif
