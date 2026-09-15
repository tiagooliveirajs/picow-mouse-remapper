#include "AppLog.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#ifndef APP_FIRMWARE_VERSION
#define APP_FIRMWARE_VERSION "dev"
#endif

#ifndef APP_GATE_NAME
#define APP_GATE_NAME "DEV"
#endif

#define APP_LOG_LINE_MAX 256
#define APP_LOG_CDC_RX_SCRATCH 64

static void app_log_write_cdc_line(const char *line)
{
    // TinyUSB is serviced by Core 0. Never access it from the Bluetooth core.
    if (get_core_num() != 0) {
        return;
    }

    if (!tud_cdc_connected()) {
        return;
    }

    tud_cdc_write(line, strlen(line));
    tud_cdc_write_str("\r\n");
    tud_cdc_write_flush();
}

void APP_LOG_Init(void)
{
    APP_LOG_Info("diagnostics initialized");
}

void APP_LOG_Info(const char *format, ...)
{
    char message[APP_LOG_LINE_MAX];
    char line[APP_LOG_LINE_MAX + 64];

    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const unsigned int core = (unsigned int)get_core_num();

    (void)snprintf(
        line,
        sizeof(line),
        "[%010lu ms][%s][C%u] %s",
        (unsigned long)now_ms,
        APP_GATE_NAME,
        core,
        message);

    // UART remains a fallback. CDC is intentionally serviced only on Core 0.
    printf("%s\n", line);
    app_log_write_cdc_line(line);
}

void APP_LOG_Task(void)
{
    if (get_core_num() != 0) {
        return;
    }

    uint8_t scratch[APP_LOG_CDC_RX_SCRATCH];
    while (tud_cdc_available()) {
        const uint32_t available = tud_cdc_available();
        const uint32_t to_read = available > sizeof(scratch) ? sizeof(scratch) : available;
        (void)tud_cdc_read(scratch, to_read);
    }

    tud_cdc_write_flush();
}

void APP_LOG_PrintBootBanner(void)
{
    APP_LOG_Info("Pico 2 W HID Remapper firmware %s", APP_FIRMWARE_VERSION);
    APP_LOG_Info("Bluetooth Classic HID integration active");
    APP_LOG_Info("USB interfaces: HID + CDC diagnostics");
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;

    if (itf != 0 || !dtr) {
        return;
    }

    APP_LOG_Info("CDC console opened");
    APP_LOG_PrintBootBanner();
}
