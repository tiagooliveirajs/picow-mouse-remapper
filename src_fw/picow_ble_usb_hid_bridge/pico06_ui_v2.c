#include "pico06_ui.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "logitech_hidpp.h"
#include "pico07_pairing.h"
#include "pico_hat_ui.h"
#include "remap_engine.h"
#include "remap_profile.h"

#define LCD_SPI spi1
#define LCD_PIN_DC 8u
#define LCD_PIN_CS 9u
#define LCD_WIDTH 240u
#define LCD_HEIGHT 240u

// With TEXT_Y=8, DRAW_H=14 and LINE_ADVANCE=27, line index 8 ends at
// y=238. A tenth line would start at y=251, so nine is the physical maximum
// with the current font/spacing.
#define UI_LINE_COUNT 9u
#define UI_TEXT_MAX 20u
#define GLYPH_SCALE 2u
#define GLYPH_W 5u
#define GLYPH_H 7u
#define DRAW_W (GLYPH_W * GLYPH_SCALE)
#define DRAW_H (GLYPH_H * GLYPH_SCALE)
#define CHAR_ADVANCE 11u
#define LINE_ADVANCE 27u
#define TEXT_X 7u
#define TEXT_Y 8u

#define COLOR_BLACK        0x0000u
#define COLOR_WHITE        0xffffu
#define COLOR_MAGENTA      0xf81fu
#define COLOR_CYAN         0x07ffu
#define COLOR_YELLOW       0xffe0u
#define COLOR_GRAY         0x8410u
#define COLOR_DARK_MAGENTA 0x0801u
#define COLOR_DARK_CYAN    0x0021u
#define COLOR_DARK_YELLOW  0x0820u

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_STATUS,
    SCREEN_MODE_MENU,
    SCREEN_CUSTOM,
    SCREEN_CAPTURE,
    SCREEN_DRAG,
    SCREEN_CUSTOM_ACTIONS,
    SCREEN_CLEAR_CONFIRM,
    SCREEN_HELP,
    SCREEN_WAIT_APPLY,
    SCREEN_SUCCESS,
    SCREEN_ERROR,
    SCREEN_DEVICES,
    SCREEN_PAIRING,
    SCREEN_PAIR_RESULTS,
    SCREEN_DEVICE_DETAILS,
    SCREEN_DELETE_CONFIRM,
    SCREEN_DEVICE_SUCCESS,
} ui_screen_t;

typedef enum {
    RENDER_WAIT_LCD = 0,
    RENDER_CLEAR,
    RENDER_TEXT,
    RENDER_IDLE,
} render_phase_t;

typedef enum {
    APPLY_KIND_NONE = 0,
    APPLY_KIND_PASSTHROUGH,
    APPLY_KIND_DEFAULT,
    APPLY_KIND_CUSTOM,
    APPLY_KIND_CLEAR,
} apply_kind_t;

static ui_screen_t g_screen;
static ui_screen_t g_help_return;
static ui_screen_t g_feedback_return;
static render_phase_t g_render_phase;
static bool g_dirty;
static bool g_was_locked;
static char g_lines[UI_LINE_COUNT][UI_TEXT_MAX + 1u];
static uint16_t g_line_colors[UI_LINE_COUNT];
static uint16_t g_clear_row;
static uint8_t g_text_line;
static uint8_t g_text_char;

static remap_profile_snapshot_t g_profile;
static bool g_have_profile;
static uint32_t g_profile_revision;
static uint32_t g_hidpp_revision;
static remap_profile_config_t g_pending;

static device_profile_snapshot_t g_device;
static bool g_have_device;
static uint32_t g_device_revision;
static device_profile_catalog_t g_catalog;
static uint32_t g_catalog_revision;
static pico07_pairing_snapshot_t g_pair;
static uint32_t g_pair_revision;

static uint8_t g_mode_selection;
static remap_target_t g_capture_target;
static remap_source_t g_drag_source;
static uint8_t g_drag_selection;
static uint8_t g_action_selection;
static uint32_t g_wait_apply_id;
static apply_kind_t g_apply_kind;
static char g_error[UI_TEXT_MAX + 1u];

static uint8_t g_device_selection;
static uint8_t g_pair_selection;
static uint8_t g_detail_action;
static uint8_t g_selected_record_index;

static const uint8_t k_alpha[26][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f},{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f},{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f},{0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},{0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0a},{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
};

static const uint8_t k_digit[10][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e},{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
};

static uint8_t glyph_row(char c, uint8_t row)
{
    if (row >= GLYPH_H) return 0;
    if (c >= 'A' && c <= 'Z') return k_alpha[(uint8_t)(c - 'A')][row];
    if (c >= '0' && c <= '9') return k_digit[(uint8_t)(c - '0')][row];
    if (c == '-') return row == 3u ? 0x1fu : 0u;
    if (c == ':') return (row == 2u || row == 5u) ? 0x04u : 0u;
    if (c == '.') return row == 6u ? 0x04u : 0u;
    if (c == '=') return (row == 2u || row == 4u) ? 0x1fu : 0u;
    if (c == '/') return (uint8_t)(1u << (row > 4u ? 0u : (4u - row)));
    if (c == '>') {
        static const uint8_t rows[7] = {0x10,0x08,0x04,0x02,0x04,0x08,0x10};
        return rows[row];
    }
    return 0;
}

static void lcd_select(bool selected) { gpio_put(LCD_PIN_CS, !selected); }

static void lcd_command(uint8_t command)
{
    gpio_put(LCD_PIN_DC, false);
    lcd_select(true);
    spi_write_blocking(LCD_SPI, &command, 1);
    lcd_select(false);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0u) return;
    gpio_put(LCD_PIN_DC, true);
    lcd_select(true);
    spi_write_blocking(LCD_SPI, data, len);
    lcd_select(false);
}

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t coords[4];
    lcd_command(0x2a);
    coords[0]=(uint8_t)(x0>>8); coords[1]=(uint8_t)x0;
    coords[2]=(uint8_t)((x1-1u)>>8); coords[3]=(uint8_t)(x1-1u);
    lcd_data(coords, sizeof(coords));
    lcd_command(0x2b);
    coords[0]=(uint8_t)(y0>>8); coords[1]=(uint8_t)y0;
    coords[2]=(uint8_t)((y1-1u)>>8); coords[3]=(uint8_t)(y1-1u);
    lcd_data(coords, sizeof(coords));
    lcd_command(0x2c);
}

