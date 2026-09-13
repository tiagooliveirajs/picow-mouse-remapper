#ifndef PICO05_STATUS_H
#define PICO05_STATUS_H

// Cooperative, read-only PICO-05 gate screen. It renders the current device
// identity/profile snapshot in the product accessibility baseline: black
// background and white text. Call frequently from Core0 after pico_hat_ui_task.
void pico05_status_init(void);
void pico05_status_task(void);

#endif // PICO05_STATUS_H
