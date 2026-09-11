// Copyright © 2025 Shiomachi Software. All rights reserved.
#include "Common.h"
#include "btstack.h"

// -----------------------------------------------------------------------------
// Logitech Lift POC remapper
// -----------------------------------------------------------------------------
// The BLE HIDS host exposes the Report Map separately from each incoming report.
// For this POC we keep the mouse's original HID descriptor/report format and only
// rewrite the two button bits we care about:
//
//   HID Button 5 (Forward) -> HID Button 1 (Left)
//
// Button 5 is suppressed on USB. While Button 5 remains physically pressed,
// Button 1 remains logically pressed, so ordinary pointer motion becomes a native
// USB left-button drag-and-drop with no host-side software or macro.
//
// The bit locations are discovered from the BLE HID Report Map instead of being
// hard-coded to a Logitech-specific byte layout.

#define POC_HID_USAGE_PAGE_BUTTON      0x09u
#define POC_LEFT_BUTTON_USAGE          0x01u
#define POC_FORWARD_BUTTON_USAGE       0x05u
#define POC_INVALID_BIT_POS            0xffffu

typedef struct {
    bool scanned;
    bool supported;
    uint16_t left_bit_pos;
    uint16_t forward_bit_pos;
} POC_HID_REPORT_MAP;

static POC_HID_REPORT_MAP f_poc_report_map[256] = {0};
static const uint8_t *f_poc_descriptor = NULL;
static uint16_t f_poc_descriptor_len = 0;

// These are implemented by hog_host_demo.c. The current bridge exposes service 0,
// which is sufficient for the single HID service used by the POC target mouse.
extern const uint8_t* get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);

static bool poc_report_id_matches(uint16_t descriptor_report_id, uint8_t report_id)
{
    if (descriptor_report_id == HID_REPORT_ID_UNDEFINED) {
        return report_id == 0;
    }
    return descriptor_report_id == report_id;
}

static bool poc_read_bit(const uint8_t *report, uint16_t report_len, uint16_t bit_pos)
{
    if (report == NULL || bit_pos >= (uint32_t)report_len * 8u) {
        return false;
    }
    return (report[bit_pos >> 3] & (uint8_t)(1u << (bit_pos & 7u))) != 0;
}

static void poc_write_bit(uint8_t *report, uint16_t report_len, uint16_t bit_pos, bool value)
{
    if (report == NULL || bit_pos >= (uint32_t)report_len * 8u) {
        return;
    }

    uint8_t *byte = &report[bit_pos >> 3];
    const uint8_t mask = (uint8_t)(1u << (bit_pos & 7u));
    if (value) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

static void poc_reset_report_map_if_descriptor_changed(const uint8_t *descriptor, uint16_t descriptor_len)
{
    if (descriptor == f_poc_descriptor && descriptor_len == f_poc_descriptor_len) {
        return;
    }

    memset(f_poc_report_map, 0, sizeof(f_poc_report_map));
    f_poc_descriptor = descriptor;
    f_poc_descriptor_len = descriptor_len;
}

static POC_HID_REPORT_MAP *poc_get_report_map(uint8_t report_id,
                                               const uint8_t *descriptor,
                                               uint16_t descriptor_len)
{
    POC_HID_REPORT_MAP *map = &f_poc_report_map[report_id];
    if (map->scanned) {
        return map;
    }

    map->scanned = true;
    map->supported = false;
    map->left_bit_pos = POC_INVALID_BIT_POS;
    map->forward_bit_pos = POC_INVALID_BIT_POS;

    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, descriptor, descriptor_len, HID_REPORT_TYPE_INPUT);

    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        if (!poc_report_id_matches(item.report_id, report_id)) {
            continue;
        }
        if (item.usage_page != POC_HID_USAGE_PAGE_BUTTON) {
            continue;
        }
        // Input item bit 0 == Constant. We only want actual data controls.
        if ((item.descriptor_item.item_value & 0x01) != 0) {
            continue;
        }
        // Standard mouse buttons are one-bit Variable fields. Keeping this POC
        // deliberately narrow avoids guessing if a device uses an unusual array.
        if (item.size != 1) {
            continue;
        }

        if (item.usage == POC_LEFT_BUTTON_USAGE) {
            map->left_bit_pos = item.bit_pos;
        } else if (item.usage == POC_FORWARD_BUTTON_USAGE) {
            map->forward_bit_pos = item.bit_pos;
        }
    }

    map->supported = map->left_bit_pos != POC_INVALID_BIT_POS &&
                     map->forward_bit_pos != POC_INVALID_BIT_POS;

    if (map->supported) {
        printf("[POC] HID report %u: Button 5/Forward bit %u -> Button 1/Left bit %u\n",
               report_id, map->forward_bit_pos, map->left_bit_pos);
    } else {
        printf("[POC] HID report %u: no same-report Button 1 + Button 5 mapping\n", report_id);
    }

    return map;
}

