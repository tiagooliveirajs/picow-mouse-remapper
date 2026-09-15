#ifndef CLASSIC_HID_HOST_H
#define CLASSIC_HID_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLASSIC_HID_STATE_BOOTING = 0,
    CLASSIC_HID_STATE_INQUIRY,
    CLASSIC_HID_STATE_RESOLVING_NAMES,
    CLASSIC_HID_STATE_CONNECTING,
    CLASSIC_HID_STATE_PAIRING,
    CLASSIC_HID_STATE_CONNECTED,
    CLASSIC_HID_STATE_READY,
    CLASSIC_HID_STATE_ERROR,
} classic_hid_state_t;

// Core 1 entry point. All BTstack/CYW43 work stays in the implementation
// translation unit so BTstack and TinyUSB HID types never collide.
void CLASSIC_HID_CoreMain(void);

bool CLASSIC_HID_IsReady(void);
const uint8_t *CLASSIC_HID_GetReportDescriptor(void);
uint16_t CLASSIC_HID_GetReportDescriptorLength(void);

// State accessors intentionally avoid BTstack types so the future display/UI
// can consume them from a TinyUSB/LCD translation unit safely.
classic_hid_state_t CLASSIC_HID_GetState(void);
const char *CLASSIC_HID_GetStateName(void);
const char *CLASSIC_HID_GetDeviceName(void);
bool CLASSIC_HID_HasPairingPasskey(void);
uint32_t CLASSIC_HID_GetPairingPasskey(void);

#endif
