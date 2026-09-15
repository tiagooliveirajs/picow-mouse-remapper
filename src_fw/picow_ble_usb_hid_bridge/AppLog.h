#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the diagnostic logging layer. The Pico SDK UART stdio remains
// available as a fallback, while the TinyUSB CDC interface provides a
// /dev/ttyACM* console on the USB host.
void APP_LOG_Init(void);

// Emit an informational diagnostic line. CDC output is intentionally limited
// to Core 0 because TinyUSB device processing runs on Core 0.
void APP_LOG_Info(const char *format, ...);

// Service the CDC diagnostic interface from the Core 0 USB loop.
void APP_LOG_Task(void);

// Print firmware identity and the active Gate 01 diagnostic configuration.
void APP_LOG_PrintBootBanner(void);

#ifdef __cplusplus
}
#endif

#endif // APP_LOG_H
