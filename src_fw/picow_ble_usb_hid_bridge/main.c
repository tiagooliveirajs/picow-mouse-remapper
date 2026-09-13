/*
 * The MIT License (MIT)
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 */

#include "bsp/board_api.h"
#include "tusb.h"

#include "Common.h"
#include "boot_debug.h"
#include "canonical_hid.h"
#include "classic_keyboard.h"
#include "device_profile.h"
#include "keyboard_hid_queue.h"
#include "logitech_hidpp.h"
#include "pico06_ui.h"
#include "pico_hat_ui.h"
#include "remap_engine.h"
#include "remap_profile.h"
#include "remote_hid_queue.h"

#define LED_BLINKING_INTERVAL 200u
#define PICO06_PENDING_OUTPUTS 4u
#define PICO06_CANON_OUTPUTS 2u
#define PICO08_CORE1_STACK_SIZE_BYTES (8u * 1024u)
#define PICO08_BT_START_DELAY_MS 500u

static uint32_t g_pico08_core1_stack[PICO08_CORE1_STACK_SIZE_BYTES / sizeof(uint32_t)]
    __attribute__((aligned(16)));
static bool g_bluetooth_core_started;
static uint32_t g_lcd_ready_since_ms;

volatile bool g_usb_reinit_request = false;

void usb_dev_main(void);
void hid_task(void);
void led_blinking_task(void);
bool send_hid_report(void);

extern bool is_ble_app_state_ready(void);
extern const uint8_t *get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);
extern void ble_host_main(void);

static void maybe_start_bluetooth_core(void)
{
    if (g_bluetooth_core_started) return;

    if (!pico_hat_ui_is_lcd_ready()) {
        g_lcd_ready_since_ms = 0u;
        return;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (g_lcd_ready_since_ms == 0u) {
        g_lcd_ready_since_ms = now_ms;
        boot_debug_logf("BT gate: LCD reports ready; holding 500 ms before Core1 launch");
        return;
    }
    if ((uint32_t)(now_ms - g_lcd_ready_since_ms) < PICO08_BT_START_DELAY_MS) return;

    boot_debug_logf("BOOT 20 launching BLE+Classic Core1 with %u-byte dedicated stack",
                    (unsigned)sizeof(g_pico08_core1_stack));
    boot_debug_task();
    printf("[PICO-08] starting BLE+Classic Core1 with %u-byte dedicated stack\r\n",
           (unsigned)sizeof(g_pico08_core1_stack));
    g_bluetooth_core_started = true;
    multicore_launch_core1_with_stack(ble_host_main,
                                      g_pico08_core1_stack,
                                      sizeof(g_pico08_core1_stack));
    boot_debug_logf("BOOT 21 Core1 launch call returned on Core0");
}

int main(void)
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) board_init_after_tusb();

    boot_debug_init();
    boot_debug_logf("BOOT 00 board_init + TinyUSB initialized");
    boot_debug_logf("DEBUG BUILD: PID 0x40D0 CDC+HID; firmware waits for CDC terminal");
    boot_debug_wait_for_terminal();

    boot_debug_logf("BOOT 01 stdio_init_all begin");
    stdio_init_all();
    boot_debug_logf("BOOT 02 stdio_init_all complete");

    boot_debug_logf("BOOT 03 CMN_Init begin");
    CMN_Init();
    boot_debug_logf("BOOT 04 CMN_Init complete");

    boot_debug_logf("BOOT 05 canonical_hid_init begin");
    canonical_hid_init();
    boot_debug_logf("BOOT 06 canonical_hid_init complete");

    boot_debug_logf("BOOT 07 remote_hid_queue_init begin");
    remote_hid_queue_init();
    boot_debug_logf("BOOT 08 remote_hid_queue_init complete");

    boot_debug_logf("BOOT 09 keyboard_hid_queue_init begin");
    keyboard_hid_queue_init();
    boot_debug_logf("BOOT 10 keyboard_hid_queue_init complete");

    boot_debug_logf("BOOT 11 classic_keyboard_shared_init begin");
    classic_keyboard_shared_init();
    boot_debug_logf("BOOT 12 classic_keyboard_shared_init complete");

    boot_debug_logf("BOOT 13 persistent profile init begin");
    device_profile_init();
    remap_profile_init();
    boot_debug_logf("BOOT 14 persistent profile init complete");

    boot_debug_logf("BOOT 15 remap/HID++ init begin");
    logitech_hidpp_init();
    remap_engine_init();
    boot_debug_logf("BOOT 16 remap/HID++ init complete");

    boot_debug_logf("BOOT 17 pico_hat_ui_init begin");
    pico_hat_ui_init();
    boot_debug_logf("BOOT 18 pico_hat_ui_init complete");

    boot_debug_logf("BOOT 19 pico06_ui_init begin");
    pico06_ui_init();
    boot_debug_logf("BOOT 19A pico06_ui_init complete");

    flash_safe_execute_core_init();
    boot_debug_logf("BOOT 19B flash_safe_execute_core_init complete");
    g_bluetooth_core_started = false;
    g_lcd_ready_since_ms = 0u;

    boot_debug_logf("BOOT 19C entering Core0 service loop");
    boot_debug_task();
    usb_dev_main();
    return 0;
}