static uint16_t screen_background(void)
{
    switch (g_screen) {
        case SCREEN_HOME: return COLOR_DARK_MAGENTA;
        case SCREEN_HELP: return COLOR_DARK_YELLOW;
        case SCREEN_SUCCESS:
        case SCREEN_DEVICE_SUCCESS: return COLOR_DARK_CYAN;
        default: return COLOR_BLACK;
    }
}

static void clear_one_row(uint16_t row)
{
    static uint8_t pixels[LCD_WIDTH * 2u];
    const uint16_t color = screen_background();
    for (uint16_t i=0; i<LCD_WIDTH; ++i) {
        pixels[i*2u]=(uint8_t)(color>>8);
        pixels[i*2u+1u]=(uint8_t)color;
    }
    lcd_window(0u,row,LCD_WIDTH,(uint16_t)(row+1u));
    lcd_data(pixels,sizeof(pixels));
}

static void draw_glyph(uint16_t x, uint16_t y, char c, uint16_t foreground)
{
    static uint8_t pixels[DRAW_W * DRAW_H * 2u];
    const uint16_t background = screen_background();
    size_t out=0;
    for (uint8_t py=0; py<DRAW_H; ++py) {
        const uint8_t bits=glyph_row(c,(uint8_t)(py/GLYPH_SCALE));
        for (uint8_t px=0; px<DRAW_W; ++px) {
            const uint8_t sx=(uint8_t)(px/GLYPH_SCALE);
            const bool on=(bits & (uint8_t)(1u << (4u-sx))) != 0;
            const uint16_t color=on ? foreground : background;
            pixels[out++]=(uint8_t)(color>>8);
            pixels[out++]=(uint8_t)color;
        }
    }
    lcd_window(x,y,(uint16_t)(x+DRAW_W),(uint16_t)(y+DRAW_H));
    lcd_data(pixels,sizeof(pixels));
}

static void set_screen(ui_screen_t screen) { g_screen=screen; g_dirty=true; }

static void set_line(uint8_t index, uint16_t color, const char *format, ...)
{
    if (index >= UI_LINE_COUNT) return;
    va_list args; va_start(args,format);
    vsnprintf(g_lines[index],sizeof(g_lines[index]),format,args);
    va_end(args);
    g_line_colors[index]=color;
}

static void set_title(const char *format, ...)
{
    va_list args; va_start(args,format);
    vsnprintf(g_lines[0],sizeof(g_lines[0]),format,args);
    va_end(args);
    g_line_colors[0]=COLOR_MAGENTA;
}

static uint16_t option_color(bool selected, bool current)
{
    if (selected) return COLOR_YELLOW;
    if (current) return COLOR_CYAN;
    return COLOR_WHITE;
}

static pico07_device_type_t caps_type(uint32_t caps)
{
    const bool mouse=(caps & DEVICE_CAP_MOUSE)!=0;
    const bool keyboard=(caps & DEVICE_CAP_KEYBOARD)!=0;
    if (mouse && keyboard) return PICO07_TYPE_COMPOSITE;
    if (mouse) return PICO07_TYPE_MOUSE;
    if (keyboard) return PICO07_TYPE_KEYBOARD;
    return PICO07_TYPE_UNSUPPORTED;
}

static bool same_identity(uint8_t type_a,const uint8_t a[6],uint8_t type_b,const uint8_t b[6])
{
    return type_a==type_b && memcmp(a,b,6)==0;
}

static bool current_supports_mouse(void)
{
    return g_have_device && g_device.connected &&
           (g_device.capabilities & DEVICE_CAP_MOUSE)!=0;
}

static const char *source_short(remap_source_t source)
{
    switch(source){
        case REMAP_SOURCE_LEFT:return "LEFT";
        case REMAP_SOURCE_RIGHT:return "RIGHT";
        case REMAP_SOURCE_MIDDLE:return "MIDDLE";
        case REMAP_SOURCE_BACK:return "BACK";
        case REMAP_SOURCE_FORWARD:return "FORWARD";
        default:return "NONE";
    }
}

static remap_source_t source_for_target_in(const remap_profile_config_t *config,
                                           remap_target_t target,bool *found)
{
    if(config!=NULL){
        for(uint8_t i=0;i<REMAP_SOURCE_COUNT;++i){
            if(config->mappings[i]==(uint8_t)target){*found=true;return (remap_source_t)i;}
        }
    }
    *found=false; return REMAP_SOURCE_LEFT;
}

static remap_source_t source_for_target(remap_target_t target,bool *found)
{
    return source_for_target_in(&g_pending,target,found);
}

static bool pending_source_is_applied(remap_source_t source)
{
    if(!g_have_profile || g_profile.active.mode!=DEVICE_PROFILE_CUSTOM_REMAP)return false;
    if(g_pending.mappings[source]!=g_profile.active.mappings[source])return false;
    if(source==REMAP_SOURCE_BACK && g_pending.drag_fix_back!=g_profile.active.drag_fix_back)return false;
    if(source==REMAP_SOURCE_FORWARD && g_pending.drag_fix_forward!=g_profile.active.drag_fix_forward)return false;
    return g_pending.mappings[source]!=REMAP_TARGET_PASSTHROUGH;
}

static const char *policy_short(device_drag_fix_policy_t policy)
{
    switch(policy){
        case DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED:return "FORCE";
        case DEVICE_DRAG_FIX_OFF:return "OFF";
        default:return "AUTO";
    }
}

