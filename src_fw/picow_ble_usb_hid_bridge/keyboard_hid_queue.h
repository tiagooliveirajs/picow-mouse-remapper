#ifndef KEYBOARD_HID_QUEUE_H
#define KEYBOARD_HID_QUEUE_H

#include <stdbool.h>

#include "Common.h"

void keyboard_hid_queue_init(void);
bool keyboard_hid_queue_enqueue(const ST_HID_RPT *report);
bool keyboard_hid_queue_peek(ST_HID_RPT *report);
void keyboard_hid_queue_advance(void);
void keyboard_hid_queue_clear(void);

#endif // KEYBOARD_HID_QUEUE_H
