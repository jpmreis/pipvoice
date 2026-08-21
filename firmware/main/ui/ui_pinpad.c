/* Pip UI - PIN pad guarding the settings/WiFi area */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

#define PIN_LEN 4

static lv_obj_t *s_scr;
static lv_obj_t *s_dots[PIN_LEN];
static lv_obj_t *s_msg;
static char      s_entry[PIN_LEN + 1];
static uint8_t   s_pos;

static void draw_dots(void)
{
    for (int i = 0; i < PIN_LEN; i++)
        lv_obj_set_style_bg_color(s_dots[i],
            i < s_pos ? COL_ACCENT : COL_SURFACE2, 0);
}

void scr_pinpad_reset(void)
{
    s_pos = 0;
    memset(s_entry, 0, sizeof(s_entry));
    if (s_msg) lv_label_set_text(s_msg, "Enter PIN");
    if (s_scr) draw_dots();
}

static void submit(void)
{
    bool ok = g_ui.cb.pin_check ? g_ui.cb.pin_check(s_entry) : false;
    if (ok) {
        nav_to(SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else {
        lv_label_set_text(s_msg, "Wrong PIN - try again");
        s_pos = 0;
        memset(s_entry, 0, sizeof(s_entry));
        draw_dots();
    }
}

static void gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* entered by sliding up from home: swipe up dismisses it again */
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        lv_indev_wait_release(lv_indev_active());
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_TOP);
    }
}

static void key_clicked(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);

    if (v == -1) {                       /* backspace */
        if (s_pos) { s_pos--; s_entry[s_pos] = 0; }
    } else if (v == -2) {                /* cancel */
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_TOP);
        return;
    } else if (s_pos < PIN_LEN) {
        s_entry[s_pos++] = (char)('0' + v);
    }
    draw_dots();
    if (s_pos == PIN_LEN) submit();
}

lv_obj_t *scr_pinpad_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    s_msg = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_msg, FONT_BODY, 0);
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 26);

    /* dots */
    lv_obj_t *row = lv_obj_create(s_scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SCREEN_W, 30);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, 0);
    for (int i = 0; i < PIN_LEN; i++) {
        s_dots[i] = lv_obj_create(row);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 22, 22);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
    }

    /* keypad 3x4 */
    static const char *keys[12] =
        { "1","2","3","4","5","6","7","8","9", LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE };
    static const intptr_t vals[12] = { 1,2,3,4,5,6,7,8,9, -2, 0, -1 };

    lv_obj_t *pad = lv_obj_create(s_scr);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 300, 348);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 12; i++) {
        lv_obj_t *btn = mk_round_button(pad, 80, COL_SURFACE,
                                        key_clicked, (void *)vals[i]);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, keys[i]);
        lv_obj_set_style_text_font(l, FONT_TITLE, 0);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_obj_center(l);
    }

    scr_pinpad_reset();
    return s_scr;
}
