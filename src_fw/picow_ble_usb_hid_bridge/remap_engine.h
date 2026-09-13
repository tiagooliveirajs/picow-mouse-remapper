#ifndef REMAP_ENGINE_H
#define REMAP_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

#include "Common.h"
#include "remap_profile.h"

void remap_engine_init(void);
void remap_engine_reset_device(void);

// Convert one canonical firmware-owned mouse report into remapped USB reports.
size_t remap_engine_process_canonical(const ST_HID_RPT *input,
                                      ST_HID_RPT *outputs,
                                      size_t output_capacity);

// Emit zero-motion state changes caused by profile Apply or HID++ held-state.
size_t remap_engine_poll_async(ST_HID_RPT *outputs, size_t output_capacity);

// Force released mouse/keyboard outputs before disconnect/state boundaries.
size_t remap_engine_neutralize(ST_HID_RPT *outputs, size_t output_capacity);

// Physical source DOWN events for Custom capture. Movement/wheel never enter it.
bool remap_engine_poll_source_down(remap_source_t *source);

#endif // REMAP_ENGINE_H