static void build_drag_status(char *line,size_t size,bool forward)
{
    const remap_source_t source=forward?REMAP_SOURCE_FORWARD:REMAP_SOURCE_BACK;
    const char *name=forward?"FWD":"BACK";
    const device_drag_fix_policy_t policy=forward?g_profile.active.drag_fix_forward:g_profile.active.drag_fix_back;
    if(g_profile.active.mode==DEVICE_PROFILE_CUSTOM_REMAP &&
       g_profile.active.mappings[source]==REMAP_TARGET_PASSTHROUGH){snprintf(line,size,"%s NO MAPPING",name);return;}
    device_profile_snapshot_t device; logitech_hidpp_snapshot_t hidpp;
    const bool have_device=device_profile_get_snapshot(&device);
    const bool have_hidpp=logitech_hidpp_get_snapshot(&hidpp);
    const uint8_t bit=forward?LOGITECH_HIDPP_SOURCE_FORWARD:LOGITECH_HIDPP_SOURCE_BACK;
    if(g_profile.active.mode==DEVICE_PROFILE_DEFAULT_REMAP){
        const device_drag_backend_t backend=have_device?(forward?device.auto_forward_backend:device.auto_back_backend):DEVICE_DRAG_BACKEND_STANDARD;
        if(backend==DEVICE_DRAG_BACKEND_STANDARD){snprintf(line,size,"%s AUTO STD",name);return;}
    }else if(policy==DEVICE_DRAG_FIX_OFF){snprintf(line,size,"%s DRAG OFF",name);return;}
    else if(policy==DEVICE_DRAG_FIX_AUTO && have_device){
        const device_drag_backend_t backend=forward?device.auto_forward_backend:device.auto_back_backend;
        if(backend==DEVICE_DRAG_BACKEND_STANDARD){snprintf(line,size,"%s AUTO STD",name);return;}
    }
    if(have_hidpp && (hidpp.failed_mask&bit)!=0)snprintf(line,size,"%s %s UNSUP",name,policy_short(policy));
    else if(have_hidpp && (hidpp.applied_mask&bit)!=0)snprintf(line,size,"%s %s HIDPP",name,policy_short(policy));
    else if(have_hidpp && (hidpp.desired_mask&bit)!=0)snprintf(line,size,"%s %s SETUP",name,policy_short(policy));
    else snprintf(line,size,"%s %s STD",name,policy_short(policy));
}

static void build_custom_binding(uint8_t line_index,const char *target_name,remap_target_t target)
{
    bool found=false; const remap_source_t source=source_for_target(target,&found);
    const uint16_t color=found && pending_source_is_applied(source)?COLOR_CYAN:COLOR_WHITE;
    set_line(line_index,color,"%s: %s",target_name,found?source_short(source):"PASS");
}

static device_drag_fix_policy_t selection_to_policy(uint8_t selection)
{
    if(selection==1u)return DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED;
    if(selection==2u)return DEVICE_DRAG_FIX_OFF;
    return DEVICE_DRAG_FIX_AUTO;
}

static uint8_t policy_to_selection(device_drag_fix_policy_t policy)
{
    if(policy==DEVICE_DRAG_FIX_FORCE_IF_SUPPORTED)return 1u;
    if(policy==DEVICE_DRAG_FIX_OFF)return 2u;
    return 0u;
}

static int applied_drag_selection(void)
{
    if(!g_have_profile || g_profile.active.mode!=DEVICE_PROFILE_CUSTOM_REMAP)return -1;
    if(g_profile.active.mappings[g_drag_source]==REMAP_TARGET_PASSTHROUGH)return -1;
    const device_drag_fix_policy_t policy=g_drag_source==REMAP_SOURCE_BACK?g_profile.active.drag_fix_back:g_profile.active.drag_fix_forward;
    return (int)policy_to_selection(policy);
}

static void build_help(void)
{
    set_title("HELP");
    if(g_help_return==SCREEN_DRAG){
        set_line(1,COLOR_WHITE,"AUTO = KNOWN FIX"); set_line(2,COLOR_WHITE,"FORCE = TRY HIDPP");
        set_line(3,COLOR_WHITE,"OFF = STANDARD HID"); set_line(4,COLOR_WHITE,"OFF HAS NO HOLD FIX");
        set_line(5,COLOR_WHITE,"APPLY SAVES CHANGES"); set_line(8,COLOR_GRAY,"K1 BACK"); return;
    }
    if(g_help_return==SCREEN_CUSTOM || g_help_return==SCREEN_CAPTURE ||
       g_help_return==SCREEN_CUSTOM_ACTIONS || g_help_return==SCREEN_CLEAR_CONFIRM){
        set_line(1,COLOR_WHITE,"JOY LEFT -> LEFT"); set_line(2,COLOR_WHITE,"JOY RIGHT -> RIGHT");
        set_line(3,COLOR_WHITE,"JOY UP -> FORWARD"); set_line(4,COLOR_WHITE,"JOY DOWN -> BACK");
        set_line(5,COLOR_WHITE,"JOY PRESS -> MIDDLE"); set_line(6,COLOR_WHITE,"K2 ACTIONS");
        set_line(8,COLOR_GRAY,"K1 BACK"); return;
    }
    if(g_help_return==SCREEN_DEVICES || g_help_return==SCREEN_DEVICE_DETAILS ||
       g_help_return==SCREEN_DELETE_CONFIRM){
        set_line(1,COLOR_WHITE,"PAIR SAVES BLE BOND"); set_line(2,COLOR_WHITE,"MOUSE HAS PROFILE");
        set_line(3,COLOR_WHITE,"KEYBOARD IS SEPARATE"); set_line(4,COLOR_WHITE,"DELETE ERASES BOND");
        set_line(5,COLOR_WHITE,"AND DEVICE PROFILE"); set_line(6,COLOR_WHITE,"GAMEPAD UNSUPPORTED");
        set_line(8,COLOR_GRAY,"K1 BACK"); return;
    }
    if(g_help_return==SCREEN_PAIRING || g_help_return==SCREEN_PAIR_RESULTS){
        set_line(1,COLOR_WHITE,"PUT HID IN PAIR MODE"); set_line(2,COLOR_WHITE,"SCAN FINDS BLE HID");
        set_line(3,COLOR_WHITE,"SELECT MOUSE/KEYBOARD"); set_line(4,COLOR_WHITE,"GAMEPAD NOT SUPPORTED");
        set_line(5,COLOR_WHITE,"K1 CANCELS PAIRING"); set_line(8,COLOR_GRAY,"K1 BACK"); return;
    }
    set_line(1,COLOR_WHITE,"JOY = LEFT OF LCD"); set_line(2,COLOR_WHITE,"K1=A DEVICES/BACK");
    set_line(3,COLOR_WHITE,"K2=B MOUSE REMAP"); set_line(4,COLOR_WHITE,"K3=X HELP");
    set_line(5,COLOR_WHITE,"K4=Y SCREEN LOCK"); set_line(6,COLOR_WHITE,"JOY PRESS = STATUS");
    set_line(8,COLOR_GRAY,"K1 BACK");
}

