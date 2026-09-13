#include "boot_debug.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define BOOT_DEBUG_BUFFER_SIZE 8192u
#define BOOT_DEBUG_LINE_SIZE 256u

static critical_section_t g_boot_debug_lock;
static bool g_boot_debug_initialized;
static char g_boot_debug_buffer[BOOT_DEBUG_BUFFER_SIZE];
static uint32_t g_boot_debug_read;
static uint32_t g_boot_debug_write;
static uint32_t g_boot_debug_dropped;

static void enqueue_char(char ch)
{
    const uint32_t next = (g_boot_debug_write + 1u) % BOOT_DEBUG_BUFFER_SIZE;
    if (next == g_boot_debug_read) {
        g_boot_debug_read = (g_boot_debug_read + 1u) % BOOT_DEBUG_BUFFER_SIZE;
        ++g_boot_debug_dropped;
    }
    g_boot_debug_buffer[g_boot_debug_write] = ch;
    g_boot_debug_write = next;
}

void boot_debug_init(void)
{
    if (g_boot_debug_initialized) return;
    critical_section_init(&g_boot_debug_lock);
    g_boot_debug_read = 0u;
    g_boot_debug_write = 0u;
    g_boot_debug_dropped = 0u;
    g_boot_debug_initialized = true;
}

void boot_debug_logf(const char *format, ...)
{
    if (!g_boot_debug_initialized || format == NULL) return;

    char payload[BOOT_DEBUG_LINE_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);

    char line[BOOT_DEBUG_LINE_SIZE + 32u];
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    snprintf(line, sizeof(line), "[%08lu ms] %s\r\n", (unsigned long)now, payload);

    critical_section_enter_blocking(&g_boot_debug_lock);
    for (size_t i = 0; line[i] != '\0'; ++i) enqueue_char(line[i]);
    critical_section_exit(&g_boot_debug_lock);
}

bool boot_debug_terminal_connected(void)
{
    return tud_cdc_connected();
}

void boot_debug_task(void)
{
    if (!g_boot_debug_initialized || !tud_cdc_connected()) return;

    uint8_t chunk[128];
    while (tud_cdc_write_available() > 0u) {
        uint32_t count = 0u;
        critical_section_enter_blocking(&g_boot_debug_lock);
        while (count < sizeof(chunk) && g_boot_debug_read != g_boot_debug_write) {
            chunk[count++] = (uint8_t)g_boot_debug_buffer[g_boot_debug_read];
            g_boot_debug_read = (g_boot_debug_read + 1u) % BOOT_DEBUG_BUFFER_SIZE;
        }
        critical_section_exit(&g_boot_debug_lock);

        if (count == 0u) break;
        const uint32_t written = tud_cdc_write(chunk, count);
        if (written < count) {
            critical_section_enter_blocking(&g_boot_debug_lock);
            for (uint32_t i = written; i < count; ++i) enqueue_char((char)chunk[i]);
            critical_section_exit(&g_boot_debug_lock);
            break;
        }
    }
    tud_cdc_write_flush();
}

void boot_debug_wait_for_terminal(void)
{
    boot_debug_logf("DEBUG CDC enumerating; open /dev/ttyACM* to continue boot");
    while (!tud_cdc_connected()) {
        tud_task();
        boot_debug_task();
        sleep_ms(1);
    }

    boot_debug_logf("DEBUG terminal connected; continuing instrumented boot");
    for (unsigned i = 0; i < 50u; ++i) {
        tud_task();
        boot_debug_task();
        sleep_ms(1);
    }
}
