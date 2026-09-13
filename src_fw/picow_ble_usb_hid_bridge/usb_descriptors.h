/*
 * Stable firmware-owned USB report IDs.
 * Remote BLE Report IDs must never leak across the PICO-04 canonical boundary.
 */
#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

enum
{
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE = 2,
    REPORT_ID_COUNT
};

#endif /* USB_DESCRIPTORS_H_ */
