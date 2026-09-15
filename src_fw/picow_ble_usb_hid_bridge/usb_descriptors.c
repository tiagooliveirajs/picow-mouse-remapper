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

#include "BluetoothHost.h"
#include "usb_descriptors.h"

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 * [MSB]         HID | MSC | CDC         [LSB]
 */
#define _PID_MAP(itf, n)    ( (CFG_TUD_##itf) << (n) )
#define USB_PID             (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
                             _PID_MAP(MIDI, 3) | _PID_MAP(VENDOR, 4) )

#define USB_VID     0xCafe
#define USB_BCD     0x0200

tusb_desc_device_t const desc_device =
{
    .bLength                = sizeof(tusb_desc_device_t),
    .bDescriptorType        = TUSB_DESC_DEVICE,
    .bcdUSB                 = USB_BCD,
    .bDeviceClass           = TUSB_CLASS_MISC,
    .bDeviceSubClass        = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol        = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0        = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor               = USB_VID,
    .idProduct              = USB_PID,
    .bcdDevice              = 0x0100,

    .iManufacturer          = 0x01,
    .iProduct               = 0x02,
    .iSerialNumber          = 0x03,

    .bNumConfigurations     = 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &desc_device;
}

uint8_t const desc_hid_report[] =
{
    TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(REPORT_ID_KEYBOARD      )),
    TUD_HID_REPORT_DESC_MOUSE   ( HID_REPORT_ID(REPORT_ID_MOUSE         )),
    TUD_HID_REPORT_DESC_CONSUMER( HID_REPORT_ID(REPORT_ID_CONSUMER_CONTROL )),
    TUD_HID_REPORT_DESC_GAMEPAD ( HID_REPORT_ID(REPORT_ID_GAMEPAD       ))
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;

    const uint8_t *remote_descriptor = BT_HOST_GetReportDescriptor();
    if (BT_HOST_IsReady() && remote_descriptor != NULL) {
        return remote_descriptor;
    }

    return desc_hid_report;
}

enum
{
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_HID       0x83

static uint8_t const desc_cdc[] =
{
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

#define DYNAMIC_CONFIG_BUF_SIZE CONFIG_TOTAL_LEN
static uint8_t desc_configuration[DYNAMIC_CONFIG_BUF_SIZE] __attribute__((aligned(4)));

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;

    uint8_t *p_desc = desc_configuration;
    uint8_t const * const desc_end = p_desc + DYNAMIC_CONFIG_BUF_SIZE;

    uint16_t report_desc_len = BT_HOST_GetReportDescriptorLength();
    if (!BT_HOST_IsReady() || report_desc_len == 0) {
        report_desc_len = sizeof(desc_hid_report);
    }

    tusb_desc_configuration_t *config_desc = (tusb_desc_configuration_t*) p_desc;
    config_desc->bLength = sizeof(tusb_desc_configuration_t);
    config_desc->bDescriptorType = TUSB_DESC_CONFIGURATION;
    config_desc->wTotalLength = 0;
    config_desc->bNumInterfaces = ITF_NUM_TOTAL;
    config_desc->bConfigurationValue = 1;
    config_desc->iConfiguration = 0;
    config_desc->bmAttributes = TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP;
    config_desc->bMaxPower = 250;
    p_desc += sizeof(tusb_desc_configuration_t);

    TU_ASSERT((size_t)(desc_end - p_desc) >= sizeof(desc_cdc), NULL);
    memcpy(p_desc, desc_cdc, sizeof(desc_cdc));
    p_desc += sizeof(desc_cdc);

    TU_ASSERT((size_t)(desc_end - p_desc) >= TUD_HID_DESC_LEN, NULL);
    tusb_desc_interface_t *if_desc = (tusb_desc_interface_t*) p_desc;
    if_desc->bLength = sizeof(tusb_desc_interface_t);
    if_desc->bDescriptorType = TUSB_DESC_INTERFACE;
    if_desc->bInterfaceNumber = ITF_NUM_HID;
    if_desc->bAlternateSetting = 0;
    if_desc->bNumEndpoints = 1;
    if_desc->bInterfaceClass = TUSB_CLASS_HID;
    if_desc->bInterfaceSubClass = HID_SUBCLASS_NONE;
    if_desc->bInterfaceProtocol = HID_ITF_PROTOCOL_NONE;
    if_desc->iInterface = 0;
    p_desc += sizeof(tusb_desc_interface_t);

    *p_desc++ = 9;
    *p_desc++ = HID_DESC_TYPE_HID;
    tu_unaligned_write16(p_desc, 0x0111); p_desc += 2;
    *p_desc++ = 0;
    *p_desc++ = 1;
    *p_desc++ = HID_DESC_TYPE_REPORT;
    tu_unaligned_write16(p_desc, report_desc_len); p_desc += 2;

    tusb_desc_endpoint_t *ep_desc = (tusb_desc_endpoint_t*) p_desc;
    ep_desc->bLength = sizeof(tusb_desc_endpoint_t);
    ep_desc->bDescriptorType = TUSB_DESC_ENDPOINT;
    ep_desc->bEndpointAddress = EPNUM_HID;
    ep_desc->bmAttributes.xfer = TUSB_XFER_INTERRUPT;
    ep_desc->wMaxPacketSize = CFG_TUD_HID_EP_BUFSIZE;
    ep_desc->bInterval = 1;
    p_desc += sizeof(tusb_desc_endpoint_t);

    TU_ASSERT(p_desc <= desc_end, NULL);
    TU_ASSERT((size_t)(p_desc - desc_configuration) == DYNAMIC_CONFIG_BUF_SIZE, NULL);

    config_desc->wTotalLength = tu_htole16((uint16_t)(p_desc - desc_configuration));
    return desc_configuration;
}

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

char const *string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 },
    "TinyUSB",
    "Pico 2 W HID Remapper",
    NULL,
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    size_t chr_count;

    switch ( index ) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL:
            chr_count = board_usb_get_serial(_desc_str + 1, 32);
            break;

        default:
            if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;

            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
            if ( chr_count > max_count ) chr_count = max_count;

            for ( size_t i = 0; i < chr_count; i++ ) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