static void build_success(void)
{
    switch(g_apply_kind){
        case APPLY_KIND_PASSTHROUGH:set_line(1,COLOR_WHITE,"PASSTHROUGH PROFILE");set_line(2,COLOR_WHITE,"APPLIED SUCCESSFULLY");set_line(3,COLOR_WHITE,"ORIGINAL MOUSE");set_line(4,COLOR_WHITE,"BUTTONS ARE ACTIVE");break;
        case APPLY_KIND_DEFAULT:set_line(1,COLOR_WHITE,"DEFAULT PROFILE");set_line(2,COLOR_WHITE,"APPLIED SUCCESSFULLY");set_line(3,COLOR_WHITE,"LEFT NOW ESCAPE");set_line(4,COLOR_WHITE,"FWD NOW LEFT CLICK");break;
        case APPLY_KIND_CUSTOM:set_line(1,COLOR_WHITE,"CUSTOM ACTIONS");set_line(2,COLOR_WHITE,"APPLIED SUCCESSFULLY");set_line(3,COLOR_WHITE,"CUSTOM MAP SAVED");set_line(4,COLOR_WHITE,"FOR THIS MOUSE");break;
        case APPLY_KIND_CLEAR:set_line(1,COLOR_WHITE,"CLEAR MAPPINGS");set_line(2,COLOR_WHITE,"APPLIED SUCCESSFULLY");set_line(3,COLOR_WHITE,"CUSTOM BUTTONS NOW");set_line(4,COLOR_WHITE,"PASSTHROUGH");break;
        default:set_line(2,COLOR_WHITE,"APPLIED SUCCESSFULLY");break;
    }
    set_line(7,COLOR_GRAY,"K1 BACK TO OPTIONS"); set_line(8,COLOR_GRAY,"JOY PRESS STATUS");
}

static const device_profile_saved_record_t *selected_record(void)
{
    if(g_selected_record_index>=g_catalog.count)return NULL;
    return &g_catalog.records[g_selected_record_index];
}

static void build_saved_record_line(uint8_t line,uint8_t record_index,bool selected)
{
    if(record_index>=g_catalog.count)return;
    const device_profile_saved_record_t *r=&g_catalog.records[record_index];
    const pico07_device_type_t type=caps_type(r->capabilities);
    const bool current=g_have_device && g_device.connected &&
        same_identity(g_device.identity_addr_type,g_device.identity_addr,
                      r->identity_addr_type,r->identity_addr);
    const char *short_type=type==PICO07_TYPE_MOUSE?"MOUSE":
        type==PICO07_TYPE_KEYBOARD?"KEYBOARD":
        type==PICO07_TYPE_COMPOSITE?"COMPOSITE":"UNSUPPORTED";
    if(r->pnp_valid)set_line(line,option_color(selected,current),"%c %s %04X:%04X",selected?'>':' ',short_type,r->vendor_id,r->product_id);
    else set_line(line,option_color(selected,current),"%c %s %02X%02X",selected?'>':' ',short_type,r->identity_addr[4],r->identity_addr[5]);
}

