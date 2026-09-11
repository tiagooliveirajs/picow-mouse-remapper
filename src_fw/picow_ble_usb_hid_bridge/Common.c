// Copyright © 2025 Shiomachi Software. All rights reserved.
#include "Common.h"
#include "btstack.h"

// -----------------------------------------------------------------------------
// Logitech Lift POC
// -----------------------------------------------------------------------------
// The validated Linux setup for the Lift needs Logitech HID++
// REPROG_CONTROLS_V4 (0x1B04) to obtain real press/hold/release semantics for
// physical Forward (CID 0x0056). Ordinary host mouse events are not sufficient
// for the drag-and-drop use case.
//
// This POC therefore does two things entirely inside the Pico:
//   1. asks the Lift to temporarily divert CID 0x0056 to HID++ events;
//   2. turns the diverted held-state into HID Button 1 in the ordinary mouse
//      input report that is forwarded to USB.
//
// There is also a standards-based fallback that maps HID Button 5 -> Button 1
// if a device happens to expose Forward with native held-button semantics.

#define POC_HID_USAGE_PAGE_BUTTON          0x09u
#define POC_LEFT_BUTTON_USAGE              0x01u
#define POC_FORWARD_BUTTON_USAGE           0x05u
#define POC_INVALID_BIT_POS                0xffffu

#define POC_HIDPP_REPORT_ID_SHORT          0x10u
#define POC_HIDPP_REPORT_ID_LONG           0x11u
#define POC_HIDPP_LONG_PAYLOAD_LEN         19u
#define POC_HIDPP_DIRECT_DEVICE_INDEX      0xffu
#define POC_HIDPP_SOFTWARE_ID              0x02u
#define POC_HIDPP_ROOT_FEATURE_INDEX       0x00u
#define POC_HIDPP_REPROG_CONTROLS_V4       0x1b04u
#define POC_HIDPP_FORWARD_CID              0x0056u
#define POC_HIDPP_SET_TEMP_DIVERT_FLAGS    0x03u

#define POC_REPORT_MAP_SLOTS               8u
#define POC_RELATIVE_FIELD_MAX             16u

typedef struct {
    uint16_t bit_pos;
    uint8_t size;
} POC_RELATIVE_FIELD;

typedef struct {
    bool used;
    uint8_t report_id;
    bool has_left;
    bool has_forward;
    uint16_t left_bit_pos;
    uint16_t forward_bit_pos;
    uint16_t expected_report_len;
    uint8_t relative_count;
    POC_RELATIVE_FIELD relative[POC_RELATIVE_FIELD_MAX];
} POC_HID_REPORT_MAP;

typedef enum {
    POC_HIDPP_NEED_GET_FEATURE = 0,
    POC_HIDPP_WAIT_GET_FEATURE,
    POC_HIDPP_NEED_SET_DIVERT,
    POC_HIDPP_WAIT_SET_DIVERT,
    POC_HIDPP_ACTIVE,
    POC_HIDPP_FAILED
} POC_HIDPP_STATE;

// Implemented by hog_host_demo.c / hog_host_demo_poc.c
extern const uint8_t* get_ble_hid_report_descriptor_data(void);
extern uint16_t get_ble_hid_report_descriptor_len(void);
extern uint16_t poc_hids_current_cid(void);
extern uint8_t poc_hids_send_hidpp_long(const uint8_t *payload, uint8_t payload_len);

static POC_HID_REPORT_MAP f_poc_report_maps[POC_REPORT_MAP_SLOTS] = {0};
static const uint8_t *f_poc_descriptor = NULL;
static uint16_t f_poc_descriptor_len = 0;

static POC_HIDPP_STATE f_poc_hidpp_state = POC_HIDPP_NEED_GET_FEATURE;
static uint16_t f_poc_connection_cid = 0;
static uint8_t f_poc_reprog_feature_index = 0;
static uint8_t f_poc_send_retry_count = 0;
static bool f_poc_hidpp_forward_pressed = false;

static bool f_poc_mouse_template_valid = false;
static uint8_t f_poc_mouse_report_id = 0;
static uint16_t f_poc_mouse_report_len = 0;
static uint8_t f_poc_mouse_report[CMN_HID_RPT_DATA_SIZE] = {0};
static bool f_poc_physical_left_pressed = false;

