#ifndef CLASSIC_KEYBOARD_H
#define CLASSIC_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define CLASSIC_KEYBOARD_MESSAGE_MAX 21u

typedef enum {
    CLASSIC_KEYBOARD_IDLE = 0,
    CLASSIC_KEYBOARD_SCANNING,
    CLASSIC_KEYBOARD_CONNECTING,
    CLASSIC_KEYBOARD_READY,
    CLASSIC_KEYBOARD_ERROR,
} classic_keyboard_state_t;

typedef struct {
    uint32_t revision;
    classic_keyboard_state_t state;
    bool connected;
    bool saved_peer;
    char message[CLASSIC_KEYBOARD_MESSAGE_MAX + 1u];
} classic_keyboard_snapshot_t;

// Initialize cross-core state before Core1 is launched.
void classic_keyboard_shared_init(void);

// Called by the BTstack/Core1 translation unit after l2cap/sm init and before
// hci_power_control(HCI_POWER_ON).
void classic_keyboard_core1_init(void);

// Core0 UI requests. Actual BTstack work is executed on Core1.
bool classic_keyboard_request_pair(void);
bool classic_keyboard_request_cancel(void);
bool classic_keyboard_get_snapshot(classic_keyboard_snapshot_t *snapshot);

#endif // CLASSIC_KEYBOARD_H
