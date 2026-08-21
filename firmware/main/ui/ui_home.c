/* Pip UI - home: pick a person to talk to, or open the inbox */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_gear_lbl;
static lv_obj_t *s_wifi_lbl;       /* mirrors the gear on the right */
static lv_obj_t *s_status_lbl;     /* battery, when present */
static lv_obj_t *s_grid;           /* greeting + contact grid (scrolls) */
static lv_obj_t *s_inbox_btn;
static lv_obj_t *s_inbox_lbl;
static lv_obj_t *s_badge;
static lv_obj_t *s_badge_lbl;

/* ------------- events ------------- */
static void contact_clicked(lv_event_t *e)
{
    const ui_contact_t *c = lv_event_get_user_data(e);
    strncpy(g_ui.selected_contact_id, c->id, UI_ID_LEN - 1);
    strncpy(g_ui.selected_contact_name, c->name, UI_NAME_LEN - 1);
    ui_react_mark_seen(g_ui.selected_contact_id);  /* badge = seen on open */
    scr_record_show();
    nav_to(SCR_RECORD, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void inbox_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_INBOX, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void settings_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    nav_to(SCR_PINPAD, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
        lv_indev_wait_release(lv_indev_active());
        settings_clicked(e);
    }
}

/* ------------- build ------------- */
lv_obj_t *scr_home_create(void)
{
    s_scr = mk_screen();
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    /* status bar: settings gear left, wifi/battery right */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, 56);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);   /* overflowing gear
                                                         must not scroll */

    /* nudged down and enlarged: the rounded panel corner makes targets
     * flush with the corner hard to hit */
    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 72, 56);
    lv_obj_align(gear, LV_ALIGN_LEFT_MID, 0, 6);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear, settings_clicked, LV_EVENT_CLICKED, NULL);
    s_gear_lbl = lv_label_create(gear);
    lv_label_set_text(s_gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(s_gear_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_gear_lbl, FONT_TITLE, 0);
    lv_obj_center(s_gear_lbl);

    /* wifi mirrors the gear: same box, same font, same downward nudge */
    lv_obj_t *wifi_box = lv_obj_create(bar);
    lv_obj_remove_style_all(wifi_box);
    lv_obj_set_size(wifi_box, 72, 56);
    lv_obj_align(wifi_box, LV_ALIGN_RIGHT_MID, 0, 6);
    lv_obj_clear_flag(wifi_box, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_lbl = lv_label_create(wifi_box);
    lv_obj_set_style_text_font(s_wifi_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_wifi_lbl, COL_TEXT_DIM, 0);
    lv_obj_center(s_wifi_lbl);

    s_status_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_status_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status_lbl, COL_TEXT_DIM, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, -72, 6);

    /* greeting + contact grid: the "Hello <owner>" heading is the grid's
     * first (full-width) row, so it scrolls away with the contacts. The
     * grid spans the full top of the screen with the bar floating
     * transparently above it (pad_top keeps content below the bar at
     * rest), so scrolling content slides under the bar instead of being
     * cropped at its edge. */
    /* vertical budget: with 4 contacts the second row of NAMES must be
     * fully visible at scroll 0, i.e. clear the grid clip line which sits
     * flush with the inbox pill (448 - 68 - 8 = 372):
     *   48 pad_top + 30 hello + 14 gap + 132 row1 + 14 gap
     *     + 108 name-y + 22 name = 368 <= 372.
     * Touch any of these numbers together or row 2 crops again. */
    s_grid = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, SCREEN_W, SCREEN_H - 76);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(s_grid, 48, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    /* track placement must be START: CENTER re-centers overflowing
     * content on every scroll relayout, so with 5+ contacts the grid
     * rested fully scrolled down and sprang back when scrolled up */
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, 14, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);

    /* inbox button pinned at bottom */
    s_inbox_btn = mk_button(s_scr, "", COL_SURFACE, inbox_clicked, NULL);
    lv_obj_set_size(s_inbox_btn, SCREEN_W - 24, 68);
    lv_obj_set_style_radius(s_inbox_btn, 34, 0);   /* pill: clears the
                                                      panel's round corner */
    lv_obj_align(s_inbox_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    s_inbox_lbl = lv_obj_get_child(s_inbox_btn, 0);

    s_badge = lv_obj_create(s_inbox_btn);
    lv_obj_remove_style_all(s_badge);
    lv_obj_set_size(s_badge, 34, 34);
    lv_obj_set_style_radius(s_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_badge, COL_DANGER, 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_align(s_badge, LV_ALIGN_RIGHT_MID, -6, 0);
    s_badge_lbl = lv_label_create(s_badge);
    lv_obj_set_style_text_font(s_badge_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_badge_lbl, COL_TEXT, 0);
    lv_obj_center(s_badge_lbl);

    /* the bar draws over the grid so gear + status stay visible while
     * content scrolls beneath them */
    lv_obj_move_foreground(bar);

    return s_scr;
}

/* ------------- refresh ------------- */
static void rebuild_contacts(void)
{
    lv_obj_clean(s_grid);

    if (g_ui.owner_name[0]) {
        lv_obj_t *hello = lv_label_create(s_grid);
        lv_label_set_text_fmt(hello, "Hello %s", g_ui.owner_name);
        lv_obj_set_style_text_font(hello, FONT_TITLE, 0);
        ui_fg_label(hello, false);
        lv_obj_set_width(hello, LV_PCT(100));   /* own flex row */
        lv_obj_set_style_text_align(hello, LV_TEXT_ALIGN_CENTER, 0);
    }

    for (uint8_t i = 0; i < g_ui.contact_count; i++) {
        ui_contact_t *c = &g_ui.contacts[i];

        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, (SCREEN_W / 2) - 16, 132);

        lv_obj_t *av = mk_round_button(cell, 104, lv_color_hex(c->color),
                                       contact_clicked, c);
        lv_obj_align(av, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_t *init = lv_label_create(av);
        char ini[2] = { c->name[0] ? c->name[0] : '?', 0 };
        lv_label_set_text(init, ini);
        lv_obj_set_style_text_font(init, FONT_BIG, 0);
        lv_obj_set_style_text_color(init, COL_BG, 0);
        lv_obj_center(init);

        /* unseen reaction to something this user sent: chip on the tile */
        const ui_reaction_t *rb = ui_react_badge_for(c->id);
        if (rb) {
            lv_obj_t *chip = lv_obj_create(av);
            lv_obj_remove_style_all(chip);
            const char *txt = ui_reaction_text(rb->key);
            lv_obj_set_size(chip, txt ? LV_SIZE_CONTENT : 40, 40);
            lv_obj_set_style_radius(chip, 20, 0);
            lv_obj_set_style_bg_color(chip, COL_SURFACE2, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(chip, 2, 0);
            lv_obj_set_style_border_color(chip, COL_ACCENT, 0);
            lv_obj_set_style_pad_hor(chip, txt ? 10 : 0, 0);
            lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 8, -4);
            if (txt) {
                lv_obj_t *l = lv_label_create(chip);
                lv_label_set_text(l, txt);
                lv_obj_set_style_text_font(l, FONT_SMALL, 0);
                lv_obj_set_style_text_color(l, COL_ACCENT, 0);
                lv_obj_center(l);
            } else {
                lv_obj_t *e = lv_image_create(chip);
                lv_image_set_src(e, ui_emoji_img(rb->key));
                lv_image_set_scale(e, 112);        /* 64 px -> 28 px */
                lv_obj_set_size(e, 28, 28);
                lv_image_set_inner_align(e, LV_IMAGE_ALIGN_CENTER);
                lv_obj_center(e);
            }
        }

        lv_obj_t *nm = lv_label_create(cell);
        lv_label_set_text(nm, c->name);
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        ui_fg_label(nm, false);
        /* just under the avatar it belongs to - the row gap (pad_row)
         * provides the larger break before the next row */
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 108);
    }
}

