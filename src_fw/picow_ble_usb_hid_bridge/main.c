/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"

#include "Common.h"
#include "canonical_hid.h"
#include "device_profile.h"
#include "pico05_status.h"
#include "pico_hat_ui.h"
#include "remote_hid_queue.h"

#define LED_BLINKING_INTERVAL 200 // ms
#define PICO05_PENDING_OUTPUTS 2u

// Kept because the upstream-derived HOGP source still raises the legacy flag.
// USB identity is stable from PICO-04 onward, so the request is only consumed.
volatile bool g_usb_reinit_request = false;

void usb_dev_main(void);
void hid_task(void);
void led_blinking_task(void);
void pico_hat_event_log_task(void);
bool send_hid_report(void);

extern bool is_ble_app_state_ready(void);
extern const uint8_t *get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);
extern void ble_host_main(void);

/*------------- MAIN -------------*/
int main(void)
{
    board_init();

    tud_init(BOARD_TUD_RHPORT);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    stdio_init_all();
    CMN_Init(); // retained for legacy helpers; active HOGP traffic uses remote_hid_queue
    canonical_hid_init();
    remote_hid_queue_init();
    device_profile_init();

    // PICO-03 validated HAT baseline. LCD/input remains cooperative so USB HID
    // is serviced before local UI work in every Core0 pass.
    pico_hat_ui_init();
    pico05_status_init();

    // Lock out Core0 when BTstack performs flash writes on Core1.
    flash_safe_execute_core_init();
    multicore_launch_core1(ble_host_main);

    usb_dev_main();
    return 0;
}

//--------------------------------------------------------------------+
// Main loop for the USB device (Core0)
//--------------------------------------------------------------------+
void usb_dev_main(void)
{
    while (1) {
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            printf("[PICO-05] ignored legacy USB re-enumeration request\r\n");
        }

        tud_task();
        hid_task();
        pico_hat_ui_task();
        pico05_status_task();
        pico_hat_event_log_task();
        led_blinking_task();
    }
}

//--------------------------------------------------------------------+
// Local input logging
//--------------------------------------------------------------------+
void pico_hat_event_log_task(void)
{
    pico_hat_event_t event;
    if (!pico_hat_ui_poll_event(&event)) {
        return;
    }

    // PICO-05 does not assign profile/remap semantics to HAT buttons. The old
    // PICO-03 colored marker renderer is deliberately not called here because
    // the gate screen now owns the framebuffer using the black/white product
    // accessibility baseline. KEY4 screen lock remains implemented in ui.c.
    printf("[PICO-05] %s %s lock=%u\r\n",
           pico_hat_ui_input_name(event.input),
           event.pressed ? "DOWN" : "UP",
           pico_hat_ui_is_screen_locked() ? 1u : 0u);
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+
void tud_mount_cb(void)
{
}

void tud_umount_cb(void)
{
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
}

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

// PICO-05 consumes RAW normalized HOGP reports directly. This intentionally
// bypasses Common.c's historical PICO-01 Forward->Left transform, so a newly
// paired/restored PASSTHROUGH profile cannot inherit an implicit remap.
bool send_hid_report(void)
{
    static ST_HID_RPT remote_report;
    static ST_HID_RPT pending[PICO05_PENDING_OUTPUTS];
    static uint8_t pending_count = 0;
    static uint8_t pending_index = 0;
    static bool readiness_initialized = false;
    static bool previous_ble_ready = false;

    const bool ble_ready = is_ble_app_state_ready();
    if (!readiness_initialized) {
        readiness_initialized = true;
        previous_ble_ready = ble_ready;
    }

    // A BLE disconnect is a hard state boundary. Drop stale raw/pending traffic
    // and synthesize neutral mouse state before resetting device maps.
    if (previous_ble_ready && !ble_ready) {
        pending_index = 0;
        pending_count = (uint8_t)canonical_hid_neutralize(pending,
                                                           PICO05_PENDING_OUTPUTS);
        remote_hid_queue_clear();
        canonical_hid_reset_device();
        printf("[PICO-05] BLE disconnect -> neutral USB outputs\r\n");
    }
    previous_ble_ready = ble_ready;

    // If no canonical output is pending, translate one queued raw remote report.
    if (pending_index >= pending_count) {
        pending_index = 0;
        pending_count = 0;

        if (remote_hid_queue_peek(&remote_report)) {
            pending_count = (uint8_t)canonical_hid_process_remote_report(
                &remote_report,
                get_ble_hid_report_descriptor_data(),
                get_ble_hid_report_descriptor_len(),
                pending,
                PICO05_PENDING_OUTPUTS);

            // The raw remote report has been fully copied/consumed by the
            // canonical adapter whether or not it produced a USB report.
            remote_hid_queue_advance();
        }
    }

    if (pending_index >= pending_count) {
        return false;
    }

    if (tud_suspended()) {
        tud_remote_wakeup();
        return false;
    }

    if (!tud_hid_ready()) {
        return false;
    }

    ST_HID_RPT *output = &pending[pending_index];
    if (!tud_hid_report(output->report_id, output->report, output->report_len)) {
        return false;
    }

    pending_index++;
    return true;
}

//--------------------------------------------------------------------+
// HID task
//--------------------------------------------------------------------+
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

//--------------------------------------------------------------------+
// LED task
//--------------------------------------------------------------------+
void led_blinking_task(void)
{
    static uint32_t start_ms = 0;
    static bool led_state = false;
    const uint32_t blink_interval = LED_BLINKING_INTERVAL;

    if (is_ble_app_state_ready()) {
        if (!led_state) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            led_state = true;
        }
    } else {
        if (board_millis() - start_ms < blink_interval) return;
        start_ms += blink_interval;
        led_state = !led_state;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
    }
}
