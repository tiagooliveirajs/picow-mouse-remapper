#include "keyboard_hid_queue.h"

#include <string.h>

#include "pico/stdlib.h"

#define KEYBOARD_HID_QUEUE_CAPACITY 32u

static ST_HID_RPT g_reports[KEYBOARD_HID_QUEUE_CAPACITY];
static uint8_t g_head;
static uint8_t g_tail;
static critical_section_t g_lock;
static bool g_initialized;

void keyboard_hid_queue_init(void)
{
    if (g_initialized) return;
    critical_section_init(&g_lock);
    g_head = 0u;
    g_tail = 0u;
    memset(g_reports, 0, sizeof(g_reports));
    g_initialized = true;
}

bool keyboard_hid_queue_enqueue(const ST_HID_RPT *report)
{
    if (!g_initialized || report == NULL) return false;

    bool ok = false;
    critical_section_enter_blocking(&g_lock);
    const uint8_t next_tail = (uint8_t)((g_tail + 1u) % KEYBOARD_HID_QUEUE_CAPACITY);
    if (next_tail != g_head) {
        memcpy(&g_reports[g_tail], report, sizeof(*report));
        g_tail = next_tail;
        ok = true;
    }
    critical_section_exit(&g_lock);
    return ok;
}

bool keyboard_hid_queue_peek(ST_HID_RPT *report)
{
    if (!g_initialized || report == NULL) return false;

    bool ok = false;
    critical_section_enter_blocking(&g_lock);
    if (g_head != g_tail) {
        memcpy(report, &g_reports[g_head], sizeof(*report));
        ok = true;
    }
    critical_section_exit(&g_lock);
    return ok;
}

void keyboard_hid_queue_advance(void)
{
    if (!g_initialized) return;
    critical_section_enter_blocking(&g_lock);
    if (g_head != g_tail) {
        g_head = (uint8_t)((g_head + 1u) % KEYBOARD_HID_QUEUE_CAPACITY);
    }
    critical_section_exit(&g_lock);
}

void keyboard_hid_queue_clear(void)
{
    if (!g_initialized) return;
    critical_section_enter_blocking(&g_lock);
    g_head = 0u;
    g_tail = 0u;
    critical_section_exit(&g_lock);
}
