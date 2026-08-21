/* Pip UI - reactions: long-press picker, reaction toast, contact badges.
 * Emoji are baked-in RGB565A8 bitmaps (tools/gen_emoji.py); the "Ha Ha",
 * "!!", "?" reactions are plain Montserrat text, matching the PWA. */
#include "ui_internal.h"
#include "assets/emoji_assets.h"
#include <stdio.h>
#include <string.h>

const lv_image_dsc_t *ui_emoji_img(const char *key)
{
    if (!key) return NULL;
    if (!strcmp(key, "heart")) return &emoji_heart;
    if (!strcmp(key, "up"))    return &emoji_up;
    if (!strcmp(key, "down"))  return &emoji_down;
    if (!strcmp(key, "joy"))   return &emoji_joy;
    return NULL;
}

const char *ui_reaction_text(const char *key)
{
    if (!key) return NULL;
    if (!strcmp(key, "haha"))  return "Ha Ha";
    if (!strcmp(key, "bang"))  return "!!";
    if (!strcmp(key, "quest")) return "?";
    return NULL;
}

/* ---------------- contact badges (sender side) ---------------- */
const ui_reaction_t *ui_react_badge_for(const char *contact_id)
{
    for (uint8_t i = 0; i < g_ui.reaction_badge_count; i++)
        if (!strcmp(g_ui.reaction_badges[i].from, contact_id))
            return &g_ui.reaction_badges[i];
    return NULL;
}

void ui_react_mark_seen(const char *contact_id)
{
    for (uint8_t i = 0; i < g_ui.reaction_badge_count; i++) {
        if (strcmp(g_ui.reaction_badges[i].from, contact_id)) continue;
        /* no scr_home_refresh here: this runs inside the tapped tile's
         * own event and a rebuild would delete the dispatching widget;
         * the home screen re-renders on the way back anyway */
        for (uint8_t j = i; j + 1 < g_ui.reaction_badge_count; j++)
            g_ui.reaction_badges[j] = g_ui.reaction_badges[j + 1];
        g_ui.reaction_badge_count--;
        if (g_ui.cb.reactions_seen) g_ui.cb.reactions_seen(contact_id);
        return;
    }
}

/* ---------------- picker overlay ---------------- */
static lv_obj_t *s_scrim;
static char      s_pick_id[UI_ID_LEN];
static char      s_pick_cur[UI_REACT_KEY_LEN];

/* picker order mirrors the iMessage tapback row */
static const char *const REACT_KEYS[7] =
    { "heart", "up", "down", "joy", "haha", "bang", "quest" };

static void react_close(void)
{
    if (!s_scrim) return;
    /* async: we may be inside an event of one of the scrim's children */
    lv_obj_delete_async(s_scrim);
    s_scrim = NULL;
}

static void scrim_clicked(lv_event_t *e)
{
    /* only a direct tap on the backdrop closes without picking */
    if (lv_event_get_target(e) == lv_event_get_current_target(e))
        react_close();
}

static void opt_clicked(lv_event_t *e)
{
    const char *key = lv_event_get_user_data(e);
    /* picking the current reaction again clears it */
    const char *send = strcmp(key, s_pick_cur) ? key : "";
    if (g_ui.cb.react) g_ui.cb.react(s_pick_id, send);
    react_close();
}

void scr_react_open(const ui_message_t *m)
{
    if (s_scrim) return;                    /* already open */
    strlcpy(s_pick_id, m->id, sizeof(s_pick_id));
    strlcpy(s_pick_cur, m->reaction, sizeof(s_pick_cur));

    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_scrim, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_set_style_pad_column(panel, 10, 0);
    lv_obj_set_size(panel, SCREEN_W - 24, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 7; i++) {
        const char *key = REACT_KEYS[i];
        const lv_image_dsc_t *img = ui_emoji_img(key);

        lv_obj_t *btn = lv_button_create(panel);
        lv_obj_set_style_bg_color(btn, COL_SURFACE, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, opt_clicked, LV_EVENT_CLICKED, (void *)key);
        if (!strcmp(key, s_pick_cur)) {     /* current pick: accent ring */
            lv_obj_set_style_border_width(btn, 3, 0);
            lv_obj_set_style_border_color(btn, COL_ACCENT, 0);
        }
        if (img) {
            lv_obj_set_size(btn, 76, 76);
            lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_t *e = lv_image_create(btn);
            lv_image_set_src(e, img);
            lv_obj_center(e);
        } else {
            lv_obj_set_size(btn, LV_SIZE_CONTENT, 60);
            lv_obj_set_style_radius(btn, 30, 0);
            lv_obj_set_style_pad_hor(btn, 20, 0);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, ui_reaction_text(key));
            lv_obj_set_style_text_font(l, FONT_BODY, 0);
            lv_obj_set_style_text_color(l, COL_ACCENT, 0);
            lv_obj_center(l);
        }
    }
}

/* ---------------- reaction toast (sender side) ---------------- */
/* ui_toast() can't render emoji (text-only, Montserrat); same box +
 * timing, with the reaction bitmap or text chip inline. */
static void rtoast_del_cb(lv_timer_t *t)
{
    lv_obj_t *box = lv_timer_get_user_data(t);
    lv_obj_delete(box);
    lv_timer_delete(t);
}

static void rtoast_dismiss_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_current_target(e);
    lv_timer_t *t = lv_obj_get_user_data(box);
    if (!t) return;
    lv_timer_delete(t);
    lv_obj_set_user_data(box, NULL);
    lv_obj_delete_async(box);
}

void ui_notify_reaction(const char *from_name, const char *key)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_style_bg_color(box, COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, COL_ACCENT, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_column(box, 10, 0);
    lv_obj_set_width(box, SCREEN_W - 40);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_image_dsc_t *img = ui_emoji_img(key);
    if (img) {
        lv_obj_t *e = lv_image_create(box);
        lv_image_set_src(e, img);
        lv_image_set_scale(e, 128);              /* 64 px -> 32 px */
        /* scale shrinks the render, not the layout box: pin it */
        lv_obj_set_size(e, 32, 32);
        lv_image_set_inner_align(e, LV_IMAGE_ALIGN_CENTER);
    }

    lv_obj_t *l = lv_label_create(box);
    const char *txt = ui_reaction_text(key);
    if (txt) lv_label_set_text_fmt(l, "%s reacted \"%s\"", from_name, txt);
    else     lv_label_set_text_fmt(l, "%s reacted", from_name);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    lv_timer_t *t = lv_timer_create(rtoast_del_cb, 2500, box);
    lv_timer_set_repeat_count(t, 1);
    lv_obj_set_user_data(box, t);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, rtoast_dismiss_cb, LV_EVENT_CLICKED, NULL);
}
