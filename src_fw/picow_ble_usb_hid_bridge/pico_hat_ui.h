#ifndef PICO_HAT_UI_H
#define PICO_HAT_UI_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PICO_HAT_INPUT_JOY_UP = 0,
    PICO_HAT_INPUT_JOY_DOWN,
    PICO_HAT_INPUT_JOY_LEFT,
    PICO_HAT_INPUT_JOY_RIGHT,
    PICO_HAT_INPUT_JOY_PRESS,
    PICO_HAT_INPUT_KEY1,
    PICO_HAT_INPUT_KEY2,
    PICO_HAT_INPUT_KEY3,
    PICO_HAT_INPUT_KEY4,
    PICO_HAT_INPUT_COUNT
} pico_hat_input_t;

typedef struct {
    pico_hat_input_t input;
    bool pressed;
} pico_hat_event_t;

// Configure the Waveshare Pico-LCD-1.3 GPIO/SPI baseline. Initialization is
// non-blocking: LCD reset and wake-up sequencing continues from pico_hat_ui_task().
void pico_hat_ui_init(void);

// Cooperative Core0 task. Call frequently from the existing USB loop.
void pico_hat_ui_task(void);

// Retrieve one debounced local input transition, if available.
bool pico_hat_ui_poll_event(pico_hat_event_t *event);

// Screen lock is volatile by design and resets to false on every boot.
bool pico_hat_ui_is_screen_locked(void);

// True after panel init and the PICO-03 startup test pattern are complete. Later
// gate/product renderers may safely own the framebuffer only after this point.
bool pico_hat_ui_is_lcd_ready(void);

// Human-readable names intended for UART validation logs.
const char *pico_hat_ui_input_name(pico_hat_input_t input);

#endif // PICO_HAT_UI_H