void usb_dev_main(void)
{
    while (1) {
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            boot_debug_logf("legacy USB re-enumeration request ignored");
            printf("[PICO-08] ignored legacy USB re-enumeration request\r\n");
        }

        tud_task();
        boot_debug_task();
        hid_task();
        pico_hat_ui_task();
        pico06_ui_task();
        maybe_start_bluetooth_core();
        led_blinking_task();
        boot_debug_task();
    }
}

void tud_mount_cb(void) {}
void tud_umount_cb(void) {}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {}

bool send_hid_report(void)
{
    static ST_HID_RPT keyboard_report;
    static ST_HID_RPT remote_report;
    static ST_HID_RPT canonical[PICO06_CANON_OUTPUTS];
    static ST_HID_RPT pending[PICO06_PENDING_OUTPUTS];
    static uint8_t pending_count;
    static uint8_t pending_index;
    static bool readiness_initialized;
    static bool previous_ble_ready;

    const bool ble_ready = is_ble_app_state_ready();
    if (!readiness_initialized) {
        readiness_initialized = true;
        previous_ble_ready = ble_ready;
    }

    if (previous_ble_ready && !ble_ready) {
        pending_index = 0;
        pending_count = (uint8_t)remap_engine_neutralize(pending,
                                                         PICO06_PENDING_OUTPUTS);
        remote_hid_queue_clear();
        canonical_hid_reset_device();
        remap_engine_reset_device();
        boot_debug_logf("BLE mouse disconnect -> neutral mouse outputs");
        printf("[PICO-08] BLE mouse disconnect -> neutral mouse outputs\r\n");
    }
    previous_ble_ready = ble_ready;

    if (keyboard_hid_queue_peek(&keyboard_report)) {
        if (tud_suspended()) {
            tud_remote_wakeup();
            return false;
        }
        if (!tud_hid_ready()) return false;
        if (!tud_hid_report(keyboard_report.report_id,
                            keyboard_report.report,
                            keyboard_report.report_len)) {
            return false;
        }
        keyboard_hid_queue_advance();
        return true;
    }

    if (pending_index >= pending_count) {
        pending_index = 0;
        pending_count = (uint8_t)remap_engine_poll_async(pending,
                                                         PICO06_PENDING_OUTPUTS);
    }

    if (pending_index >= pending_count && remote_hid_queue_peek(&remote_report)) {
        const size_t canonical_count = canonical_hid_process_remote_report(
            &remote_report,
            get_ble_hid_report_descriptor_data(),
            get_ble_hid_report_descriptor_len(),
            canonical,
            PICO06_CANON_OUTPUTS);
        remote_hid_queue_advance();

        pending_index = 0;
        pending_count = 0;
        for (size_t i = 0; i < canonical_count &&
                           pending_count < PICO06_PENDING_OUTPUTS; ++i) {
            pending_count += (uint8_t)remap_engine_process_canonical(
                &canonical[i],
                &pending[pending_count],
                PICO06_PENDING_OUTPUTS - pending_count);
        }
    }

    if (pending_index >= pending_count) return false;

    if (tud_suspended()) {
        tud_remote_wakeup();
        return false;
    }
    if (!tud_hid_ready()) return false;

    ST_HID_RPT *output = &pending[pending_index];
    if (!tud_hid_report(output->report_id, output->report, output->report_len)) {
        return false;
    }

    ++pending_index;
    return true;
}

void hid_task(void)
{
    (void)send_hid_report();
}

void tud_hid_report_complete_cb(uint8_t instance,
                                uint8_t const *report,
                                uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void led_blinking_task(void)
{
    static uint32_t start_ms;
    static bool led_state;

    if (board_millis() - start_ms < LED_BLINKING_INTERVAL) return;
    start_ms += LED_BLINKING_INTERVAL;
    board_led_write(led_state);
    led_state = !led_state;
}
