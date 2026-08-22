/* Pip UI - inbox: list of received messages */
#include "ui_internal.h"
#include <stdio.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_header;
static lv_obj_t *s_list;
static lv_obj_t *s_empty_lbl;

static void back_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        /* swallow the release so the row under the finger doesn't
         * also get a click (which would open a message) */
        lv_indev_wait_release(lv_indev_active());
        back_clicked(e);
    }
}

static void row_clicked(lv_event_t *e)
{
    ui_message_t *m = lv_event_get_user_data(e);
    scr_playback_show(m);
    nav_to(SCR_PLAYBACK, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void row_long_pressed(lv_event_t *e)
{
    /* LVGL still fires CLICKED on release after a long press (only
     * SHORT_CLICKED is suppressed): swallow the release so the picker
     * doesn't also open the playback screen - same idiom as gesture_cb */
    lv_indev_wait_release(lv_indev_active());
    scr_react_open(lv_event_get_user_data(e));
}

lv_obj_t *scr_inbox_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_header = mk_header(s_scr, "Inbox", true, back_clicked);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, SCREEN_W - 16, SCREEN_H - 64);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_empty_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_empty_lbl, "No messages yet.\nAsk someone to send one!");
    lv_obj_set_style_text_font(s_empty_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_empty_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_empty_lbl);

    return s_scr;
}

/* quiet-hours card: first row of the list, so it scrolls with the
 * messages instead of eating a fixed strip of a small screen */
static void mk_dnd_banner(void)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(card, UI_SURFACE_OPA, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, LV_SYMBOL_BELL "  Quiet hours - Zzz");
    lv_obj_set_style_text_font(t, FONT_BODY, 0);
    lv_obj_set_style_text_color(t, COL_ACCENT, 0);

    lv_obj_t *b = lv_label_create(card);
    lv_label_set_text_fmt(b, "New messages are waiting for you\n"
                             "and arrive at %u:00.", g_ui.dnd_until_h);
    lv_obj_set_style_text_font(b, FONT_SMALL, 0);
    lv_obj_set_style_text_color(b, COL_TEXT_DIM, 0);
}

void scr_inbox_refresh(void)
{
    if (!s_scr) return;
    lv_obj_clean(s_list);
    if (g_ui.dnd) mk_dnd_banner();

    if (g_ui.inbox_count == 0) {
        /* the banner already says something: don't stack two messages */
        if (g_ui.dnd) lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    for (uint8_t i = 0; i < g_ui.inbox_count; i++) {
        ui_message_t *m = &g_ui.inbox[i];

        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_style_bg_color(row, m->heard ? COL_SURFACE : COL_SURFACE2, 0);
        /* faded when a background is set: contrast without hiding it */
        lv_obj_set_style_bg_opa(row, UI_SURFACE_OPA, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_size(row, LV_PCT(100), 76);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, m);
        lv_obj_add_event_cb(row, row_long_pressed, LV_EVENT_LONG_PRESSED, m);
        if (!m->heard) {   /* unheard: amber stroke, matches the meta line */
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, COL_ACCENT, 0);
        }

        /* sender mini-avatar - always the sender's color so it can't be
         * mistaken for someone whose avatar color is red */
        lv_obj_t *av = lv_obj_create(row);
        lv_obj_remove_style_all(av);
        lv_obj_set_size(av, 44, 44);
        lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(av, lv_color_hex(m->sender_color), 0);
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_t *ini = lv_label_create(av);
        char letter[2] = { m->sender_name[0] ? m->sender_name[0] : '?', 0 };
        lv_label_set_text(ini, letter);
        lv_obj_set_style_text_font(ini, FONT_BODY, 0);
        lv_obj_set_style_text_color(ini, COL_BG, 0);
        lv_obj_center(ini);

        lv_obj_t *who = lv_label_create(row);
        lv_label_set_text(who, m->sender_name);
        lv_obj_set_style_text_font(who, FONT_BODY, 0);
        lv_obj_set_style_text_color(who, COL_TEXT, 0);
        lv_obj_align(who, LV_ALIGN_TOP_LEFT, 58, 4);

        /* unheard: amber meta line (matches the web client) on top of the
         * lighter row background; heard rows fall back to dim */
        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text_fmt(meta, "%s   -   %u:%02u%s", m->when,
                              m->duration_s / 60, m->duration_s % 60,
                              m->heard ? "" : "   -   new");
        lv_obj_set_style_text_font(meta, FONT_SMALL, 0);
        lv_obj_set_style_text_color(meta, m->heard ? COL_TEXT_DIM : COL_ACCENT, 0);
        lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 58, -4);

        lv_obj_t *play = lv_label_create(row);
        lv_label_set_text(play, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_font(play, FONT_TITLE, 0);
        lv_obj_set_style_text_color(play, COL_ACCENT, 0);
        lv_obj_align(play, LV_ALIGN_RIGHT_MID, -6, 0);

        /* own reaction, tucked left of the play glyph */
        if (m->reaction[0]) {
            const lv_image_dsc_t *img = ui_emoji_img(m->reaction);
            if (img) {
                lv_obj_t *r = lv_image_create(row);
                lv_image_set_src(r, img);
                lv_image_set_scale(r, 96);          /* 64 px -> 24 px */
                lv_obj_set_size(r, 24, 24);
                lv_image_set_inner_align(r, LV_IMAGE_ALIGN_CENTER);
                lv_obj_align(r, LV_ALIGN_RIGHT_MID, -46, 0);
            } else if (ui_reaction_text(m->reaction)) {
                lv_obj_t *r = lv_label_create(row);
                lv_label_set_text(r, ui_reaction_text(m->reaction));
                lv_obj_set_style_text_font(r, FONT_SMALL, 0);
                lv_obj_set_style_text_color(r, COL_ACCENT, 0);
                lv_obj_align(r, LV_ALIGN_RIGHT_MID, -46, 0);
            }
        }
    }
}

void scr_inbox_apply_theme(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_image_src(s_scr, ui_bg_image(), 0);
    ui_fg_header(s_header);
    ui_fg_label(s_empty_lbl, true);
    scr_inbox_refresh();             /* re-applies the rows' faded opacity */
}