static void build_lines(void)
{
    memset(g_lines,0,sizeof(g_lines));
    for(uint8_t i=0;i<UI_LINE_COUNT;++i)g_line_colors[i]=COLOR_WHITE;

    switch(g_screen){
        case SCREEN_HOME:
            set_title("HOME"); set_line(1,COLOR_WHITE,"JOY = LEFT OF LCD"); set_line(2,COLOR_WHITE,"PRESS = JOY CENTER");
            set_line(3,COLOR_WHITE,"K1=A DEVICES"); set_line(4,COLOR_WHITE,"K2=B MOUSE REMAP");
            set_line(5,COLOR_WHITE,"K3=X HELP"); set_line(6,COLOR_WHITE,"K4=Y SCREEN LOCK");
            set_line(8,COLOR_GRAY,"JOY PRESS = STATUS"); break;

        case SCREEN_STATUS:{
            set_title("STATUS");
            if(!g_have_device || !g_device.connected){set_line(1,COLOR_WHITE,"NO ACTIVE HID");set_line(2,COLOR_CYAN,"USB HID STABLE");}
            else{
                set_line(1,COLOR_CYAN,"DEVICE %s",pico07_device_type_name(caps_type(g_device.capabilities)));
                if((g_device.capabilities&DEVICE_CAP_MOUSE)!=0 && g_have_profile){
                    set_line(2,COLOR_CYAN,"PROFILE %s",remap_profile_mode_name(g_profile.active.mode));
                    char fwd[UI_TEXT_MAX+1u],back[UI_TEXT_MAX+1u]; build_drag_status(fwd,sizeof(fwd),true); build_drag_status(back,sizeof(back),false);
                    set_line(3,COLOR_WHITE,"%s",fwd); set_line(4,COLOR_WHITE,"%s",back);
                }else set_line(2,COLOR_WHITE,"KEYBOARD SAVED HID");
                set_line(5,COLOR_WHITE,"STORAGE %s",g_device.storage_ok?"OK":"ERROR");
            }
            set_line(6,COLOR_GRAY,"K1=A HOME"); set_line(7,COLOR_GRAY,"K2=B REMAP MOUSE"); set_line(8,COLOR_GRAY,"K3=X HELP"); break;}

        case SCREEN_MODE_MENU:{
            set_title("MOUSE REMAP ACTION");
            const device_profile_mode_t active=g_have_profile?g_profile.active.mode:DEVICE_PROFILE_PASSTHROUGH;
            set_line(1,option_color(g_mode_selection==0u,active==DEVICE_PROFILE_PASSTHROUGH),"%c APPLY PASSTHROUGH",g_mode_selection==0u?'>':' ');
            set_line(2,option_color(g_mode_selection==1u,active==DEVICE_PROFILE_DEFAULT_REMAP),"%c APPLY DEFAULT",g_mode_selection==1u?'>':' ');
            set_line(3,option_color(g_mode_selection==2u,active==DEVICE_PROFILE_CUSTOM_REMAP),"%c EDIT CUSTOM",g_mode_selection==2u?'>':' ');
            set_line(6,COLOR_GRAY,"JOY PRESS SELECT"); set_line(7,COLOR_GRAY,"K1 BACK/CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;}

        case SCREEN_CUSTOM:
            set_title("CUSTOM PENDING"); build_custom_binding(1,"LEFT",REMAP_TARGET_LEFT); build_custom_binding(2,"RIGHT",REMAP_TARGET_RIGHT);
            build_custom_binding(3,"FWD",REMAP_TARGET_FORWARD); build_custom_binding(4,"BACK",REMAP_TARGET_BACK); build_custom_binding(5,"MIDDLE",REMAP_TARGET_MIDDLE);
            set_line(7,COLOR_GRAY,"K2 ACTIONS"); set_line(8,COLOR_GRAY,"K1 CANCEL  K3 HELP"); break;

        case SCREEN_CAPTURE:
            set_title("SET TARGET %s",remap_target_name(g_capture_target)); set_line(2,COLOR_WHITE,"PRESS MOUSE BUTTON"); set_line(4,COLOR_WHITE,"MOVE/WHEEL IGNORED");
            set_line(7,COLOR_GRAY,"K1 CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;

        case SCREEN_DRAG:{
            set_title("DRAG FIX %s",g_drag_source==REMAP_SOURCE_FORWARD?"FORWARD":"BACK"); const int applied=applied_drag_selection();
            set_line(1,option_color(g_drag_selection==0u,applied==0),"%c AUTO",g_drag_selection==0u?'>':' ');
            set_line(2,option_color(g_drag_selection==1u,applied==1),"%c FORCE IF SUPPORTED",g_drag_selection==1u?'>':' ');
            set_line(3,option_color(g_drag_selection==2u,applied==2),"%c OFF",g_drag_selection==2u?'>':' ');
            set_line(4,COLOR_WHITE,"OFF = STANDARD HID"); set_line(5,COLOR_WHITE,"NO HELD-STATE FIX");
            set_line(7,COLOR_GRAY,"JOY PRESS SELECT"); set_line(8,COLOR_GRAY,"K1 BACK  K3 HELP"); break;}

        case SCREEN_CUSTOM_ACTIONS:
            set_title("CUSTOM ACTIONS"); set_line(1,option_color(g_action_selection==0u,false),"%c APPLY",g_action_selection==0u?'>':' ');
            set_line(2,option_color(g_action_selection==1u,false),"%c CLEAR MAPPINGS",g_action_selection==1u?'>':' ');
            set_line(5,COLOR_WHITE,"APPLY SAVES PENDING"); set_line(6,COLOR_GRAY,"JOY PRESS SELECT"); set_line(7,COLOR_GRAY,"K1 BACK/CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;

        case SCREEN_CLEAR_CONFIRM:
            set_title("CLEAR MAPPINGS"); set_line(2,COLOR_WHITE,"CLEAR SAVED CUSTOM?"); set_line(3,COLOR_WHITE,"ALL BUTTONS PASS"); set_line(4,COLOR_WHITE,"DRAG FIX -> AUTO");
            set_line(6,COLOR_GRAY,"JOY PRESS CONFIRM"); set_line(7,COLOR_GRAY,"K1 CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;

        case SCREEN_HELP: build_help(); break;
        case SCREEN_WAIT_APPLY:set_title("APPLYING");set_line(2,COLOR_WHITE,"NEUTRALIZING USB");set_line(4,COLOR_WHITE,"SAVING TO DEVICE");break;
        case SCREEN_SUCCESS:build_success();break;
        case SCREEN_ERROR:set_title("ERROR");set_line(2,COLOR_WHITE,"%s",g_error);set_line(7,COLOR_GRAY,"K1 BACK");set_line(8,COLOR_GRAY,"JOY PRESS STATUS");break;

        case SCREEN_DEVICES:{
            set_title("SAVED DEVICES");
            set_line(1,option_color(g_device_selection==0u,false),"%c PAIR NEW DEVICE",g_device_selection==0u?'>':' ');
            uint8_t first=0;
            if(g_device_selection>5u) first=(uint8_t)(g_device_selection-5u);
            for(uint8_t row=0;row<5u;++row){
                const uint8_t rec=(uint8_t)(first+row);
                if(rec>=g_catalog.count)break;
                build_saved_record_line((uint8_t)(2u+row),rec,g_device_selection==(uint8_t)(rec+1u));
            }
            set_line(7,COLOR_GRAY,"JOY PRESS SELECT"); set_line(8,COLOR_GRAY,"K1 HOME  K3 HELP"); break;}

        case SCREEN_PAIRING:
            set_title("PAIR NEW DEVICE"); set_line(1,COLOR_WHITE,"%s",g_pair.message[0]?g_pair.message:"WAITING");
            if(g_pair.state==PICO07_PAIR_SCANNING){set_line(2,COLOR_WHITE,"PUT HID IN PAIR MODE");set_line(3,COLOR_CYAN,"FOUND %u HID",g_pair.result_count);}
            else if(g_pair.state==PICO07_PAIR_CONNECTING){set_line(2,COLOR_WHITE,"CONNECTING BLE");}
            else if(g_pair.state==PICO07_PAIR_CLASSIFYING){set_line(2,COLOR_WHITE,"READING REPORT MAP");set_line(3,COLOR_WHITE,"MOUSE OR KEYBOARD");}
            set_line(7,COLOR_GRAY,"K1 CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;

        case SCREEN_PAIR_RESULTS:
            set_title("SELECT HID DEVICE");
            for(uint8_t i=0;i<g_pair.result_count && i<6u;++i){
                set_line((uint8_t)(1u+i),option_color(g_pair_selection==i,false),"%c %s",g_pair_selection==i?'>':' ',g_pair.results[i].name);
            }
            set_line(7,COLOR_GRAY,"JOY PRESS PAIR"); set_line(8,COLOR_GRAY,"K1 CANCEL  K3 HELP"); break;

        case SCREEN_DEVICE_DETAILS:{
            const device_profile_saved_record_t *r=selected_record(); set_title("DEVICE DETAILS");
            if(r==NULL){set_line(2,COLOR_WHITE,"DEVICE NOT FOUND");break;}
            const pico07_device_type_t type=caps_type(r->capabilities); set_line(1,COLOR_CYAN,"TYPE %s",pico07_device_type_name(type));
            if(r->pnp_valid)set_line(2,COLOR_WHITE,"VID %04X PID %04X",r->vendor_id,r->product_id); else set_line(2,COLOR_WHITE,"ID %02X%02X%02X%02X",r->identity_addr[2],r->identity_addr[3],r->identity_addr[4],r->identity_addr[5]);
            const bool current=g_have_device&&g_device.connected&&same_identity(g_device.identity_addr_type,g_device.identity_addr,r->identity_addr_type,r->identity_addr);
            set_line(3,current?COLOR_CYAN:COLOR_WHITE,current?"CONNECTED":"SAVED / OFFLINE");
            if((r->capabilities&DEVICE_CAP_MOUSE)!=0)set_line(4,COLOR_WHITE,"PROFILE %s",remap_profile_mode_name(r->profile_mode)); else set_line(4,COLOR_WHITE,"KEYBOARD NO REMAP");
            set_line(5,option_color(g_detail_action==0u,false),"%c CONNECT",g_detail_action==0u?'>':' ');
            set_line(6,option_color(g_detail_action==1u,false),"%c DELETE SAVED",g_detail_action==1u?'>':' ');
            set_line(7,COLOR_GRAY,"JOY PRESS SELECT"); set_line(8,COLOR_GRAY,"K1 DEVICES K3 HELP"); break;}

        case SCREEN_DELETE_CONFIRM:{
            const device_profile_saved_record_t *r=selected_record(); set_title("DELETE SAVED DEVICE");
            if(r!=NULL)set_line(1,COLOR_WHITE,"DELETE %s?",pico07_device_type_name(caps_type(r->capabilities)));
            set_line(2,COLOR_WHITE,"ERASE BLE BOND"); set_line(3,COLOR_WHITE,"ERASE DEVICE PROFILE"); set_line(4,COLOR_WHITE,"ERASE REMAP OVERRIDE");
            set_line(6,COLOR_GRAY,"JOY PRESS CONFIRM"); set_line(7,COLOR_GRAY,"K1 CANCEL"); set_line(8,COLOR_GRAY,"K3 HELP"); break;}

        case SCREEN_DEVICE_SUCCESS:
            set_line(1,COLOR_WHITE,"%s",g_pair.message[0]?g_pair.message:"DEVICE UPDATED");
            if(g_pair.last_type!=PICO07_TYPE_UNKNOWN)set_line(2,COLOR_WHITE,"TYPE %s",pico07_device_type_name(g_pair.last_type));
            if(g_pair.last_type==PICO07_TYPE_KEYBOARD && g_pair.last_was_new_pair){set_line(3,COLOR_WHITE,"BLE BOND IS SAVED");set_line(4,COLOR_WHITE,"SIMULTANEOUS NEXT GATE");}
            else set_line(3,COLOR_WHITE,"SAVED DEVICES UPDATED");
            set_line(7,COLOR_GRAY,"K1 BACK TO DEVICES"); set_line(8,COLOR_GRAY,"JOY PRESS STATUS"); break;
    }
}

static void begin_render(void){build_lines();g_clear_row=0;g_text_line=0;g_text_char=0;g_render_phase=RENDER_CLEAR;g_dirty=false;}

static void render_task(void)
{
    if(g_dirty)begin_render();
    switch(g_render_phase){
        case RENDER_WAIT_LCD:begin_render();break;
        case RENDER_CLEAR:clear_one_row(g_clear_row++);if(g_clear_row>=LCD_HEIGHT)g_render_phase=RENDER_TEXT;break;
        case RENDER_TEXT:{
            while(g_text_line<UI_LINE_COUNT && g_lines[g_text_line][g_text_char]=='\0'){++g_text_line;g_text_char=0;}
            if(g_text_line>=UI_LINE_COUNT){g_render_phase=RENDER_IDLE;break;}
            const char c=g_lines[g_text_line][g_text_char];
            if(c!=' '){const uint16_t x=(uint16_t)(TEXT_X+(uint16_t)g_text_char*CHAR_ADVANCE);const uint16_t y=(uint16_t)(TEXT_Y+(uint16_t)g_text_line*LINE_ADVANCE);
                if(x+DRAW_W<=LCD_WIDTH && y+DRAW_H<=LCD_HEIGHT)draw_glyph(x,y,c,g_line_colors[g_text_line]);}
            ++g_text_char;break;}
        default:break;
    }
}

static void drain_source_events(void){remap_source_t source;while(remap_engine_poll_source_down(&source)){(void)source;}}
static void start_capture(remap_target_t target){drain_source_events();g_capture_target=target;set_screen(SCREEN_CAPTURE);}

static void show_error(const char *message,ui_screen_t return_screen)
{
    snprintf(g_error,sizeof(g_error),"%s",message);g_feedback_return=return_screen;set_screen(SCREEN_ERROR);
}

static void request_apply(const remap_profile_config_t *config,apply_kind_t kind,ui_screen_t return_screen)
{
    uint32_t id=0;if(!remap_profile_request_apply(config,&id)){show_error("APPLY BUSY/INVALID",return_screen);return;}
    g_wait_apply_id=id;g_apply_kind=kind;g_feedback_return=return_screen;set_screen(SCREEN_WAIT_APPLY);
}

static void select_mode(void)
{
    remap_profile_config_t config;if(g_have_profile)config=g_profile.active;else remap_profile_make_passthrough(&config);
    if(g_mode_selection==0u){config.mode=DEVICE_PROFILE_PASSTHROUGH;request_apply(&config,APPLY_KIND_PASSTHROUGH,SCREEN_MODE_MENU);}
    else if(g_mode_selection==1u){config.mode=DEVICE_PROFILE_DEFAULT_REMAP;config.drag_fix_back=DEVICE_DRAG_FIX_AUTO;config.drag_fix_forward=DEVICE_DRAG_FIX_AUTO;request_apply(&config,APPLY_KIND_DEFAULT,SCREEN_MODE_MENU);}
    else{g_pending=config;g_pending.mode=DEVICE_PROFILE_CUSTOM_REMAP;set_screen(SCREEN_CUSTOM);}
}

static void capture_source(remap_source_t source)
{
    for(uint8_t i=0;i<REMAP_SOURCE_COUNT;++i)if(g_pending.mappings[i]==(uint8_t)g_capture_target)g_pending.mappings[i]=REMAP_TARGET_PASSTHROUGH;
    g_pending.mappings[source]=(uint8_t)g_capture_target;
    if(source==REMAP_SOURCE_BACK||source==REMAP_SOURCE_FORWARD){g_drag_source=source;const device_drag_fix_policy_t policy=source==REMAP_SOURCE_BACK?g_pending.drag_fix_back:g_pending.drag_fix_forward;g_drag_selection=policy_to_selection(policy);set_screen(SCREEN_DRAG);}else set_screen(SCREEN_CUSTOM);
}

static void move_device_selection(bool down)
{
    const uint8_t max=g_catalog.count;
    if(down)g_device_selection=g_device_selection>=max?0u:(uint8_t)(g_device_selection+1u);
    else g_device_selection=g_device_selection==0u?max:(uint8_t)(g_device_selection-1u);
    g_dirty=true;
}

static void open_selected_device(void)
{
    if(g_device_selection==0u){if(!pico07_pairing_request_scan())show_error("PAIR COMMAND BUSY",SCREEN_DEVICES);else set_screen(SCREEN_PAIRING);return;}
    g_selected_record_index=(uint8_t)(g_device_selection-1u);g_detail_action=0u;set_screen(SCREEN_DEVICE_DETAILS);
}

static void handle_pressed(pico_hat_input_t input)
{
    if(input==PICO_HAT_INPUT_KEY4)return;
    if(input==PICO_HAT_INPUT_KEY3 && g_screen!=SCREEN_HELP && g_screen!=SCREEN_WAIT_APPLY && g_screen!=SCREEN_SUCCESS && g_screen!=SCREEN_DEVICE_SUCCESS && g_screen!=SCREEN_ERROR){g_help_return=g_screen;set_screen(SCREEN_HELP);return;}

    switch(g_screen){
        case SCREEN_HOME:
            if(input==PICO_HAT_INPUT_JOY_PRESS)set_screen(SCREEN_STATUS);
            else if(input==PICO_HAT_INPUT_KEY1){g_device_selection=0u;set_screen(SCREEN_DEVICES);}
            else if(input==PICO_HAT_INPUT_KEY2){if(!current_supports_mouse())show_error("CONNECT MOUSE FIRST",SCREEN_HOME);else{g_mode_selection=g_have_profile?(uint8_t)g_profile.active.mode:0u;if(g_mode_selection>2u)g_mode_selection=0u;set_screen(SCREEN_MODE_MENU);}}
            break;
        case SCREEN_STATUS:
            if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_HOME);
            else if(input==PICO_HAT_INPUT_KEY2){if(!current_supports_mouse())show_error("CONNECT MOUSE FIRST",SCREEN_STATUS);else{g_mode_selection=g_have_profile?(uint8_t)g_profile.active.mode:0u;if(g_mode_selection>2u)g_mode_selection=0u;set_screen(SCREEN_MODE_MENU);}}
            break;
        case SCREEN_MODE_MENU:
            if(input==PICO_HAT_INPUT_JOY_UP){g_mode_selection=g_mode_selection==0u?2u:(uint8_t)(g_mode_selection-1u);g_dirty=true;}
            else if(input==PICO_HAT_INPUT_JOY_DOWN){g_mode_selection=(uint8_t)((g_mode_selection+1u)%3u);g_dirty=true;}
            else if(input==PICO_HAT_INPUT_JOY_PRESS)select_mode();else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_HOME);break;
        case SCREEN_CUSTOM:
            if(input==PICO_HAT_INPUT_JOY_LEFT)start_capture(REMAP_TARGET_LEFT);else if(input==PICO_HAT_INPUT_JOY_RIGHT)start_capture(REMAP_TARGET_RIGHT);else if(input==PICO_HAT_INPUT_JOY_UP)start_capture(REMAP_TARGET_FORWARD);else if(input==PICO_HAT_INPUT_JOY_DOWN)start_capture(REMAP_TARGET_BACK);else if(input==PICO_HAT_INPUT_JOY_PRESS)start_capture(REMAP_TARGET_MIDDLE);else if(input==PICO_HAT_INPUT_KEY2){g_action_selection=0u;set_screen(SCREEN_CUSTOM_ACTIONS);}else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_MODE_MENU);break;
        case SCREEN_CAPTURE:if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_CUSTOM);break;
        case SCREEN_DRAG:
            if(input==PICO_HAT_INPUT_JOY_UP){g_drag_selection=g_drag_selection==0u?2u:(uint8_t)(g_drag_selection-1u);g_dirty=true;}else if(input==PICO_HAT_INPUT_JOY_DOWN){g_drag_selection=(uint8_t)((g_drag_selection+1u)%3u);g_dirty=true;}else if(input==PICO_HAT_INPUT_JOY_PRESS){const device_drag_fix_policy_t policy=selection_to_policy(g_drag_selection);if(g_drag_source==REMAP_SOURCE_BACK)g_pending.drag_fix_back=policy;else g_pending.drag_fix_forward=policy;set_screen(SCREEN_CUSTOM);}else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_CUSTOM);break;
        case SCREEN_CUSTOM_ACTIONS:
            if(input==PICO_HAT_INPUT_JOY_UP||input==PICO_HAT_INPUT_JOY_DOWN){g_action_selection=(uint8_t)(1u-g_action_selection);g_dirty=true;}else if(input==PICO_HAT_INPUT_JOY_PRESS){if(g_action_selection==0u){g_pending.mode=DEVICE_PROFILE_CUSTOM_REMAP;request_apply(&g_pending,APPLY_KIND_CUSTOM,SCREEN_CUSTOM_ACTIONS);}else set_screen(SCREEN_CLEAR_CONFIRM);}else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_CUSTOM);break;
        case SCREEN_CLEAR_CONFIRM:
            if(input==PICO_HAT_INPUT_JOY_PRESS){memset(g_pending.mappings,0,sizeof(g_pending.mappings));g_pending.mode=DEVICE_PROFILE_CUSTOM_REMAP;g_pending.drag_fix_back=DEVICE_DRAG_FIX_AUTO;g_pending.drag_fix_forward=DEVICE_DRAG_FIX_AUTO;request_apply(&g_pending,APPLY_KIND_CLEAR,SCREEN_CUSTOM_ACTIONS);}else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_CUSTOM_ACTIONS);break;
        case SCREEN_HELP:if(input==PICO_HAT_INPUT_KEY1||input==PICO_HAT_INPUT_KEY3||input==PICO_HAT_INPUT_JOY_PRESS)set_screen(g_help_return);break;
        case SCREEN_SUCCESS:if(input==PICO_HAT_INPUT_KEY1)set_screen(g_feedback_return);else if(input==PICO_HAT_INPUT_JOY_PRESS)set_screen(SCREEN_STATUS);break;
        case SCREEN_ERROR:if(input==PICO_HAT_INPUT_KEY1)set_screen(g_feedback_return);else if(input==PICO_HAT_INPUT_JOY_PRESS)set_screen(SCREEN_STATUS);break;

        case SCREEN_DEVICES:
            if(input==PICO_HAT_INPUT_JOY_UP)move_device_selection(false);else if(input==PICO_HAT_INPUT_JOY_DOWN)move_device_selection(true);else if(input==PICO_HAT_INPUT_JOY_PRESS)open_selected_device();else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_HOME);break;
        case SCREEN_PAIRING:
            if(input==PICO_HAT_INPUT_KEY1){(void)pico07_pairing_request_cancel();set_screen(SCREEN_DEVICES);}break;
        case SCREEN_PAIR_RESULTS:
            if(input==PICO_HAT_INPUT_JOY_UP){g_pair_selection=g_pair_selection==0u?(g_pair.result_count? (uint8_t)(g_pair.result_count-1u):0u):(uint8_t)(g_pair_selection-1u);g_dirty=true;}
            else if(input==PICO_HAT_INPUT_JOY_DOWN){if(g_pair.result_count)g_pair_selection=(uint8_t)((g_pair_selection+1u)%g_pair.result_count);g_dirty=true;}
            else if(input==PICO_HAT_INPUT_JOY_PRESS){if(!pico07_pairing_request_connect_result(g_pair_selection))show_error("PAIR COMMAND BUSY",SCREEN_PAIR_RESULTS);else set_screen(SCREEN_PAIRING);}
            else if(input==PICO_HAT_INPUT_KEY1){(void)pico07_pairing_request_cancel();set_screen(SCREEN_DEVICES);}break;
        case SCREEN_DEVICE_DETAILS:{
            const device_profile_saved_record_t *r=selected_record();
            if(input==PICO_HAT_INPUT_JOY_UP||input==PICO_HAT_INPUT_JOY_DOWN){g_detail_action=(uint8_t)(1u-g_detail_action);g_dirty=true;}
            else if(input==PICO_HAT_INPUT_JOY_PRESS && r!=NULL){if(g_detail_action==0u){if(!pico07_pairing_request_connect_saved(r->identity_addr_type,r->identity_addr))show_error("DEVICE COMMAND BUSY",SCREEN_DEVICE_DETAILS);else set_screen(SCREEN_PAIRING);}else set_screen(SCREEN_DELETE_CONFIRM);}
            else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_DEVICES);break;}
        case SCREEN_DELETE_CONFIRM:{
            const device_profile_saved_record_t *r=selected_record();
            if(input==PICO_HAT_INPUT_JOY_PRESS && r!=NULL){if(!pico07_pairing_request_delete_saved(r->identity_addr_type,r->identity_addr))show_error("DELETE COMMAND BUSY",SCREEN_DEVICE_DETAILS);else set_screen(SCREEN_PAIRING);}else if(input==PICO_HAT_INPUT_KEY1)set_screen(SCREEN_DEVICE_DETAILS);break;}
        case SCREEN_DEVICE_SUCCESS:if(input==PICO_HAT_INPUT_KEY1){g_device_selection=0u;set_screen(SCREEN_DEVICES);}else if(input==PICO_HAT_INPUT_JOY_PRESS)set_screen(SCREEN_STATUS);break;
        case SCREEN_WAIT_APPLY:default:break;
    }
}

