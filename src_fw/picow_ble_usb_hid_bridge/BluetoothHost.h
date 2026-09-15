#ifndef BLUETOOTH_HOST_H
#define BLUETOOTH_HOST_H

#include <stdbool.h>
#include <stdint.h>

void BT_HOST_CoreMain(void);
bool BT_HOST_IsReady(void);
const uint8_t *BT_HOST_GetReportDescriptor(void);
uint16_t BT_HOST_GetReportDescriptorLength(void);
const char *BT_HOST_GetTransportName(void);

#endif