// [File Scope Variables]
static ST_QUE f_astQue[CMN_QUE_KIND_NUM] = {0}; // Array of queue control structures
static ST_HID_RPT f_astQueData_hid[CMN_QUE_DATA_MAX_HID_RPT] = {0}; // Data buffer for the HID queue
static critical_section_t f_stSpinLock = {0}; // Spinlock structure

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

static void poc_zero_bits(uint8_t *report, uint16_t report_len, uint16_t bit_pos, uint8_t size)
{
    for (uint8_t i = 0; i < size; i++) {
        poc_write_bit(report, report_len, (uint16_t)(bit_pos + i), false);
    }
}

static void poc_reset_report_maps(void)
{
    memset(f_poc_report_maps, 0, sizeof(f_poc_report_maps));
    f_poc_mouse_template_valid = false;
    f_poc_mouse_report_id = 0;
    f_poc_mouse_report_len = 0;
    memset(f_poc_mouse_report, 0, sizeof(f_poc_mouse_report));
    f_poc_physical_left_pressed = false;
}

static void poc_reset_for_connection(uint16_t connection_cid)
{
    f_poc_connection_cid = connection_cid;
    f_poc_hidpp_state = POC_HIDPP_NEED_GET_FEATURE;
    f_poc_reprog_feature_index = 0;
    f_poc_send_retry_count = 0;
    f_poc_hidpp_forward_pressed = false;
    f_poc_descriptor = NULL;
    f_poc_descriptor_len = 0;
    poc_reset_report_maps();

    printf("[POC] new HIDS connection cid=%u; starting Lift HID++ setup\n", connection_cid);
}

static void poc_reset_report_maps_if_descriptor_changed(const uint8_t *descriptor,
                                                         uint16_t descriptor_len)
{
    if (descriptor == f_poc_descriptor && descriptor_len == f_poc_descriptor_len) {
        return;
    }

    f_poc_descriptor = descriptor;
    f_poc_descriptor_len = descriptor_len;
    poc_reset_report_maps();
}

static POC_HID_REPORT_MAP *poc_get_report_map(uint8_t report_id,
                                               const uint8_t *descriptor,
                                               uint16_t descriptor_len)
{
    POC_HID_REPORT_MAP *free_slot = NULL;

    for (uint8_t i = 0; i < POC_REPORT_MAP_SLOTS; i++) {
        if (f_poc_report_maps[i].used) {
            if (f_poc_report_maps[i].report_id == report_id) {
                return &f_poc_report_maps[i];
            }
        } else if (free_slot == NULL) {
            free_slot = &f_poc_report_maps[i];
        }
    }

    // The Lift uses only a small number of report IDs. If an unusual device
    // exceeds the tiny POC cache, recycle slot 0 rather than consuming RAM for
    // a 256-entry map.
    POC_HID_REPORT_MAP *map = free_slot != NULL ? free_slot : &f_poc_report_maps[0];
    memset(map, 0, sizeof(*map));
    map->used = true;
    map->report_id = report_id;
    map->left_bit_pos = POC_INVALID_BIT_POS;
    map->forward_bit_pos = POC_INVALID_BIT_POS;

    uint32_t max_bit_end = 0;
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, descriptor, descriptor_len, HID_REPORT_TYPE_INPUT);

    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        if (!poc_report_id_matches(item.report_id, report_id)) {
            continue;
        }

        const uint32_t item_end = (uint32_t)item.bit_pos + item.size;
        if (item_end > max_bit_end) {
            max_bit_end = item_end;
        }

        // Input item bit 0 == Constant.
        if ((item.descriptor_item.item_value & 0x01) != 0) {
            continue;
        }

        // Input item bit 2 == Relative. These fields must be cleared when we
        // synthesize a button-only report, otherwise an old movement delta
        // would be replayed on press/release.
        if ((item.descriptor_item.item_value & 0x04) != 0 &&
            map->relative_count < POC_RELATIVE_FIELD_MAX) {
            map->relative[map->relative_count].bit_pos = item.bit_pos;
            map->relative[map->relative_count].size = item.size;
            map->relative_count++;
        }

        if (item.usage_page != POC_HID_USAGE_PAGE_BUTTON || item.size != 1) {
            continue;
        }

        if (item.usage == POC_LEFT_BUTTON_USAGE) {
            map->has_left = true;
            map->left_bit_pos = item.bit_pos;
        } else if (item.usage == POC_FORWARD_BUTTON_USAGE) {
            map->has_forward = true;
            map->forward_bit_pos = item.bit_pos;
        }
    }

    map->expected_report_len = (uint16_t)((max_bit_end + 7u) / 8u);

    if (map->has_left) {
        printf("[POC] mouse report id=%u left-bit=%u forward-bit=%s\n",
               report_id,
               map->left_bit_pos,
               map->has_forward ? "present" : "not-present");
    }

    return map;
}

