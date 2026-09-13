#ifndef REMAP_PROFILE_H
#define REMAP_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "device_profile.h"

#define REMAP_SOURCE_COUNT 5u

typedef enum {
    REMAP_SOURCE_LEFT = 0,
    REMAP_SOURCE_RIGHT,
    REMAP_SOURCE_MIDDLE,
    REMAP_SOURCE_BACK,
    REMAP_SOURCE_FORWARD,
} remap_source_t;

typedef enum {
    REMAP_TARGET_PASSTHROUGH = 0,
    REMAP_TARGET_LEFT = 1,
    REMAP_TARGET_RIGHT = 2,
    REMAP_TARGET_MIDDLE = 3,
    REMAP_TARGET_BACK = 4,
    REMAP_TARGET_FORWARD = 5,
} remap_target_t;

typedef struct {
    device_profile_mode_t mode;
    uint8_t mappings[REMAP_SOURCE_COUNT]; // target enum per physical source
    device_drag_fix_policy_t drag_fix_back;
    device_drag_fix_policy_t drag_fix_forward;
} remap_profile_config_t;

typedef struct {
    uint32_t revision;
    bool connected;
    bool storage_ok;
    bool restored;
    uint8_t identity_addr_type;
    uint8_t identity_addr[6];
    remap_profile_config_t active;
    uint32_t last_apply_id;
    bool last_apply_ok;
} remap_profile_snapshot_t;

void remap_profile_init(void);

// Core1 task. It follows the identity published by device_profile, restores the
// matching saved remap, and performs requested TLV commits away from Core0 USB.
void remap_profile_core1_task(void);

bool remap_profile_get_snapshot(remap_profile_snapshot_t *snapshot);

// Non-blocking Core0 request. The returned id can be observed in last_apply_id.
bool remap_profile_request_apply(const remap_profile_config_t *config,
                                 uint32_t *request_id);

void remap_profile_make_passthrough(remap_profile_config_t *config);
void remap_profile_make_default(remap_profile_config_t *config);

const char *remap_profile_mode_name(device_profile_mode_t mode);
const char *remap_source_name(remap_source_t source);
const char *remap_target_name(remap_target_t target);

#endif // REMAP_PROFILE_H
