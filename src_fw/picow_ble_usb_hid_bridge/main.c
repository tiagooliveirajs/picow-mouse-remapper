/*
 * The MIT License (MIT)
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 */

#include "bsp/board_api.h"
#include "tusb.h"

#include "Common.h"
#include "canonical_hid.h"
#include "device_profile.h"
#include "logitech_hidpp.h"
#include "pico06_ui.h"
#include "pico_hat_ui.h"
#include "remap_engine.h"
#include "remap_profile.h"
#include "remote_hid_queue.h"

#define LED_BLINKING_INTERVAL 200u
#define PICO06_PENDING_OUTPUTS 4u
#define PICO06_CANON_OUTPUTS 2u

// Upstream HOGP still raises the legacy request. USB identity is stable from
// PICO-04 onward, so PICO-06 consumes it without re-enumerating the host.
volatile bool g_usb_reinit_request = false;

void usb_dev_main(void);
void hid_task(void);
void led_blinking_task(void);
bool send_hid_report(void);

extern bool is_ble_app_state_ready(void);
extern const uint8_t *get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);
extern void ble_host_main(void);

int main(void)
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) board_init_after_tusb();

    stdio_init_all();
    CMN_Init(); // legacy helper storage; active HOGP path uses remote_hid_queue
    canonical_hid_init();
    remote_hid_queue_init();
    device_profile_init();
    remap_profile_init();
    logitech_hidpp_init();
    remap_engine_init();

    pico_hat_ui_init();
    pico06_ui_init();

    // BTstack/TLV flash work is kept on Core1; protect Core0 while flash writes.
    flash_safe_execute_core_init();
    multicore_launch_core1(ble_host_main);

    usb_dev_main();
    return 0;
}

void usb_dev_main(void)
{
    while (1) {
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            printf("[PICO-06] ignored legacy USB re-enumeration request\r\n");
        }

        tud_task();
        hid_task();
        pico_hat_ui_task();
        pico06_ui_task();
        led_blinking_task();
    }
}

void tud_mount_cb(void) {}
void tud_umount_cb(void) {}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {}

bool send_hid_report(void)
{
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
        printf("[PICO-06] BLE disconnect -> neutral USB outputs\r\n");
    }
    previous_ble_ready = ble_ready;

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

    if (is_ble_app_state_ready()) {
        if (!led_state) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            led_state = true;
        }
        return;
    }

    if (board_millis() - start_ms < LED_BLINKING_INTERVAL) return;
    start_ms += LED_BLINKING_INTERVAL;
    led_state = !led_state;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
}
