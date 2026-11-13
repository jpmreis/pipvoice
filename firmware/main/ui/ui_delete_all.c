/* Pip UI - "Delete all messages?" confirmation, reached from Settings.
 *
 * Settings already sits behind the PIN, so this screen is the second
 * look rather than a second lock: it states how many messages go (new
 * and already heard, counted from the inbox the moment it opens) and
 * puts Cancel on the side the thumb rests on. The delete itself is the
 * app's job (cb.delete_all_messages): per message it is the same local
 * unlink + trash marker the single Delete uses, so the server-side
 * removal rides the sync task's normal queue. */
#include "ui_internal.h"
#include <stdio.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_count;     /* "15 messages"                  */
static lv_obj_t *s_detail;    /* "3 new  ·  12 already heard"   */

static void cancel_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        cancel_clicked(e);
    }
}

static void delete_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    uint8_t n = g_ui.inbox_count;
    if (g_ui.cb.delete_all_messages) g_ui.cb.delete_all_messages();
    if (n == 1) ui_toast("1 message deleted", COL_TEXT_DIM);
    else {
        char buf[40];
        snprintf(buf, sizeof(buf), "%u messages deleted", (unsigned)n);
        ui_toast(buf, COL_TEXT_DIM);
    }
    nav_to(SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

lv_obj_t *scr_delete_all_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    mk_header(s_scr, "Delete all?", true, cancel_clicked);

    /* body: a column centred in the band between header and buttons */
    lv_obj_t *col = lv_obj_create(s_scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, GEO_MODAL_W,
                    SCREEN_H - GEO_LIST_TOP - GEO_PB_BTN_H - GEO_PB_BTN_BOTTOM);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, GEO_LIST_TOP);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(col);
    lv_label_set_text(icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_font(icon, FONT_BIG, 0);
    lv_obj_set_style_text_color(icon, COL_DANGER, 0);

    s_count = lv_label_create(col);
    lv_obj_set_style_text_font(s_count, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_count, COL_TEXT, 0);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_CENTER, 0);

    s_detail = lv_label_create(col);
    lv_obj_set_style_text_font(s_detail, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_detail, COL_ACCENT, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *note = lv_label_create(col);
    lv_label_set_text(note, "They are removed from this Pip\n"
                            "and from the server. No undo.");
    lv_obj_set_style_text_font(note, FONT_SMALL, 0);
    lv_obj_set_style_text_color(note, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    /* same two-button footer as the playback screen (per-board tokens) */
    lv_obj_t *cancel = mk_button(s_scr, "Cancel", COL_SURFACE2,
                                 cancel_clicked, NULL);
    lv_obj_set_size(cancel, GEO_PB_BTN_W, GEO_PB_BTN_H);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, GEO_PB_BTN_X, -GEO_PB_BTN_BOTTOM);

    lv_obj_t *del = mk_button(s_scr, LV_SYMBOL_TRASH "  Delete", COL_DANGER,
                              delete_clicked, NULL);
    lv_obj_set_style_text_color(del, COL_BG, 0);
    lv_obj_set_size(del, GEO_PB_BTN_W, GEO_PB_BTN_H);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, -GEO_PB_BTN_X, -GEO_PB_BTN_BOTTOM);

    return s_scr;
}

void scr_delete_all_refresh(void)
{
    if (!s_scr) return;
    unsigned total = g_ui.inbox_count;
    unsigned unheard = g_ui.unheard_count;
    unsigned heard = total - unheard;

    lv_label_set_text_fmt(s_count, "%u message%s", total, total == 1 ? "" : "s");
    /* U+2022 bullet: in LVGL's built-in Montserrat (U+00B7 is not) */
    lv_label_set_text_fmt(s_detail, "%u new  \xe2\x80\xa2  %u already heard",
                          unheard, heard);
}
