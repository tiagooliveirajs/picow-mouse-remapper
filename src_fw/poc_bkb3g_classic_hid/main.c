#include <stdio.h>

#include "bsp/board_api.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "Common.h"

#define USB_REINIT_STABILIZATION_DELAY_MS 100

// Implemented in bkb3g_classic_hid.c so BTstack headers never share a
// translation unit with TinyUSB HID headers. Both stacks define a
// hid_report_type_t type, so keeping them isolated avoids a C type collision.
void bkb3g_classic_hid_core_main(void);

volatile bool g_usb_reinit_request = false;

static bool send_next_usb_hid_report(void) {
    static ST_HID_RPT report;

    if (!CMN_PeekQueue(CMN_QUE_KIND_HID_RPT, &report)) {
        return false;
    }

    if (tud_suspended()) {
        tud_remote_wakeup();
        return false;
    }

    if (!tud_hid_ready()) {
        return false;
    }

    // report.report already contains the Bluetooth device's Report ID as byte 0.
    // The USB descriptor is switched to the Bluetooth HID descriptor when the
    // keyboard becomes ready, so the report can be forwarded unchanged.
    if (!tud_hid_report(0, report.report, report.report_len)) {
        return false;
    }

    CMN_AdvanceQueue(CMN_QUE_KIND_HID_RPT);
    return true;
}

static void usb_device_task(void) {
    while (true) {
        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;

            if (tud_mounted()) {
                tud_disconnect();
                board_delay(USB_REINIT_STABILIZATION_DELAY_MS);
            }

            // Reports queued against the old USB descriptor must not be sent
            // after re-enumeration.
            CMN_ClearQueue(CMN_QUE_KIND_HID_RPT);
            tud_connect();
        }

        tud_task();
        (void)send_next_usb_hid_report();
        tight_loop_contents();
    }
}

int main(void) {
    board_init();

    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    // USB stdio is disabled because TinyUSB owns the USB peripheral as HID.
    // printf remains available on UART0 (GPIO0 TX / GPIO1 RX).
    stdio_init_all();
    CMN_Init();

    // BTstack may write link keys to flash from Core1. Register Core0 so flash
    // safe execution can coordinate both cores.
    flash_safe_execute_core_init();

    multicore_launch_core1(bkb3g_classic_hid_core_main);
    usb_device_task();

    return 0;
}

void tud_mount_cb(void) {}
void tud_umount_cb(void) {}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance;
    (void)report;
    (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen) {
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
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
