#ifndef LOGITECH_HIDPP_H
#define LOGITECH_HIDPP_H

#include <stdbool.h>
#include <stdint.h>

#define LOGITECH_HIDPP_SOURCE_BACK    (1u << 0)
#define LOGITECH_HIDPP_SOURCE_FORWARD (1u << 1)

typedef struct {
    uint32_t revision;
    bool connected;
    bool feature_available;
    uint8_t feature_index;
    uint8_t desired_mask;
    uint8_t applied_mask;
    uint8_t held_mask;
    uint8_t supported_mask;
    uint8_t failed_mask;
} logitech_hidpp_snapshot_t;

void logitech_hidpp_init(void);

// Core1 lifecycle/task. The task derives desired diversion from the applied
// PICO-06 remap profile and PICO-05 source-specific backend resolver.
void logitech_hidpp_on_connect(void);
void logitech_hidpp_on_disconnect(void);
void logitech_hidpp_core1_task(void);

// Called for every normalized raw HOGP report on Core1. Returns true when the
// report is a HID++ control/event packet consumed by this backend.
bool logitech_hidpp_process_report(uint8_t report_id,
                                   const uint8_t *report,
                                   uint16_t report_len);

bool logitech_hidpp_get_snapshot(logitech_hidpp_snapshot_t *snapshot);

#endif // LOGITECH_HIDPP_H
