/* Pip UI - background picker: thumbnail grid, entered from settings.
 * Mirrors the PWA picker (.themes in style.css): 3 columns, panel-aspect
 * tiles, amber ring on the active choice, "None" first. Thumbnails come
 * from the app via cb.theme_thumb (PSRAM, cached in LittleFS by the sync
 * task); a theme whose thumb hasn't downloaded yet falls back to its
 * label on a plain surface. */
#include "ui_internal.h"
#include <string.h>

static lv_obj_t *s_scr, *s_grid;
static lv_image_dsc_t s_dsc[UI_MAX_THEMES];   /* referenced by lv_image */

static void back_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        lv_indev_wait_release(lv_indev_active());
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_TOP);
    }
}

/* move the ring immediately - the definitive refresh happens when the app
 * applies the background, but that's a multi-second download away and the
 * picker felt unresponsive without instant feedback */
static void ring_tile(lv_obj_t *btn)
{
    uint32_t n = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, (int32_t)i);
        lv_obj_set_style_border_width(c, c == btn ? 3 : 0, 0);
        if (c == btn) lv_obj_set_style_border_color(c, COL_ACCENT, 0);
    }
}

static void tile_clicked(lv_event_t *e)
{
    /* user_data: 0 = "None", i+1 = g_ui.themes[i] */
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    if (!g_ui.cb.theme_selected) return;
    if (idx == 0) {
        if (!g_ui.theme_active) return;            /* already none */
        if (g_ui.cb.theme_selected("", false))
            ring_tile(btn);
    } else if (idx - 1 < g_ui.theme_count) {
        const ui_theme_info_t *t = &g_ui.themes[idx - 1];
        if (g_ui.theme_active && !strcmp(t->name, g_ui.theme_name))
            return;                                /* already active */
        /* toast only when the pick was accepted - a busy download
         * swallows the pick, and toasting then would lie */
        if (g_ui.cb.theme_selected(t->name, t->black_text)) {
            ring_tile(btn);
            ui_toast("Fetching background...", COL_ACCENT);
        }
    }
}

static lv_obj_t *mk_tile(uintptr_t idx, bool selected)
{
    lv_obj_t *btn = lv_button_create(s_grid);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, UI_THEME_THUMB_W, UI_THEME_THUMB_H);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_clip_corner(btn, true, 0);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (selected) {
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_border_color(btn, COL_ACCENT, 0);
    }
    lv_obj_add_event_cb(btn, tile_clicked, LV_EVENT_CLICKED, (void *)idx);
    return btn;
}

static void refresh_now(void *arg)
{
    LV_UNUSED(arg);
    if (!s_grid) return;
    lv_obj_clean(s_grid);

    lv_obj_t *none = mk_tile(0, !g_ui.theme_active);
    lv_obj_t *nl = lv_label_create(none);
    lv_label_set_text(nl, "None");
    lv_obj_set_style_text_font(nl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(nl, COL_TEXT_DIM, 0);
    lv_obj_center(nl);

    for (uint8_t i = 0; i < g_ui.theme_count; i++) {
        const ui_theme_info_t *t = &g_ui.themes[i];
        bool sel = g_ui.theme_active && !strcmp(t->name, g_ui.theme_name);
        lv_obj_t *btn = mk_tile((uintptr_t)i + 1, sel);

        const uint16_t *px = g_ui.cb.theme_thumb
                           ? g_ui.cb.theme_thumb(t->name, t->ver) : NULL;
        if (px) {
            s_dsc[i] = (lv_image_dsc_t){
                .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                            .cf = LV_COLOR_FORMAT_RGB565,
                            .w = UI_THEME_THUMB_W, .h = UI_THEME_THUMB_H,
                            .stride = UI_THEME_THUMB_W * 2 },
                .data_size = UI_THEME_THUMB_BYTES,
                .data = (const uint8_t *)px,
            };
            lv_obj_t *img = lv_image_create(btn);
            lv_image_set_src(img, &s_dsc[i]);
            lv_obj_center(img);
        } else {
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, t->label);
            lv_obj_set_style_text_font(l, FONT_SMALL, 0);
            lv_obj_set_style_text_color(l, COL_TEXT_DIM, 0);
            lv_obj_center(l);
        }
    }
}

void scr_theme_picker_refresh(void)
{
    /* deferred one tick: picking "None" applies synchronously from inside
     * a tile's own click event, and rebuilding there would delete the
     * widget mid-event. All callers run in the LVGL task. */
    lv_async_call(refresh_now, NULL);
}

lv_obj_t *scr_theme_picker_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    mk_header(s_scr, "Background", true, back_clicked);

    s_grid = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_grid);
    /* 3 x 108 tiles + 2 x 10 gaps = 344 */
    lv_obj_set_size(s_grid, 344, SCREEN_H - 64);
    lv_obj_align(s_grid, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_grid, 10, 0);
    lv_obj_set_style_pad_column(s_grid, 10, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);

    scr_theme_picker_refresh();
    return s_scr;
}
