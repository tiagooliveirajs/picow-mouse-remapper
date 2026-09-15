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
 */

#include "bsp/board_api.h"
#include "tusb.h"

#include "AppLog.h"
#include "BluetoothHost.h"
#include "Common.h"

#define USB_REINIT_STABILIZATION_DELAY 100 // ms
#define LED_BLINKING_INTERVAL 200 // ms

volatile bool g_usb_reinit_request = false;

void usb_dev_main(void);
void hid_task(void);
void led_blinking_task(void);
bool send_hid_report(void);

int main(void)
{
    board_init();

    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    stdio_init_all();
    APP_LOG_Init();
    APP_LOG_PrintBootBanner();

    CMN_Init();
    APP_LOG_Info("common HID queue initialized");

    // BTstack can write link keys from Core 1. Register Core 0 so flash-safe
    // execution coordinates both cores.
    flash_safe_execute_core_init();
    APP_LOG_Info("flash-safe multicore support initialized");

    multicore_launch_core1(BT_HOST_CoreMain);
    APP_LOG_Info("Bluetooth host launched on Core 1 (%s)", BT_HOST_GetTransportName());
    APP_LOG_Info("USB HID/CDC loop starting on Core 0");

    usb_dev_main();
    return 0;
}

void usb_dev_main(void)
{
    while (1)
    {
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            APP_LOG_Info("Bluetooth HID descriptor changed; re-enumerating USB device");

            if (tud_mounted()) {
                tud_disconnect();
                board_delay(USB_REINIT_STABILIZATION_DELAY);
            }

            // Reports queued for a previous HID descriptor must never be sent
            // after USB re-enumeration.
            CMN_ClearQueue(CMN_QUE_KIND_HID_RPT);
            tud_connect();
        }

        tud_task();
        APP_LOG_Task();
        led_blinking_task();
        hid_task();
    }
}

void tud_mount_cb(void)
{
    APP_LOG_Info("USB mounted by host");
}

void tud_umount_cb(void)
{
    APP_LOG_Info("USB unmounted from host");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    APP_LOG_Info("USB suspended (remote wakeup=%s)", remote_wakeup_en ? "enabled" : "disabled");
}

void tud_resume_cb(void)
{
    APP_LOG_Info("USB resumed");
}

bool send_hid_report(void)
{
    static ST_HID_RPT stHidRpt;
    bool bRet = false;

    if (CMN_PeekQueue(CMN_QUE_KIND_HID_RPT, &stHidRpt)) {
        if (tud_suspended()) {
            tud_remote_wakeup();
            return bRet;
        }

        if (tud_hid_ready()) {
            if (tud_hid_report(0, stHidRpt.report, stHidRpt.report_len)) {
                CMN_AdvanceQueue(CMN_QUE_KIND_HID_RPT);
                bRet = true;
            }
        }
    }

    return bRet;
}

void hid_task(void)
{
    (void)send_hid_report();
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void) instance;
    (void) len;
    (void) report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t* buffer,
                               uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer,
                           uint16_t bufsize)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;
}

void led_blinking_task(void)
{
    static uint32_t start_ms = 0;
    static bool led_state = false;
    const uint32_t blink_interval = LED_BLINKING_INTERVAL;

    if (BT_HOST_IsReady()) {
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