static void poc_hidpp_fail(const char *reason)
{
    if (f_poc_hidpp_state != POC_HIDPP_FAILED) {
        printf("[POC] HID++ setup failed: %s\n", reason);
    }
    f_poc_hidpp_state = POC_HIDPP_FAILED;
}

static void poc_hidpp_try_send(void)
{
    if (f_poc_hidpp_state != POC_HIDPP_NEED_GET_FEATURE &&
        f_poc_hidpp_state != POC_HIDPP_NEED_SET_DIVERT) {
        return;
    }

    uint8_t payload[POC_HIDPP_LONG_PAYLOAD_LEN] = {0};
    payload[0] = POC_HIDPP_DIRECT_DEVICE_INDEX;

    if (f_poc_hidpp_state == POC_HIDPP_NEED_GET_FEATURE) {
        // HID++ 2.0 Root.GetFeature(0x1B04)
        payload[1] = POC_HIDPP_ROOT_FEATURE_INDEX;
        payload[2] = POC_HIDPP_SOFTWARE_ID; // function 0, software id 2
        payload[3] = (uint8_t)(POC_HIDPP_REPROG_CONTROLS_V4 >> 8);
        payload[4] = (uint8_t)(POC_HIDPP_REPROG_CONTROLS_V4 & 0xffu);
    } else {
        // REPROG_CONTROLS_V4.SetControlReporting(0x0056, 0x03, 0x0000)
        // 0x03 = TemporaryDiverted | ChangeTemporaryDivert.
        payload[1] = f_poc_reprog_feature_index;
        payload[2] = (uint8_t)((3u << 4) | POC_HIDPP_SOFTWARE_ID);
        payload[3] = (uint8_t)(POC_HIDPP_FORWARD_CID >> 8);
        payload[4] = (uint8_t)(POC_HIDPP_FORWARD_CID & 0xffu);
        payload[5] = POC_HIDPP_SET_TEMP_DIVERT_FLAGS;
        payload[6] = 0;
        payload[7] = 0;
    }

    const uint8_t status = poc_hids_send_hidpp_long(payload, sizeof(payload));
    if (status == ERROR_CODE_SUCCESS) {
        f_poc_send_retry_count = 0;
        if (f_poc_hidpp_state == POC_HIDPP_NEED_GET_FEATURE) {
            f_poc_hidpp_state = POC_HIDPP_WAIT_GET_FEATURE;
            printf("[POC] HID++ query sent: feature 0x1B04\n");
        } else {
            f_poc_hidpp_state = POC_HIDPP_WAIT_SET_DIVERT;
            printf("[POC] HID++ divert request sent: CID 0x0056\n");
        }
        return;
    }

    // The HIDS client may briefly be busy finishing the previous GATT write.
    // Retry on later input reports before declaring the transport unusable.
    f_poc_send_retry_count++;
    if (f_poc_send_retry_count == 1 || f_poc_send_retry_count == 10) {
        printf("[POC] HID++ write busy/failed status=0x%02x retry=%u\n",
               status, f_poc_send_retry_count);
    }
    if (f_poc_send_retry_count >= 32) {
        poc_hidpp_fail("cannot write HID++ long report 0x11");
    }
}

static bool poc_hidpp_error_matches(const ST_HID_RPT *hid_report,
                                     uint8_t expected_feature,
                                     uint8_t expected_function)
{
    // HID++ 2.0 error long report (without Report ID in the HOGP payload):
    // [device][0xff][original feature][original function|swid][error]...
    if (hid_report->report_id != POC_HIDPP_REPORT_ID_LONG || hid_report->report_len < 5) {
        return false;
    }
    if (hid_report->report[1] != 0xffu) {
        return false;
    }

    const uint8_t original_feature = hid_report->report[2];
    const uint8_t original_function = (uint8_t)(hid_report->report[3] >> 4);
    const uint8_t original_sw_id = (uint8_t)(hid_report->report[3] & 0x0fu);

    if (original_feature != expected_feature ||
        original_function != expected_function ||
        original_sw_id != POC_HIDPP_SOFTWARE_ID) {
        return false;
    }

    printf("[POC] HID++ error feature=0x%02x function=%u code=0x%02x\n",
           original_feature, original_function, hid_report->report[4]);
    return true;
}