static void refresh_snapshots(void)
{
    remap_profile_snapshot_t profile;
    if(remap_profile_get_snapshot(&profile) && profile.revision!=g_profile_revision){g_profile_revision=profile.revision;g_profile=profile;g_have_profile=true;if(g_screen==SCREEN_STATUS)g_dirty=true;if(g_screen==SCREEN_WAIT_APPLY&&g_wait_apply_id!=0u&&profile.last_apply_id==g_wait_apply_id){g_wait_apply_id=0u;if(profile.last_apply_ok)set_screen(SCREEN_SUCCESS);else show_error("APPLY FAILED",g_feedback_return);}}
    logitech_hidpp_snapshot_t hidpp;if(logitech_hidpp_get_snapshot(&hidpp)&&hidpp.revision!=g_hidpp_revision){g_hidpp_revision=hidpp.revision;if(g_screen==SCREEN_STATUS)g_dirty=true;}
    device_profile_snapshot_t device;if(device_profile_get_snapshot(&device)&&device.revision!=g_device_revision){g_device_revision=device.revision;g_device=device;g_have_device=true;if(g_screen==SCREEN_STATUS||g_screen==SCREEN_DEVICE_DETAILS||g_screen==SCREEN_DEVICES)g_dirty=true;}
    device_profile_catalog_t catalog;if(device_profile_get_catalog(&catalog)&&catalog.revision!=g_catalog_revision){g_catalog_revision=catalog.revision;g_catalog=catalog;if(g_device_selection>g_catalog.count)g_device_selection=g_catalog.count;if(g_screen==SCREEN_DEVICES||g_screen==SCREEN_DEVICE_DETAILS||g_screen==SCREEN_DELETE_CONFIRM)g_dirty=true;}
    pico07_pairing_snapshot_t pair;if(pico07_pairing_get_snapshot(&pair)&&pair.revision!=g_pair_revision){g_pair_revision=pair.revision;g_pair=pair;
        if((g_screen==SCREEN_PAIRING||g_screen==SCREEN_PAIR_RESULTS||g_screen==SCREEN_DELETE_CONFIRM||g_screen==SCREEN_DEVICE_DETAILS)){
            if(pair.state==PICO07_PAIR_RESULTS){g_pair_selection=0u;set_screen(SCREEN_PAIR_RESULTS);}
            else if(pair.state==PICO07_PAIR_SUCCESS)set_screen(SCREEN_DEVICE_SUCCESS);
            else if(pair.state==PICO07_PAIR_ERROR)show_error(pair.message[0]?pair.message:"DEVICE ERROR",SCREEN_DEVICES);
            else g_dirty=true;
        }
    }
}

