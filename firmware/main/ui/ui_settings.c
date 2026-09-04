/* Pip UI - settings (reached via PIN pad) */
#include "ui_internal.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_vol_slider;
static lv_obj_t *s_bri_slider;

static void back_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        lv_indev_wait_release(lv_indev_active());
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_TOP);
    }
}

static void volume_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (g_ui.cb.volume_changed)
        g_ui.cb.volume_changed((uint8_t)lv_slider_get_value(slider));
}

static void brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (g_ui.cb.brightness_changed)
        g_ui.cb.brightness_changed((uint8_t)lv_slider_get_value(slider));
}

static void background_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_THEME_PICKER, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* also called by app_main when a fresh box boots with no networks */
void ui_open_wifi_setup(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (g_ui.cb.wifi_setup_enter && g_ui.cb.wifi_setup_enter(ssid, pass)) {
        scr_wifi_setup_show(ssid, pass);
        nav_to(SCR_WIFI_SETUP, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else {
        ui_flash_error("Could not start setup mode");
    }
}

static void wifi_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_open_wifi_setup();
}

static lv_obj_t *mk_slider_row(lv_obj_t *parent, const char *name,
                               int32_t min, int32_t max, int32_t val,
                               lv_event_cb_t cb, lv_obj_t **slider_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), 92);
    lv_obj_set_style_bg_color(box, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 14, 0);

    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(slider, COL_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, NULL);
    if (slider_out) *slider_out = slider;
    return box;
}

void ui_settings_set_levels(uint8_t volume_pct, uint8_t brightness)
{
    if (s_vol_slider) lv_slider_set_value(s_vol_slider, volume_pct, LV_ANIM_OFF);
    if (s_bri_slider) lv_slider_set_value(s_bri_slider, brightness, LV_ANIM_OFF);
}

lv_obj_t *scr_settings_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    mk_header(s_scr, "Settings", true, back_clicked);

    lv_obj_t *col = lv_obj_create(s_scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, GEO_SETTINGS_W, GEO_PICKER_H);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, GEO_PICKER_TOP);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    mk_slider_row(col, LV_SYMBOL_VOLUME_MAX "  Volume", 0, 100, 100,
                  volume_changed, &s_vol_slider);
    mk_slider_row(col, LV_SYMBOL_EYE_OPEN "  Brightness", 10, 255, 255,
                  brightness_changed, &s_bri_slider);

    /* background picker moved to its own thumbnail screen (SCR_THEME_PICKER) */
    lv_obj_t *bg = mk_button(col, LV_SYMBOL_IMAGE "  Background",
                             COL_SURFACE2, background_clicked, NULL);
    lv_obj_set_width(bg, LV_PCT(100));

    lv_obj_t *wifi = mk_button(col, LV_SYMBOL_WIFI "  WiFi setup",
                               COL_SURFACE2, wifi_clicked, NULL);
    lv_obj_set_width(wifi, LV_PCT(100));

    lv_obj_t *about = lv_label_create(col);
    lv_label_set_text_fmt(about, "Pip v%s",
                          esp_app_get_description()->version);
    lv_obj_set_style_text_font(about, FONT_SMALL, 0);
    lv_obj_set_style_text_color(about, COL_TEXT_DIM, 0);

    return s_scr;
}
