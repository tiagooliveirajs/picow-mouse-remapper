#include "canonical_hid.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "usb_descriptors.h"

#define CANON_REPORT_MAP_SLOTS 12u
#define CANON_BUTTON_COUNT 5u
#define CANON_USAGE_PAGE_GENERIC_DESKTOP 0x01u
#define CANON_USAGE_PAGE_BUTTON 0x09u
#define CANON_USAGE_PAGE_CONSUMER 0x0cu
#define CANON_USAGE_X 0x30u
#define CANON_USAGE_Y 0x31u
#define CANON_USAGE_WHEEL 0x38u
#define CANON_USAGE_AC_PAN 0x0238u

typedef struct {
    bool valid;
    uint16_t bit_pos;
    uint8_t size;
} canon_field_t;

typedef struct {
    bool used;
    uint8_t report_id;
    bool has_any_mouse_field;
    canon_field_t buttons[CANON_BUTTON_COUNT];
    canon_field_t x;
    canon_field_t y;
    canon_field_t wheel;
    canon_field_t pan;
} canon_report_map_t;

static canon_report_map_t g_maps[CANON_REPORT_MAP_SLOTS];
static const uint8_t *g_descriptor;
static uint16_t g_descriptor_len;
static uint8_t g_button_state;

static bool report_id_matches(uint16_t descriptor_report_id, uint8_t report_id)
{
    if (descriptor_report_id == HID_REPORT_ID_UNDEFINED) {
        return report_id == 0;
    }
    return descriptor_report_id == report_id;
}

static bool field_fits(const canon_field_t *field, uint16_t report_len)
{
    return field != NULL && field->valid && field->size > 0 && field->size <= 32u &&
           ((uint32_t)field->bit_pos + field->size) <= ((uint32_t)report_len * 8u);
}

static uint32_t read_unsigned_bits(const uint8_t *report,
                                   uint16_t report_len,
                                   const canon_field_t *field)
{
    if (report == NULL || !field_fits(field, report_len)) {
        return 0;
    }

    uint32_t value = 0;
    for (uint8_t bit = 0; bit < field->size; ++bit) {
        const uint16_t source_bit = (uint16_t)(field->bit_pos + bit);
        if ((report[source_bit >> 3] & (uint8_t)(1u << (source_bit & 7u))) != 0) {
            value |= (uint32_t)1u << bit;
        }
    }
    return value;
}

static int32_t read_signed_bits(const uint8_t *report,
                                uint16_t report_len,
                                const canon_field_t *field)
{
    const uint32_t raw = read_unsigned_bits(report, report_len, field);
    if (field == NULL || !field->valid || field->size == 0 || field->size >= 32u) {
        return (int32_t)raw;
    }

    const uint32_t sign_bit = (uint32_t)1u << (field->size - 1u);
    if ((raw & sign_bit) == 0) {
        return (int32_t)raw;
    }

    const uint32_t mask = ((uint32_t)1u << field->size) - 1u;
    return (int32_t)(raw | ~mask);
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static int8_t clamp_i8(int32_t value)
{
    if (value > INT8_MAX) return INT8_MAX;
    if (value < INT8_MIN) return INT8_MIN;
    return (int8_t)value;
}

static void reset_maps(void)
{
    memset(g_maps, 0, sizeof(g_maps));
}

static void ensure_descriptor(const uint8_t *descriptor, uint16_t descriptor_len)
{
    if (descriptor == g_descriptor && descriptor_len == g_descriptor_len) {
        return;
    }

    g_descriptor = descriptor;
    g_descriptor_len = descriptor_len;
    reset_maps();
}

static canon_report_map_t *get_report_map(uint8_t report_id,
                                           const uint8_t *descriptor,
                                           uint16_t descriptor_len)
{
    ensure_descriptor(descriptor, descriptor_len);

    canon_report_map_t *free_slot = NULL;
    for (size_t i = 0; i < CANON_REPORT_MAP_SLOTS; ++i) {
        if (g_maps[i].used) {
            if (g_maps[i].report_id == report_id) {
                return &g_maps[i];
            }
        } else if (free_slot == NULL) {
            free_slot = &g_maps[i];
        }
    }

    canon_report_map_t *map = free_slot != NULL ? free_slot : &g_maps[0];
    memset(map, 0, sizeof(*map));
    map->used = true;
    map->report_id = report_id;

    if (descriptor == NULL || descriptor_len == 0) {
        return map;
    }

    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator,
                                    descriptor,
                                    descriptor_len,
                                    HID_REPORT_TYPE_INPUT);

    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        if (!report_id_matches(item.report_id, report_id)) {
            continue;
        }

        // Input item bit 0 == Constant. Ignore padding/vendor constants.
        if ((item.descriptor_item.item_value & 0x01u) != 0) {
            continue;
        }

        if (item.usage_page == CANON_USAGE_PAGE_BUTTON &&
            item.usage >= 1u && item.usage <= CANON_BUTTON_COUNT &&
            item.size == 1u) {
            canon_field_t *button = &map->buttons[item.usage - 1u];
            button->valid = true;
            button->bit_pos = item.bit_pos;
            button->size = item.size;
            map->has_any_mouse_field = true;
            continue;
        }

        // Relative axes only. Absolute touch/position fields are not a mouse
        // delta source and must not be silently reinterpreted.
        const bool relative = (item.descriptor_item.item_value & 0x04u) != 0;
        if (!relative) {
            continue;
        }

        canon_field_t *field = NULL;
        if (item.usage_page == CANON_USAGE_PAGE_GENERIC_DESKTOP) {
            if (item.usage == CANON_USAGE_X) field = &map->x;
            else if (item.usage == CANON_USAGE_Y) field = &map->y;
            else if (item.usage == CANON_USAGE_WHEEL) field = &map->wheel;
        } else if (item.usage_page == CANON_USAGE_PAGE_CONSUMER &&
                   item.usage == CANON_USAGE_AC_PAN) {
            field = &map->pan;
        }

        if (field != NULL && item.size > 0u && item.size <= 32u) {
            field->valid = true;
            field->bit_pos = item.bit_pos;
            field->size = item.size;
            map->has_any_mouse_field = true;
        }
    }

    if (map->has_any_mouse_field) {
        printf("[PICO-04] canonical map report=%u buttons=%u%u%u%u%u x=%u y=%u wheel=%u pan=%u\r\n",
               report_id,
               map->buttons[0].valid ? 1u : 0u,
               map->buttons[1].valid ? 1u : 0u,
               map->buttons[2].valid ? 1u : 0u,
               map->buttons[3].valid ? 1u : 0u,
               map->buttons[4].valid ? 1u : 0u,
               map->x.valid ? 1u : 0u,
               map->y.valid ? 1u : 0u,
               map->wheel.valid ? 1u : 0u,
               map->pan.valid ? 1u : 0u);
    }

    return map;
}