void pico06_ui_init(void)
{
    memset(&g_profile,0,sizeof(g_profile));memset(&g_device,0,sizeof(g_device));memset(&g_catalog,0,sizeof(g_catalog));memset(&g_pair,0,sizeof(g_pair));
    remap_profile_make_passthrough(&g_pending);pico07_pairing_init();
    g_have_profile=false;g_have_device=false;g_profile_revision=0u;g_hidpp_revision=0u;g_device_revision=0u;g_catalog_revision=0u;g_pair_revision=0u;
    g_screen=SCREEN_HOME;g_help_return=SCREEN_HOME;g_feedback_return=SCREEN_HOME;g_render_phase=RENDER_WAIT_LCD;g_dirty=true;g_was_locked=false;g_wait_apply_id=0u;g_apply_kind=APPLY_KIND_NONE;
    g_device_selection=0u;g_pair_selection=0u;g_detail_action=0u;g_selected_record_index=0u;memset(g_error,0,sizeof(g_error));
}

void pico06_ui_task(void)
{
    if(!pico_hat_ui_is_lcd_ready())return;
    const bool locked=pico_hat_ui_is_screen_locked();if(locked){g_was_locked=true;return;}
    if(g_was_locked){g_was_locked=false;drain_source_events();set_screen(SCREEN_HOME);}
    refresh_snapshots();
    if(g_screen==SCREEN_CAPTURE){remap_source_t source;if(remap_engine_poll_source_down(&source))capture_source(source);}
    pico_hat_event_t event;if(pico_hat_ui_poll_event(&event)&&event.pressed)handle_pressed(event.input);
    render_task();
}