static void poc_remap_forward_to_left(ST_HID_RPT *hid_report)
{
    if (hid_report == NULL || hid_report->report_len == 0) {
        return;
    }

    const uint8_t *descriptor = get_ble_hid_report_descriptor_data();
    const uint16_t descriptor_len = get_ble_hid_report_descriptor_len();
    if (descriptor == NULL || descriptor_len == 0) {
        return;
    }

    poc_reset_report_map_if_descriptor_changed(descriptor, descriptor_len);

    POC_HID_REPORT_MAP *map = poc_get_report_map(hid_report->report_id,
                                                  descriptor,
                                                  descriptor_len);
    if (!map->supported) {
        return;
    }

    if (map->left_bit_pos >= (uint32_t)hid_report->report_len * 8u ||
        map->forward_bit_pos >= (uint32_t)hid_report->report_len * 8u) {
        return;
    }

    const bool physical_left = poc_read_bit(hid_report->report,
                                             hid_report->report_len,
                                             map->left_bit_pos);
    const bool physical_forward = poc_read_bit(hid_report->report,
                                                hid_report->report_len,
                                                map->forward_bit_pos);

    // Preserve a real left-button press. Forward acts as an OR-ed left button,
    // which also makes release behavior correct if both are held together.
    poc_write_bit(hid_report->report,
                  hid_report->report_len,
                  map->left_bit_pos,
                  physical_left || physical_forward);

    // Never expose Forward to the USB host in this POC.
    poc_write_bit(hid_report->report,
                  hid_report->report_len,
                  map->forward_bit_pos,
                  false);
}

static void poc_prepend_usb_report_id(ST_HID_RPT *hid_report)
{
    // HOGP transports the Report ID in the Report Reference descriptor rather
    // than inside the GATT characteristic payload. TinyUSB is currently called
    // by main.c with report_id = 0, so for report-ID based devices we prepend it
    // to the raw payload here before it reaches USB.
    if (hid_report == NULL || hid_report->report_id == 0) {
        return;
    }
    if (hid_report->report_len >= CMN_HID_RPT_DATA_SIZE) {
        return;
    }

    memmove(&hid_report->report[1], &hid_report->report[0], hid_report->report_len);
    hid_report->report[0] = hid_report->report_id;
    hid_report->report_len++;

    // main.c sends raw bytes with tud_hid_report(0, ...); mark the report as
    // normalized to avoid a future caller interpreting the ID twice.
    hid_report->report_id = 0;
}

// [File Scope Variables]
static ST_QUE f_astQue[CMN_QUE_KIND_NUM] = {0}; // Array of queue control structures
static ST_HID_RPT f_astQueData_hid[CMN_QUE_DATA_MAX_HID_RPT] = {0}; // Data buffer for the HID queue
static critical_section_t f_stSpinLock = {0}; // Spinlock structure

// Enqueues data into the specified queue
bool CMN_Enqueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;
    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt;
    ST_HID_RPT stPocHidRpt;
    PVOID pDataToQueue = pData;

    // POC transformation is done before taking the cross-core queue lock so HID
    // descriptor parsing cannot lengthen the critical section.
    if (iQue == CMN_QUE_KIND_HID_RPT && pData != NULL) {
        memcpy(&stPocHidRpt, pData, sizeof(stPocHidRpt));
        poc_remap_forward_to_left(&stPocHidRpt);
        poc_prepend_usb_report_id(&stPocHidRpt);
        pDataToQueue = &stPocHidRpt;
    }

    CMN_EntrySpinLock(); // Acquire spinlock

    if ((pstQue->head == (pstQue->tail + 1) % pstQue->max)) {
        // Queue is full
    }
    else {
        // Perform queuing
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT: // HID Report Queue
            pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
            memcpy(&pstHidRpt[pstQue->tail], pDataToQueue, sizeof(ST_HID_RPT));
            break;
        default:
            // Should not be reached
            break;
        }
        pstQue->tail = (pstQue->tail + 1) % pstQue->max;

        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Dequeues data from the specified queue
bool CMN_Dequeue(ULONG iQue, PVOID pData)
{
    bool bRet = false;
    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt;

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty

        // Do nothing
    }
    else {
        // Perform dequeuing
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT:  // HID Report Queue
            pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
            memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
            break;
        default:
            // Should not be reached
            break;
        }
        pstQue->head = (pstQue->head + 1) % pstQue->max;
        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Peeks at the data from the specified queue without removing it
bool CMN_PeekQueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;
    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt;

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty

        // Do nothing
    }
    else {
        // Copy data
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT:  // HID Report Queue
            pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
            memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
            break;
        default:
            // Should not be reached
            break;
        }
        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Advances the queue's read pointer (head)
void CMN_AdvanceQueue(ULONG iQue)
{
    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty

        // Do nothing
    }
    else {
        // Advance the head pointer
        pstQue->head = (pstQue->head + 1) % pstQue->max;
    }

    CMN_ExitSpinLock(); // Release spinlock
}

// Clears all data from the specified queue.
void CMN_ClearQueue(ULONG iQue)
{
    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock(); // Acquire spinlock

    // Reset head and tail pointers to empty the queue
    pstQue->head = 0;
    pstQue->tail = 0;

    CMN_ExitSpinLock(); // Release spinlock
}

// Enters a critical section (spinlock).
void CMN_EntrySpinLock(void)
{
    critical_section_enter_blocking(&f_stSpinLock);
}

// Exits the critical section (spinlock)
void CMN_ExitSpinLock(void)
{
    critical_section_exit(&f_stSpinLock);
}

// Initializes the common library
void CMN_Init(void)
{
    // [Initialize variables]
    critical_section_init(&f_stSpinLock);
    f_astQue[CMN_QUE_KIND_HID_RPT].pBuf = (PVOID)f_astQueData_hid;
    f_astQue[CMN_QUE_KIND_HID_RPT].max  = CMN_QUE_DATA_MAX_HID_RPT;
}
