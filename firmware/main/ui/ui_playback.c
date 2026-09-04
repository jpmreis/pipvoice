/* Pip UI - playback: listen, then Delete or Reply; leaving keeps the
 * message in the inbox */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_who;
static lv_obj_t *s_when;
static lv_obj_t *s_arc;         /* progress ring around play button */
static lv_obj_t *s_play_btn;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_time_lbl;
static lv_obj_t *s_del_btn;
static lv_obj_t *s_reply_btn;
static char      s_sender_id[UI_ID_LEN];
static char      s_sender_name[UI_NAME_LEN];
static uint16_t  s_total_s;
static bool      s_playing;

static void set_playing(bool on)
{
    s_playing = on;
    lv_label_set_text(s_play_icon, on ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void stop_if_playing(void)
{
    if (s_playing && g_ui.cb.stop_playback) g_ui.cb.stop_playback();
    set_playing(false);
}

/* ------------- events ------------- */
static void back_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    stop_if_playing();
    nav_to(SCR_INBOX, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void play_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_playing) {
        if (g_ui.cb.play_message) g_ui.cb.play_message(g_ui.playing_msg_id);
        set_playing(true);
    } else {
        stop_if_playing();
    }
}

static void del_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    stop_if_playing();
    if (g_ui.cb.delete_message) g_ui.cb.delete_message(g_ui.playing_msg_id);
    ui_toast("Message deleted", COL_TEXT_DIM);
    nav_to(SCR_INBOX, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void reply_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    /* find the sender among the contacts (by id; name as fallback for
     * messages stored before sender_id existed) */
    const ui_contact_t *c = NULL;
    for (uint8_t i = 0; i < g_ui.contact_count && !c; i++)
        if (s_sender_id[0] && !strcmp(g_ui.contacts[i].id, s_sender_id))
            c = &g_ui.contacts[i];
    for (uint8_t i = 0; i < g_ui.contact_count && !c; i++)
        if (!strcmp(g_ui.contacts[i].name, s_sender_name))
            c = &g_ui.contacts[i];
    if (!c) {
        ui_toast("Contact unavailable", COL_DANGER);
        return;
    }
    stop_if_playing();
    strlcpy(g_ui.selected_contact_id, c->id, UI_ID_LEN);
    strlcpy(g_ui.selected_contact_name, c->name, UI_NAME_LEN);
    scr_record_show();
    nav_to(SCR_RECORD, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* ------------- build ------------- */
lv_obj_t *scr_playback_create(void)
{
    s_scr = mk_screen();
    lv_obj_t *hdr = mk_header(s_scr, "", true, back_clicked);

#if GEO_ROUND
    /* "From <name>" rides the header's centred cluster */
    s_who = mk_header_title(hdr);
#else
    LV_UNUSED(hdr);
    s_who = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_who, FONT_TITLE, 0);
    lv_obj_align(s_who, LV_ALIGN_TOP_MID, 0, GEO_PB_WHO_Y);
#endif

    s_when = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_when, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_when, COL_TEXT_DIM, 0);
    lv_obj_align(s_when, LV_ALIGN_TOP_MID, 0, GEO_PB_WHEN_Y);

    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, GEO_PB_ARC_D, GEO_PB_ARC_D);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, GEO_PB_ARC_DY);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_arc, COL_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);

    s_play_btn = mk_round_button(s_scr, GEO_PB_PLAY_D, COL_ACCENT,
                                 play_clicked, NULL);
    lv_obj_align(s_play_btn, LV_ALIGN_CENTER, 0, GEO_PB_ARC_DY);
    s_play_icon = lv_label_create(s_play_btn);
    lv_obj_set_style_text_font(s_play_icon, FONT_BIG, 0);
    lv_obj_set_style_text_color(s_play_icon, COL_BG, 0);
    lv_obj_center(s_play_icon);

    s_time_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_time_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_time_lbl, COL_TEXT_DIM, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, GEO_PB_TIME_DY);

    s_del_btn = mk_button(s_scr, LV_SYMBOL_TRASH "  Delete", COL_SURFACE,
                          del_clicked, NULL);
    lv_obj_set_style_text_color(s_del_btn, COL_DANGER, 0);
    lv_obj_set_size(s_del_btn, GEO_PB_BTN_W, GEO_PB_BTN_H);
    lv_obj_align(s_del_btn, LV_ALIGN_BOTTOM_LEFT,
                 GEO_PB_BTN_X, -GEO_PB_BTN_BOTTOM);

    s_reply_btn = mk_button(s_scr, LV_SYMBOL_NEW_LINE "  Reply", COL_ACCENT,
                            reply_clicked, NULL);
    lv_obj_set_style_text_color(s_reply_btn, COL_BG, 0);
    lv_obj_set_size(s_reply_btn, GEO_PB_BTN_W, GEO_PB_BTN_H);
    lv_obj_align(s_reply_btn, LV_ALIGN_BOTTOM_RIGHT,
                 -GEO_PB_BTN_X, -GEO_PB_BTN_BOTTOM);

    return s_scr;
}

void scr_playback_show(const ui_message_t *msg)
{
    strlcpy(g_ui.playing_msg_id, msg->id, UI_ID_LEN);
    strlcpy(s_sender_id, msg->sender_id, UI_ID_LEN);
    strlcpy(s_sender_name, msg->sender_name, UI_NAME_LEN);
    s_total_s = msg->duration_s;
    lv_label_set_text_fmt(s_who, "From %s", msg->sender_name);
    lv_label_set_text(s_when, msg->when);
    lv_label_set_text_fmt(s_time_lbl, "0:00 / %u:%02u",
                          s_total_s / 60, s_total_s % 60);
    lv_arc_set_value(s_arc, 0);
    set_playing(false);
    if (g_ui.cb.message_heard) g_ui.cb.message_heard(msg->id);
}

void scr_playback_progress(uint16_t pos_s, uint16_t total_s)
{
    if (total_s) s_total_s = total_s;
    lv_label_set_text_fmt(s_time_lbl, "%u:%02u / %u:%02u",
                          pos_s / 60, pos_s % 60,
                          s_total_s / 60, s_total_s % 60);
    lv_arc_set_value(s_arc, s_total_s ? (pos_s * 100) / s_total_s : 0);
}

void scr_playback_finished(void)
{
    set_playing(false);
    lv_arc_set_value(s_arc, 100);
    lv_label_set_text(s_play_icon, LV_SYMBOL_REFRESH);  /* invite replay */
}