static bool poc_hidpp_process_report(const ST_HID_RPT *hid_report,
                                     bool *forward_state_changed)
{
    *forward_state_changed = false;

    if (hid_report->report_id != POC_HIDPP_REPORT_ID_SHORT &&
        hid_report->report_id != POC_HIDPP_REPORT_ID_LONG) {
        return false;
    }

    // HID++ vendor reports are consumed by the remapper and are intentionally
    // not forwarded to the computer in this POC.
    if (hid_report->report_len < 3) {
        return true;
    }

    if (f_poc_hidpp_state == POC_HIDPP_WAIT_GET_FEATURE &&
        poc_hidpp_error_matches(hid_report, POC_HIDPP_ROOT_FEATURE_INDEX, 0)) {
        poc_hidpp_fail("Lift does not expose REPROG_CONTROLS_V4");
        return true;
    }

    if (f_poc_hidpp_state == POC_HIDPP_WAIT_SET_DIVERT &&
        poc_hidpp_error_matches(hid_report, f_poc_reprog_feature_index, 3)) {
        poc_hidpp_fail("CID 0x0056 diversion rejected");
        return true;
    }

    const uint8_t feature = hid_report->report[1];
    const uint8_t function = (uint8_t)(hid_report->report[2] >> 4);
    const uint8_t sw_id = (uint8_t)(hid_report->report[2] & 0x0fu);

    if (f_poc_hidpp_state == POC_HIDPP_WAIT_GET_FEATURE &&
        feature == POC_HIDPP_ROOT_FEATURE_INDEX &&
        function == 0 &&
        sw_id == POC_HIDPP_SOFTWARE_ID &&
        hid_report->report_len >= 4) {

        f_poc_reprog_feature_index = hid_report->report[3];
        if (f_poc_reprog_feature_index == 0) {
            poc_hidpp_fail("feature 0x1B04 not found");
        } else {
            printf("[POC] HID++ feature 0x1B04 index=0x%02x\n",
                   f_poc_reprog_feature_index);
            f_poc_hidpp_state = POC_HIDPP_NEED_SET_DIVERT;
        }
        return true;
    }

    if (f_poc_hidpp_state == POC_HIDPP_WAIT_SET_DIVERT &&
        feature == f_poc_reprog_feature_index &&
        function == 3 &&
        sw_id == POC_HIDPP_SOFTWARE_ID) {

        f_poc_hidpp_state = POC_HIDPP_ACTIVE;
        printf("[POC] Lift Forward CID 0x0056 diverted; drag remap ACTIVE\n");
        return true;
    }

    if (f_poc_hidpp_state == POC_HIDPP_ACTIVE &&
        feature == f_poc_reprog_feature_index &&
        function == 0) {

        // DivertedButtonEvent contains the complete currently-held CID set as
        // big-endian 16-bit values. A zero CID terminates the list.
        bool forward_pressed = false;
        for (uint16_t pos = 3; pos + 1 < hid_report->report_len; pos += 2) {
            const uint16_t cid = (uint16_t)(((uint16_t)hid_report->report[pos] << 8) |
                                            hid_report->report[pos + 1]);
            if (cid == 0) {
                break;
            }
            if (cid == POC_HIDPP_FORWARD_CID) {
                forward_pressed = true;
            }
        }

        if (forward_pressed != f_poc_hidpp_forward_pressed) {
            f_poc_hidpp_forward_pressed = forward_pressed;
            *forward_state_changed = true;
            printf("[POC] Forward %s -> USB Left %s\n",
                   forward_pressed ? "DOWN" : "UP",
                   forward_pressed ? "DOWN" : "UP");
        }
    }

    return true;
}

