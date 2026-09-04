/* Pip UI - record: tap the big button, talk, send */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

typedef enum { REC_IDLE, REC_RECORDING, REC_REVIEW } rec_state_t;

static lv_obj_t   *s_scr;
static lv_obj_t   *s_header;
static lv_obj_t   *s_title;
static lv_obj_t   *s_hint;
static lv_obj_t   *s_timer_lbl;
static lv_obj_t   *s_rec_btn;
static lv_obj_t   *s_rec_icon;
static lv_obj_t   *s_send_btn;
static lv_obj_t   *s_redo_btn;
static lv_obj_t   *s_peer_lbl;   /* "<name> is recording..." */
static lv_timer_t *s_peer_ttl;   /* clears a stale indicator (lost stop) */
static rec_state_t s_state;

static void apply_state(rec_state_t st)
{
    s_state = st;
    scr_record_peer_update();   /* hidden in REVIEW (send/redo need room) */
    switch (st) {
        case REC_IDLE:
            lv_label_set_text(s_hint, "Tap to record");
            lv_label_set_text(s_timer_lbl, "0:00");
            ui_fg_label(s_timer_lbl, false);
            lv_obj_set_style_bg_color(s_rec_btn, COL_ACCENT, 0);
            lv_obj_set_style_border_width(s_rec_btn, 0, 0);
            lv_label_set_text(s_rec_icon, LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(s_rec_icon, COL_BG, 0);
            lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_redo_btn, LV_OBJ_FLAG_HIDDEN);
            break;
        case REC_RECORDING:
            lv_label_set_text(s_hint, "Recording... tap to stop");
            lv_obj_set_style_bg_color(s_rec_btn, COL_DANGER, 0);
            lv_obj_set_style_border_width(s_rec_btn, 0, 0);
            lv_label_set_text(s_rec_icon, LV_SYMBOL_STOP);
            lv_obj_set_style_text_color(s_rec_icon, COL_BG, 0);
            lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_redo_btn, LV_OBJ_FLAG_HIDDEN);
            break;
        case REC_REVIEW:
            /* inert while reviewing - but over a bright theme image the
             * dark surface + black icon vanished; light icon + a dim ring
             * keep it legible on any background */
            lv_label_set_text(s_hint, "Send it?");
            lv_obj_set_style_bg_color(s_rec_btn, COL_SURFACE2, 0);
            lv_obj_set_style_border_width(s_rec_btn, 2, 0);
            lv_obj_set_style_border_color(s_rec_btn, COL_TEXT_DIM, 0);
            lv_label_set_text(s_rec_icon, LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(s_rec_icon, COL_TEXT, 0);
            lv_obj_clear_flag(s_send_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_redo_btn, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

/* ------------- events ------------- */
static void back_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_state == REC_RECORDING && g_ui.cb.record_stop) g_ui.cb.record_stop();
    if (s_state != REC_IDLE && g_ui.cb.record_cancel)    g_ui.cb.record_cancel();
    apply_state(REC_IDLE);   /* else a later record-done event pops the
                                "Time's up" toast on the home screen */
    nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        back_clicked(e);   /* same as back: stop/cancel, then home */
    }
}

static void rec_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_state == REC_IDLE) {
        if (g_ui.cb.record_start) g_ui.cb.record_start(g_ui.selected_contact_id);
        apply_state(REC_RECORDING);
    } else if (s_state == REC_RECORDING) {
        if (g_ui.cb.record_stop) g_ui.cb.record_stop();
        apply_state(REC_REVIEW);
    }
    /* REC_REVIEW: big button inert; use Send / Redo */
}

static void send_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    /* the callback flashes its own specific error ("Still saving...",
     * "Nothing recorded") on failure - stay on this screen so the user
     * can retry instead of announcing success we can't back up */
    if (!g_ui.cb.record_send || !g_ui.cb.record_send()) return;
    ui_toast(LV_SYMBOL_OK "  Message on its way!", COL_OK);
    nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void redo_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (g_ui.cb.record_cancel) g_ui.cb.record_cancel();
    apply_state(REC_IDLE);
}

/* ------------- peer-recording indicator ------------- */
static void peer_ttl_cb(lv_timer_t *t)
{
    /* no keepalive within TTL: the recorder died mid-recording */
    LV_UNUSED(t);
    g_ui.peer_rec = false;
    scr_record_peer_update();
}