static void write_mouse_report(ST_HID_RPT *output,
                               uint8_t buttons,
                               int16_t x,
                               int16_t y,
                               int8_t wheel,
                               int8_t pan)
{
    memset(output, 0, sizeof(*output));
    output->report_id = REPORT_ID_MOUSE;
    output->report_len = 7u;
    output->report[0] = buttons;
    output->report[1] = (uint8_t)(x & 0xff);
    output->report[2] = (uint8_t)(((uint16_t)x >> 8) & 0xffu);
    output->report[3] = (uint8_t)(y & 0xff);
    output->report[4] = (uint8_t)(((uint16_t)y >> 8) & 0xffu);
    output->report[5] = (uint8_t)wheel;
    output->report[6] = (uint8_t)pan;
}

void canonical_hid_init(void)
{
    g_descriptor = NULL;
    g_descriptor_len = 0;
    g_button_state = 0;
    reset_maps();
}

void canonical_hid_reset_device(void)
{
    g_descriptor = NULL;
    g_descriptor_len = 0;
    g_button_state = 0;
    reset_maps();
}

size_t canonical_hid_process_remote_report(const ST_HID_RPT *input,
                                           const uint8_t *descriptor,
                                           uint16_t descriptor_len,
                                           ST_HID_RPT *outputs,
                                           size_t output_capacity)
{
    if (input == NULL || outputs == NULL || output_capacity == 0 ||
        descriptor == NULL || descriptor_len == 0 || input->report_len == 0) {
        return 0;
    }

    uint8_t remote_report_id = input->report_id;
    const uint8_t *payload = input->report;
    uint16_t payload_len = input->report_len;

    canon_report_map_t *map = get_report_map(remote_report_id,
                                              descriptor,
                                              descriptor_len);

    // The PICO-01 bridge prepended non-zero remote Report IDs into report[] and
    // cleared report_id before queueing. Accept that legacy representation here
    // while PICO-04 removes remote layout from the USB boundary.
    if (!map->has_any_mouse_field && remote_report_id == 0 && payload_len > 1u) {
        const uint8_t candidate_id = payload[0];
        canon_report_map_t *candidate = get_report_map(candidate_id,
                                                        descriptor,
                                                        descriptor_len);
        if (candidate->has_any_mouse_field) {
            remote_report_id = candidate_id;
            map = candidate;
            payload++;
            payload_len--;
        }
    }

    (void)remote_report_id;
    if (!map->has_any_mouse_field) {
        return 0;
    }

    for (uint8_t i = 0; i < CANON_BUTTON_COUNT; ++i) {
        if (!field_fits(&map->buttons[i], payload_len)) {
            continue;
        }

        const bool pressed = read_unsigned_bits(payload,
                                                 payload_len,
                                                 &map->buttons[i]) != 0;
        const uint8_t mask = (uint8_t)(1u << i);
        if (pressed) g_button_state |= mask;
        else g_button_state &= (uint8_t)~mask;
    }

    const int16_t x = field_fits(&map->x, payload_len)
                        ? clamp_i16(read_signed_bits(payload, payload_len, &map->x)) : 0;
    const int16_t y = field_fits(&map->y, payload_len)
                        ? clamp_i16(read_signed_bits(payload, payload_len, &map->y)) : 0;
    const int8_t wheel = field_fits(&map->wheel, payload_len)
                           ? clamp_i8(read_signed_bits(payload, payload_len, &map->wheel)) : 0;
    const int8_t pan = field_fits(&map->pan, payload_len)
                         ? clamp_i8(read_signed_bits(payload, payload_len, &map->pan)) : 0;

    write_mouse_report(&outputs[0], g_button_state, x, y, wheel, pan);
    return 1;
}

size_t canonical_hid_neutralize(ST_HID_RPT *outputs, size_t output_capacity)
{
    if (outputs == NULL || output_capacity == 0) {
        g_button_state = 0;
        return 0;
    }

    if (g_button_state == 0) {
        return 0;
    }

    g_button_state = 0;
    write_mouse_report(&outputs[0], 0, 0, 0, 0, 0);
    return 1;
}
