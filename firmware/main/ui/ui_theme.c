/* Pip UI - shared styled constructors + background theme plumbing */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

/* ---------------- background theme ---------------- */
static lv_image_dsc_t s_bg_dsc;

const lv_image_dsc_t *ui_bg_image(void)
{
    return g_ui.theme_active ? &s_bg_dsc : NULL;
}

void ui_fg_label(lv_obj_t *label, bool dim)
{
    if (g_ui.theme_active) {
        lv_obj_set_style_text_color(label, g_ui.theme_black_text
                                    ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_opa(label, dim ? LV_OPA_70 : LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(label, dim ? COL_TEXT_DIM : COL_TEXT, 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    }
}

/* ui_fg_label's rules for drawn art (arcs, dots) that has no text style
 * to carry them */
lv_color_t ui_fg_color(bool dim)
{
    if (g_ui.theme_active)
        return g_ui.theme_black_text ? lv_color_black() : lv_color_white();
    return dim ? COL_TEXT_DIM : COL_TEXT;
}

lv_opa_t ui_fg_opa(bool dim)
{
    return (g_ui.theme_active && dim) ? LV_OPA_70 : LV_OPA_COVER;
}

void ui_fg_header(lv_obj_t *bar)
{
    /* children are the (optional) back button and the title label */
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *child = lv_obj_get_child(bar, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class))
            ui_fg_label(child, false);
        else if (lv_obj_get_child_count(child) > 0)
            ui_fg_label(lv_obj_get_child(child, 0), false);
    }
}

void ui_set_background(const uint16_t *px, const char *name, bool black_text)
{
    g_ui.theme_active = (px != NULL);
    g_ui.theme_black_text = black_text;
    strlcpy(g_ui.theme_name, (px && name) ? name : "",
            sizeof(g_ui.theme_name));
    if (px) {
        s_bg_dsc = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                        .cf = LV_COLOR_FORMAT_RGB565,
                        .w = SCREEN_W, .h = SCREEN_H,
                        .stride = SCREEN_W * 2 },
            .data_size = SCREEN_W * SCREEN_H * 2,
            .data = (const uint8_t *)px,
        };
    }
    scr_home_apply_theme();
    scr_record_apply_theme();
    scr_inbox_apply_theme();
    scr_theme_picker_refresh();      /* selection ring tracks the theme */
}

lv_obj_t *mk_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *mk_header(lv_obj_t *parent, const char *title,
                    bool back_btn, lv_event_cb_t on_back)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, 56);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);   /* overflowing back
                                                         button must not scroll */

    if (back_btn) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 64, 56);          /* generous touch target */
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 6);   /* clear the rounded
                                                         corner slightly */
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(l, FONT_TITLE, 0);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_obj_center(l);
    }

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_center(t);
    return bar;
}

lv_obj_t *mk_button(lv_obj_t *parent, const char *txt, lv_color_t bg,
                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_height(btn, 64);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *mk_round_button(lv_obj_t *parent, int diameter, lv_color_t bg,
                          lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* ---------------- toast ---------------- */
static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_t *box = lv_timer_get_user_data(t);
    lv_obj_delete(box);
    lv_timer_delete(t);
}

static void toast_dismiss_cb(lv_event_t *e)
{
    /* tap or swipe-up dismisses early */
    if (lv_event_get_code(e) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP)
        return;
    lv_obj_t *box = lv_event_get_current_target(e);
    lv_timer_t *t = lv_obj_get_user_data(box);
    if (!t) return;                    /* already dismissed */
    lv_timer_delete(t);
    lv_obj_set_user_data(box, NULL);
    /* the delete is async (we are inside this object's own event), so drop
     * the tag first: ui_toast_clear() deleting it synchronously in the
     * meantime would leave the queued async call holding a freed pointer */
    lv_obj_remove_flag(box, LV_OBJ_FLAG_USER_1);
    lv_obj_delete_async(box);
}

void ui_toast(const char *text, lv_color_t color)
{
    /* Ambient: the icon on screen is already the notification and the chime
     * is the alert. A toast here would light a big box in the middle of the
     * night on a screen built to stay nearly dark. */
    if (ui_screen_is(SCR_AMBIENT)) return;

    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_style_bg_color(box, COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, color, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_width(box, SCREEN_W - 40);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    lv_timer_t *t = lv_timer_create(toast_del_cb, 2500, box);
    lv_timer_set_repeat_count(t, 1);
    lv_obj_set_user_data(box, t);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_USER_1);   /* marks it for ui_toast_clear */
    lv_obj_add_event_cb(box, toast_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(box, toast_dismiss_cb, LV_EVENT_GESTURE, NULL);
}

/* Toasts share lv_layer_top() with the power shield, so they cannot be
 * cleared by emptying the layer; USER_1 tags the ones that are ours.
 * The delete is synchronous on purpose - the only caller (ui_ambient_enter)
 * paints immediately afterwards, and an async delete would still show. */
void ui_toast_clear(void)
{
    lv_obj_t *top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *box = lv_obj_get_child(top, i);
        if (!lv_obj_has_flag(box, LV_OBJ_FLAG_USER_1)) continue;
        lv_timer_t *t = lv_obj_get_user_data(box);
        if (t) lv_timer_delete(t);
        lv_obj_delete(box);
    }
}