static void peer_opa_anim(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

void scr_record_peer_update(void)
{
    if (!s_peer_lbl) return;
    bool show = g_ui.peer_rec && s_state != REC_REVIEW &&
                !strcmp(g_ui.peer_rec_from, g_ui.selected_contact_id);
    if (show) {
        lv_label_set_text_fmt(s_peer_lbl, LV_SYMBOL_AUDIO "  %s is recording...",
                              g_ui.peer_rec_name);
        lv_obj_clear_flag(s_peer_lbl, LV_OBJ_FLAG_HIDDEN);
        /* pulse so it reads as live */
        lv_anim_delete(s_peer_lbl, NULL);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_peer_lbl);
        lv_anim_set_exec_cb(&a, peer_opa_anim);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_40);
        lv_anim_set_duration(&a, 550);
        lv_anim_set_playback_duration(&a, 550);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(s_peer_lbl, NULL);
        lv_obj_set_style_opa(s_peer_lbl, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_peer_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_ui.peer_rec) {          /* (re)arm the 20 s staleness timer */
        lv_timer_reset(s_peer_ttl);
        lv_timer_resume(s_peer_ttl);
    } else {
        lv_timer_pause(s_peer_ttl);
    }
}

/* ------------- build ------------- */
lv_obj_t *scr_record_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_header = mk_header(s_scr, "", true, back_clicked);

#if GEO_ROUND
    /* "To <name>" rides the header's centred cluster (there is no corner
     * back arrow for it to clear) */
    s_title = mk_header_title(s_header);
#else
    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, FONT_TITLE, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, GEO_REC_TITLE_Y);
#endif

    s_timer_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_timer_lbl, FONT_BIG, 0);
    lv_obj_align(s_timer_lbl, LV_ALIGN_TOP_MID, 0, GEO_REC_TIMER_Y);

    s_rec_btn = mk_round_button(s_scr, GEO_REC_BTN_D, COL_ACCENT,
                                rec_clicked, NULL);
    lv_obj_align(s_rec_btn, LV_ALIGN_CENTER, 0, GEO_REC_BTN_DY);
    s_rec_icon = lv_label_create(s_rec_btn);
    lv_obj_set_style_text_font(s_rec_icon, FONT_BIG, 0);
    lv_obj_set_style_text_color(s_rec_icon, COL_BG, 0);
    lv_obj_center(s_rec_icon);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_hint, COL_TEXT_DIM, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, GEO_REC_HINT_DY);

    s_peer_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_peer_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_peer_lbl, COL_DANGER, 0);
    lv_obj_align(s_peer_lbl, LV_ALIGN_CENTER, 0, GEO_REC_PEER_DY);
    lv_obj_add_flag(s_peer_lbl, LV_OBJ_FLAG_HIDDEN);

    s_peer_ttl = lv_timer_create(peer_ttl_cb, 20000, NULL);
    lv_timer_pause(s_peer_ttl);

    /* one row, margins solved per panel (1.8: 12|248|12|84|12 = 368;
     * round: the pair narrows to the bottom chord and centres); pill
     * radius so the outer corners follow the panel's round corner */
    s_send_btn = mk_button(s_scr, LV_SYMBOL_UPLOAD "  Send", COL_OK,
                           send_clicked, NULL);
    lv_obj_set_size(s_send_btn, GEO_SEND_W, GEO_SEND_H);
    lv_obj_set_style_radius(s_send_btn, GEO_SEND_H / 2, 0);
    lv_obj_align(s_send_btn, LV_ALIGN_BOTTOM_LEFT,
                 GEO_BTNROW_X, -GEO_BTNROW_BOTTOM);

    s_redo_btn = mk_button(s_scr, LV_SYMBOL_REFRESH, COL_SURFACE2,
                           redo_clicked, NULL);
    lv_obj_set_size(s_redo_btn, GEO_REDO_W, GEO_SEND_H);
    lv_obj_set_style_radius(s_redo_btn, GEO_SEND_H / 2, 0);
    lv_obj_align(s_redo_btn, LV_ALIGN_BOTTOM_RIGHT,
                 -GEO_BTNROW_X, -GEO_BTNROW_BOTTOM);

    apply_state(REC_IDLE);
    return s_scr;
}

void scr_record_show(void)
{
    lv_label_set_text_fmt(s_title, "To %s", g_ui.selected_contact_name);
    apply_state(REC_IDLE);   /* also re-evaluates the peer indicator */
}

void scr_record_tick(uint16_t elapsed_s, uint16_t max_s)
{
    if (s_state != REC_RECORDING) return;
    lv_label_set_text_fmt(s_timer_lbl, "%u:%02u", elapsed_s / 60, elapsed_s % 60);
    if (max_s && elapsed_s + 10 >= max_s) {
        lv_obj_set_style_text_color(s_timer_lbl, COL_DANGER, 0);
        lv_obj_set_style_text_opa(s_timer_lbl, LV_OPA_COVER, 0);
    } else {
        ui_fg_label(s_timer_lbl, false);
    }
}

void scr_record_apply_theme(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_image_src(s_scr, ui_bg_image(), 0);
    ui_fg_header(s_header);
    ui_fg_label(s_title, false);
    ui_fg_label(s_timer_lbl, false);
    ui_fg_label(s_hint, true);
}

void scr_record_limit(void)
{
    if (s_state == REC_RECORDING) {
        ui_toast("Time's up - ready to send", COL_ACCENT);
        apply_state(REC_REVIEW);
    }
}
