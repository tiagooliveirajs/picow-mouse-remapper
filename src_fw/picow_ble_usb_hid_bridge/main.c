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
#include "pico_hat_diag.h"
#include "pico_hat_ui.h"

#define LED_BLINKING_INTERVAL 200 // ms
#define PICO04_PENDING_OUTPUTS 3u

// Kept only because the PICO-01 BLE host still writes this legacy flag when it
// reaches READY. PICO-04 deliberately consumes/ignores it: the firmware-owned
// USB descriptor is stable and must not disconnect/re-enumerate on BLE changes.
volatile bool g_usb_reinit_request = false;

static volatile bool g_pico04_toggle_left_escape_request = false;

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
    CMN_Init();
    canonical_hid_init();

    // PICO-03 validated HAT baseline. LCD/input remains cooperative so USB HID
    // is serviced before local UI work in every Core0 pass.
    pico_hat_ui_init();

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
        // PICO-04: connecting a BLE device must never change USB identity.
        // Consume the old POC request without disconnecting TinyUSB.
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            printf("[PICO-04] ignored legacy USB re-enumeration request\r\n");
        }

        tud_task();
        hid_task();
        pico_hat_ui_task();
        pico_hat_event_log_task();
        led_blinking_task();
    }
}

//--------------------------------------------------------------------+
// Local gate diagnostics
//--------------------------------------------------------------------+
void pico_hat_event_log_task(void)
{
    pico_hat_event_t event;
    if (!pico_hat_ui_poll_event(&event)) {
        return;
    }

    pico_hat_diag_handle_event(&event);

    // PICO-04 validation only: B/KEY2 toggles a volatile Left -> Escape route.
    // The production profile engine replaces this hook in PICO-06.
    if (event.input == PICO_HAT_INPUT_KEY2 && event.pressed) {
        g_pico04_toggle_left_escape_request = true;
    }

    printf("[PICO-04] %s %s lock=%u\r\n",
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

// Convert the PICO-01 remote-layout queue into fixed PICO-04 USB reports and
// send at most one USB report per call. This keeps TinyUSB servicing bounded
// even when one remote report produces both mouse and keyboard transitions.
bool send_hid_report(void)
{
    static ST_HID_RPT remote_report;
    static ST_HID_RPT pending[PICO04_PENDING_OUTPUTS];
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
    // and synthesize neutral mouse/keyboard state before resetting device maps.
    if (previous_ble_ready && !ble_ready) {
        pending_index = 0;
        pending_count = (uint8_t)canonical_hid_neutralize(pending,
                                                           PICO04_PENDING_OUTPUTS);
        CMN_ClearQueue(CMN_QUE_KIND_HID_RPT);
        canonical_hid_reset_device();
        printf("[PICO-04] BLE disconnect -> neutral USB outputs\r\n");
    }
    previous_ble_ready = ble_ready;

    // Local validation toggle is processed on the same Core0 path that owns USB
    // output so switching modes cannot race a report already being transmitted.
    if (g_pico04_toggle_left_escape_request && pending_index >= pending_count) {
        g_pico04_toggle_left_escape_request = false;
        pending_index = 0;
        pending_count = (uint8_t)canonical_hid_set_left_escape_test(
            !canonical_hid_left_escape_test_enabled(),
            pending,
            PICO04_PENDING_OUTPUTS);
    }

    // If no canonical output is pending, translate one queued remote report.
    if (pending_index >= pending_count) {
        pending_index = 0;
        pending_count = 0;

        if (CMN_PeekQueue(CMN_QUE_KIND_HID_RPT, &remote_report)) {
            pending_count = (uint8_t)canonical_hid_process_remote_report(
                &remote_report,
                get_ble_hid_report_descriptor_data(),
                get_ble_hid_report_descriptor_len(),
                pending,
                PICO04_PENDING_OUTPUTS);

            // The remote report has been fully copied/consumed by the canonical
            // adapter regardless of whether it produced a USB report.
            CMN_AdvanceQueue(CMN_QUE_KIND_HID_RPT);
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
