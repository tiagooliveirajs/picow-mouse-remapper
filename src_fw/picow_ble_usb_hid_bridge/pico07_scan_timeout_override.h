#pragma once

// PICO-07 physical pairing validation override.
//
// The BTstack HOGP transport currently owns a 5,000 ms discovery timer in
// hog_host_demo.c. Some keyboards take longer to wake and begin advertising
// after the user puts them into pairing mode. For this validation candidate,
// extend only that 5 s timer by exactly 10x while leaving the 8 s connection
// timeout and all other timers unchanged.

#include <stdint.h>
#include "btstack.h"

static inline void pico07_btstack_set_timer_with_long_scan(
    btstack_timer_source_t *timer,
    uint32_t timeout_ms)
{
    if (timeout_ms == 5000u) {
        timeout_ms = 50000u;
    }
    btstack_run_loop_set_timer(timer, timeout_ms);
}

#define btstack_run_loop_set_timer(timer, timeout_ms) \
    pico07_btstack_set_timer_with_long_scan((timer), (timeout_ms))
