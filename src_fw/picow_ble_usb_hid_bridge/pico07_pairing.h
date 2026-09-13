#ifndef PICO07_PAIRING_H
#define PICO07_PAIRING_H

#include <stdbool.h>
#include <stdint.h>

#define PICO07_MAX_SCAN_RESULTS 6u
#define PICO07_DEVICE_NAME_MAX  14u

typedef enum {
    PICO07_PAIR_IDLE = 0,
    PICO07_PAIR_SCANNING,
    PICO07_PAIR_RESULTS,
    PICO07_PAIR_CONNECTING,
    PICO07_PAIR_CLASSIFYING,
    PICO07_PAIR_SUCCESS,
    PICO07_PAIR_ERROR,
} pico07_pair_state_t;

typedef enum {
    PICO07_TYPE_UNKNOWN = 0,
    PICO07_TYPE_MOUSE,
    PICO07_TYPE_KEYBOARD,
    PICO07_TYPE_COMPOSITE,
    PICO07_TYPE_UNSUPPORTED,
} pico07_device_type_t;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t rssi;
    char name[PICO07_DEVICE_NAME_MAX + 1u];
} pico07_scan_result_t;

typedef struct {
    uint32_t revision;
    pico07_pair_state_t state;
    uint8_t result_count;
    pico07_scan_result_t results[PICO07_MAX_SCAN_RESULTS];
    pico07_device_type_t last_type;
    bool last_was_new_pair;
    char message[22]; // 21 visible UI characters + NUL
} pico07_pairing_snapshot_t;

void pico07_pairing_init(void);

// Core0 UI requests. BTstack work is executed by Core1.
bool pico07_pairing_request_scan(void);
bool pico07_pairing_request_connect_result(uint8_t result_index);
bool pico07_pairing_request_connect_saved(uint8_t addr_type, const uint8_t addr[6]);
bool pico07_pairing_request_delete_saved(uint8_t addr_type, const uint8_t addr[6]);
bool pico07_pairing_request_cancel(void);

bool pico07_pairing_get_snapshot(pico07_pairing_snapshot_t *snapshot);
const char *pico07_device_type_name(pico07_device_type_t type);

#endif // PICO07_PAIRING_H
