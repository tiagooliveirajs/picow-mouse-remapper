#ifndef PICO_HAT_DIAG_H
#define PICO_HAT_DIAG_H

#include "pico_hat_ui.h"

// PICO-03-only visual diagnostic. Accepted local input events are mirrored to
// the LCD so hardware validation does not require a UART adapter.
void pico_hat_diag_handle_event(const pico_hat_event_t *event);

#endif // PICO_HAT_DIAG_H
