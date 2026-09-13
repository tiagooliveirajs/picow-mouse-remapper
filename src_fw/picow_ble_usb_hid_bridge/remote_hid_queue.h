#ifndef REMOTE_HID_QUEUE_H
#define REMOTE_HID_QUEUE_H

#include <stdbool.h>

#include "Common.h"

void remote_hid_queue_init(void);
bool remote_hid_queue_enqueue(const ST_HID_RPT *report);
bool remote_hid_queue_peek(ST_HID_RPT *report);
void remote_hid_queue_advance(void);
void remote_hid_queue_clear(void);

#endif // REMOTE_HID_QUEUE_H