/* status bar only: wifi/battery tick every few seconds - they must NOT
 * rebuild the contact grid (that reset an in-progress scroll) */
void scr_home_update_status(void)
{
    if (!s_scr) return;
    lv_label_set_text(s_wifi_lbl,
        (g_ui.wifi == UI_WIFI_ONLINE)     ? LV_SYMBOL_WIFI :
        (g_ui.wifi == UI_WIFI_PORTAL)     ? LV_SYMBOL_WARNING :
        (g_ui.wifi == UI_WIFI_CONNECTING) ? LV_SYMBOL_REFRESH : LV_SYMBOL_CLOSE);
    if (g_ui.battery_present)
        lv_label_set_text_fmt(s_status_lbl, "%s %u%%",
                              g_ui.charging ? LV_SYMBOL_CHARGE
                                            : LV_SYMBOL_BATTERY_3,
                              g_ui.battery_pct);
    else
        lv_label_set_text(s_status_lbl, "");
}

/* inbox pill + badge only: message arrivals don't touch the grid either */
void scr_home_update_inbox(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_opa(s_inbox_btn, UI_SURFACE_OPA, 0);
    if (g_ui.unheard_count > 0) {
        lv_label_set_text_fmt(s_inbox_lbl, LV_SYMBOL_BELL "  Inbox");
        lv_label_set_text_fmt(s_badge_lbl, "%u", g_ui.unheard_count);
        lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_inbox_btn, COL_SURFACE2, 0);
    } else {
        lv_label_set_text(s_inbox_lbl, LV_SYMBOL_ENVELOPE "  Inbox");
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_inbox_btn, COL_SURFACE, 0);
    }
}

/* full refresh: entering the screen, or contacts/owner/badges changed */
void scr_home_refresh(void)
{
    if (!s_scr) return;
    scr_home_update_status();
    rebuild_contacts();
    scr_home_update_inbox();
}

void scr_home_apply_theme(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_image_src(s_scr, ui_bg_image(), 0);
    ui_fg_label(s_gear_lbl, true);
    ui_fg_label(s_wifi_lbl, true);
    ui_fg_label(s_status_lbl, true);
    scr_home_refresh();              /* rebuilds greeting + contact names */
}