static void poc_apply_mouse_remap(ST_HID_RPT *hid_report,
                                  const uint8_t *descriptor,
                                  uint16_t descriptor_len,
                                  bool remember_template)
{
    POC_HID_REPORT_MAP *map = poc_get_report_map(hid_report->report_id,
                                                  descriptor,
                                                  descriptor_len);
    if (!map->has_left || map->left_bit_pos >= (uint32_t)hid_report->report_len * 8u) {
        return;
    }

    const bool physical_left = poc_read_bit(hid_report->report,
                                             hid_report->report_len,
                                             map->left_bit_pos);
    f_poc_physical_left_pressed = physical_left;

    bool physical_forward = false;
    if (map->has_forward &&
        map->forward_bit_pos < (uint32_t)hid_report->report_len * 8u) {
        physical_forward = poc_read_bit(hid_report->report,
                                        hid_report->report_len,
                                        map->forward_bit_pos);
    }

    // HID++ diverted state is authoritative for the Lift. Native Button 5 is
    // kept as a fallback for mice that already report proper held semantics.
    const bool logical_left = physical_left ||
                              physical_forward ||
                              f_poc_hidpp_forward_pressed;

    poc_write_bit(hid_report->report,
                  hid_report->report_len,
                  map->left_bit_pos,
                  logical_left);

    if (map->has_forward) {
        // Never expose the original Forward action to the USB host.
        poc_write_bit(hid_report->report,
                      hid_report->report_len,
                      map->forward_bit_pos,
                      false);
    }

    if (remember_template) {
        f_poc_mouse_template_valid = true;
        f_poc_mouse_report_id = hid_report->report_id;
        f_poc_mouse_report_len = hid_report->report_len;
        memcpy(f_poc_mouse_report, hid_report->report, hid_report->report_len);
    }
}

static bool poc_make_synthetic_mouse_report(ST_HID_RPT *synthetic,
                                            const uint8_t *descriptor,
                                            uint16_t descriptor_len)
{
    if (!f_poc_mouse_template_valid || f_poc_mouse_report_len == 0) {
        printf("[POC] Forward state changed before a mouse report template was observed\n");
        return false;
    }

    memset(synthetic, 0, sizeof(*synthetic));
    synthetic->report_id = f_poc_mouse_report_id;
    synthetic->report_len = f_poc_mouse_report_len;
    memcpy(synthetic->report, f_poc_mouse_report, f_poc_mouse_report_len);

    POC_HID_REPORT_MAP *map = poc_get_report_map(synthetic->report_id,
                                                  descriptor,
                                                  descriptor_len);
    if (!map->has_left) {
        return false;
    }

    // Reuse the last mouse state for buttons/absolute controls but clear all
    // relative fields so a synthetic press/release cannot replay old motion,
    // wheel, or pan deltas.
    for (uint8_t i = 0; i < map->relative_count; i++) {
        poc_zero_bits(synthetic->report,
                      synthetic->report_len,
                      map->relative[i].bit_pos,
                      map->relative[i].size);
    }

    poc_write_bit(synthetic->report,
                  synthetic->report_len,
                  map->left_bit_pos,
                  f_poc_physical_left_pressed || f_poc_hidpp_forward_pressed);

    if (map->has_forward) {
        poc_write_bit(synthetic->report,
                      synthetic->report_len,
                      map->forward_bit_pos,
                      false);
    }

    // Keep the zero-motion synthetic packet as the latest button-state template.
    memcpy(f_poc_mouse_report, synthetic->report, synthetic->report_len);
    return true;
}

static void poc_prepend_usb_report_id(ST_HID_RPT *hid_report)
{
    // HOGP carries Report ID in the Report Reference descriptor, not in the
    // characteristic value. main.c currently forwards raw bytes with
    // tud_hid_report(0,...), so prepend non-zero Report IDs here.
    if (hid_report == NULL || hid_report->report_id == 0) {
        return;
    }
    if (hid_report->report_len >= CMN_HID_RPT_DATA_SIZE) {
        return;
    }

    memmove(&hid_report->report[1], &hid_report->report[0], hid_report->report_len);
    hid_report->report[0] = hid_report->report_id;
    hid_report->report_len++;
    hid_report->report_id = 0;
}

