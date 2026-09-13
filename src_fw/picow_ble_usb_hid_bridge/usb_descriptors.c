/*
 * Temporary diagnostic USB identity: stable HID + CDC ACM boot log.
 *
 * This file exists only on debug/pico08-usb-cdc-bootlog. The production branch
 * keeps the PICO-04 HID-only descriptor and PID 0x4004.
 */

#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0xCAFE
#define USB_PID 0x40D0
#define USB_BCD 0x0200

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0xEF, // Miscellaneous / IAD composite
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0201,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),

    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x05,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x16, 0x01, 0x80,
    0x26, 0xFF, 0x7F,
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0x05, 0x0C,
    0x0A, 0x38, 0x02,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0,
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

enum {
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID       0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x83

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,
                          ITF_NUM_TOTAL,
                          0,
                          CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          250),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC,
                       0,
                       EPNUM_CDC_NOTIF,
                       8,
                       EPNUM_CDC_OUT,
                       EPNUM_CDC_IN,
                       64),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID,
                       0,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report),
                       EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE,
                       1),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Remapper",
    "Pico Remapper DEBUG CDC",
    NULL,
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL:
            chr_count = board_usb_get_serial(_desc_str + 1, 32);
            break;

        default: {
            if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;
            const char *str = string_desc_arr[index];
            if (str == NULL) return NULL;
            chr_count = strlen(str);
            const size_t max_count = (sizeof(_desc_str) / sizeof(_desc_str[0])) - 1;
            if (chr_count > max_count) chr_count = max_count;
            for (size_t i = 0; i < chr_count; ++i) _desc_str[1 + i] = (uint16_t)str[i];
            break;
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return _desc_str;
}