static bool poc_prepare_hid_report(const ST_HID_RPT *input,
                                   ST_HID_RPT *primary,
                                   bool *emit_primary,
                                   ST_HID_RPT *synthetic,
                                   bool *emit_synthetic)
{
    *emit_primary = true;
    *emit_synthetic = false;
    memcpy(primary, input, sizeof(*primary));

    const uint16_t current_cid = poc_hids_current_cid();
    if (current_cid != 0 && current_cid != f_poc_connection_cid) {
        poc_reset_for_connection(current_cid);
    }

    const uint8_t *descriptor = get_ble_hid_report_descriptor_data();
    const uint16_t descriptor_len = get_ble_hid_report_descriptor_len();
    if (descriptor == NULL || descriptor_len == 0) {
        poc_prepend_usb_report_id(primary);
        return true;
    }

    poc_reset_report_maps_if_descriptor_changed(descriptor, descriptor_len);
    poc_hidpp_try_send();

    bool forward_state_changed = false;
    const bool consumed_hidpp = poc_hidpp_process_report(primary, &forward_state_changed);
    if (consumed_hidpp) {
        *emit_primary = false;

        // A feature response may have advanced us to NEED_SET_DIVERT.
        poc_hidpp_try_send();

        if (forward_state_changed) {
            *emit_synthetic = poc_make_synthetic_mouse_report(synthetic,
                                                               descriptor,
                                                               descriptor_len);
            if (*emit_synthetic) {
                poc_prepend_usb_report_id(synthetic);
            }
        }
        return true;
    }

    poc_apply_mouse_remap(primary, descriptor, descriptor_len, true);
    poc_prepend_usb_report_id(primary);

    // Retry any pending HID++ write after processing ordinary mouse traffic.
    poc_hidpp_try_send();
    return true;
}

static bool poc_queue_hid_unlocked(ST_QUE *pstQue, const ST_HID_RPT *report)
{
    if (pstQue->head == (pstQue->tail + 1) % pstQue->max) {
        return false;
    }

    ST_HID_RPT *pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
    memcpy(&pstHidRpt[pstQue->tail], report, sizeof(ST_HID_RPT));
    pstQue->tail = (pstQue->tail + 1) % pstQue->max;
    return true;
}

// Enqueues data into the specified queue
bool CMN_Enqueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;
    ST_QUE *pstQue = &f_astQue[iQue];

    if (iQue == CMN_QUE_KIND_HID_RPT && pData != NULL) {
        ST_HID_RPT primary;
        ST_HID_RPT synthetic;
        bool emit_primary;
        bool emit_synthetic;

        poc_prepare_hid_report((const ST_HID_RPT *)pData,
                               &primary,
                               &emit_primary,
                               &synthetic,
                               &emit_synthetic);

        CMN_EntrySpinLock();
        bRet = true;
        if (emit_primary && !poc_queue_hid_unlocked(pstQue, &primary)) {
            bRet = false;
        }
        if (emit_synthetic && !poc_queue_hid_unlocked(pstQue, &synthetic)) {
            bRet = false;
        }
        CMN_ExitSpinLock();
        return bRet;
    }

    CMN_EntrySpinLock(); // Acquire spinlock

    if ((pstQue->head == (pstQue->tail + 1) % pstQue->max)) {
        // Queue is full
    }
    else {
        // Perform queuing
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT: // HID Report Queue
            memcpy(&((ST_HID_RPT *)pstQue->pBuf)[pstQue->tail], pData, sizeof(ST_HID_RPT));
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
    }
    else {
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT:
            pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
            memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
            break;
        default:
            break;
        }
        pstQue->head = (pstQue->head + 1) % pstQue->max;
        bRet = true;
    }

    CMN_ExitSpinLock();
    return bRet;
}

// Peeks at the data from the specified queue without removing it
bool CMN_PeekQueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;
    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt;

    CMN_EntrySpinLock();

    if (pstQue->head != pstQue->tail) {
        switch (iQue) {
        case CMN_QUE_KIND_HID_RPT:
            pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;
            memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
            break;
        default:
            break;
        }
        bRet = true;
    }

    CMN_ExitSpinLock();
    return bRet;
}

// Advances the queue's read pointer (head)
void CMN_AdvanceQueue(ULONG iQue)
{
    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock();
    if (pstQue->head != pstQue->tail) {
        pstQue->head = (pstQue->head + 1) % pstQue->max;
    }
    CMN_ExitSpinLock();
}

// Clears all data from the specified queue.
void CMN_ClearQueue(ULONG iQue)
{
    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock();
    pstQue->head = 0;
    pstQue->tail = 0;
    CMN_ExitSpinLock();
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
    critical_section_init(&f_stSpinLock);
    f_astQue[CMN_QUE_KIND_HID_RPT].pBuf = (PVOID)f_astQueData_hid;
    f_astQue[CMN_QUE_KIND_HID_RPT].max  = CMN_QUE_DATA_MAX_HID_RPT;
}
