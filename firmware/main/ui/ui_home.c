/* Pip UI - home: pick a person to talk to, or open the inbox */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_gear_lbl;
static lv_obj_t *s_wifi_lbl;       /* mirrors the gear on the right */
static lv_obj_t *s_wifi_waves;     /* strength glyph, shown when online */
static lv_obj_t *s_wave[3];
static lv_obj_t *s_wave_dot;
static lv_obj_t *s_status_lbl;     /* battery, when present */
static lv_obj_t *s_clock_lbl;      /* wall clock, once SNTP answers */
static lv_obj_t *s_grid;           /* greeting + contact grid (scrolls) */
static lv_obj_t *s_inbox_btn;
static lv_obj_t *s_inbox_lbl;
static lv_obj_t *s_badge;
static lv_obj_t *s_badge_lbl;

/* Strength glyph: the symbol font has one fixed WiFi fan, so the arcs
 * are drawn. Three concentric 90-degree arcs plus the dot they radiate
 * from, all sharing a center at (WAVE_CX, WAVE_CY) inside their box -
 * each arc is positioned by its own radius, LVGL centers it in its own
 * bounding box. */
#define WAVE_BOX_W 46
#define WAVE_BOX_H 30
#define WAVE_CX    23
#define WAVE_CY    26
#define WAVE_R0     8
#define WAVE_STEP   6
#define WAVE_W      3
#define WAVE_DOT    6

static lv_obj_t *mk_wave(lv_obj_t *parent, int r)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);   /* decoration, not a dial */
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, 2 * r, 2 * r);
    lv_obj_set_pos(a, WAVE_CX - r, WAVE_CY - r);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 225, 315);   /* 0 = 3 o'clock: this opens up */
    lv_obj_set_style_arc_width(a, WAVE_W, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}

/* dBm -> lit arcs at the usual breakpoints, with 3 dB of hysteresis
 * against the level currently shown: a signal parked on a boundary
 * would otherwise flip the glyph (and cost a panel flush) every tick.
 * rssi == 0 is "no reading" and lands on 3 - an online box should not
 * look like it is struggling just because the driver said nothing. */
static uint8_t rssi_waves(int8_t rssi, uint8_t cur)
{
    static const int8_t th[3] = { -78, -67, -55 };   /* 1, 2, 3 arcs */
    uint8_t n = 0;
    for (uint8_t i = 0; i < 3; i++)
        if (rssi >= th[i] + (cur > i ? -3 : 0)) n = i + 1;
    return n;
}

/* ------------- events ------------- */
static void contact_clicked(lv_event_t *e)
{
    const ui_contact_t *c = lv_event_get_user_data(e);
    strlcpy(g_ui.selected_contact_id, c->id, UI_ID_LEN);
    strlcpy(g_ui.selected_contact_name, c->name, UI_NAME_LEN);
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
    scr_pinpad_arm(NULL);            /* the gear path unlocks Settings */
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

    /* status bar: gear/wifi in the corners on rect panels; a circle has
     * no corners, so the round build gathers everything into one centred
     * cluster (gear · clock · wifi · battery) below the rim */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, GEO_HDR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);   /* overflowing gear
                                                         must not scroll */

#if GEO_ROUND
    lv_obj_t *cl = lv_obj_create(bar);
    lv_obj_remove_style_all(cl);
    lv_obj_set_size(cl, LV_SIZE_CONTENT, 36);
    lv_obj_align(cl, LV_ALIGN_TOP_MID, 0, GEO_STATUS_Y);
    lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cl, 10, 0);
    lv_obj_clear_flag(cl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *gear = lv_button_create(cl);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 44, 36);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 14);
    lv_obj_add_event_cb(gear, settings_clicked, LV_EVENT_CLICKED, NULL);
    s_gear_lbl = lv_label_create(gear);
    lv_label_set_text(s_gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(s_gear_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_gear_lbl, FONT_TITLE, 0);
    lv_obj_center(s_gear_lbl);

    s_clock_lbl = lv_label_create(cl);
    lv_obj_set_style_text_font(s_clock_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_clock_lbl, COL_TEXT_DIM, 0);
    lv_label_set_text(s_clock_lbl, "");

    lv_obj_t *wifi_box = lv_obj_create(cl);
    lv_obj_remove_style_all(wifi_box);
    lv_obj_set_size(wifi_box, WAVE_BOX_W, 36);
    lv_obj_clear_flag(wifi_box, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(cl);
    lv_obj_set_style_text_font(s_status_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status_lbl, COL_TEXT_DIM, 0);
#else
    /* nudged down and enlarged: the rounded panel corner makes targets
     * flush with the corner hard to hit */
    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 72, GEO_HDR_H);
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
    lv_obj_set_size(wifi_box, 72, GEO_HDR_H);
    lv_obj_align(wifi_box, LV_ALIGN_RIGHT_MID, 0, 6);
    lv_obj_clear_flag(wifi_box, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_status_lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status_lbl, COL_TEXT_DIM, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, -72, 6);

    /* clock in the bar's dead center, same downward nudge as its
     * neighbours; empty (invisible) until the box knows the time */
    s_clock_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_clock_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_clock_lbl, COL_TEXT_DIM, 0);
    lv_label_set_text(s_clock_lbl, "");
    lv_obj_align(s_clock_lbl, LV_ALIGN_CENTER, 0, 6);
#endif

    s_wifi_lbl = lv_label_create(wifi_box);
    lv_obj_set_style_text_font(s_wifi_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_wifi_lbl, COL_TEXT_DIM, 0);
    lv_obj_center(s_wifi_lbl);

    /* online: the drawn arcs take the label's place (one or the other is
     * hidden, so they can share the box) */
    s_wifi_waves = lv_obj_create(wifi_box);
    lv_obj_remove_style_all(s_wifi_waves);
    lv_obj_set_size(s_wifi_waves, WAVE_BOX_W, WAVE_BOX_H);
    lv_obj_clear_flag(s_wifi_waves, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_wifi_waves);
    for (int i = 0; i < 3; i++)
        s_wave[i] = mk_wave(s_wifi_waves, WAVE_R0 + i * WAVE_STEP);
    s_wave_dot = lv_obj_create(s_wifi_waves);
    lv_obj_remove_style_all(s_wave_dot);
    lv_obj_set_size(s_wave_dot, WAVE_DOT, WAVE_DOT);
    lv_obj_set_pos(s_wave_dot, WAVE_CX - WAVE_DOT / 2, WAVE_CY - WAVE_DOT / 2);
    lv_obj_set_style_radius(s_wave_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_wave_dot, LV_OPA_COVER, 0);

    /* greeting + contact grid: the "Hello <owner>" heading is the grid's
     * first (full-width) row, so it scrolls away with the contacts. The
     * grid spans the full top of the screen with the bar floating
     * transparently above it (pad_top keeps content below the bar at
     * rest), so scrolling content slides under the bar instead of being
     * cropped at its edge. */
    /* vertical budget: with 4 contacts (2.16: seen tiles) the second row
     * of NAMES must be fully visible at scroll 0, i.e. clear the grid
     * clip line which sits flush with the inbox pill (GEO_GRID_H):
     *   1.8:    48 + 30 hello + 14 + 132 row1 + 14 + 108 name-y + 22
     *             = 368 <= 372
     *   1.75b:  74 + 30 hello +  8 + 126 row1 +  8 + 100 name-y + 22
     *             = 368 <= 378
     *   2.16:   72 (no hello) + 158 row1 + 12 + 136 name-y + 22
     *             = 400 <= 404
     * Touch any of these numbers together or row 2 crops again. */
    s_grid = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, SCREEN_W, GEO_GRID_H);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(s_grid, GEO_HOME_PAD_TOP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    /* track placement must be START: CENTER re-centers overflowing
     * content on every scroll relayout, so with 5+ contacts the grid
     * rested fully scrolled down and sprang back when scrolled up */
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, GEO_GRID_PAD_ROW, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);

    /* inbox button pinned at bottom (round: narrowed to the chord) */
    s_inbox_btn = mk_button(s_scr, "", COL_SURFACE, inbox_clicked, NULL);
    lv_obj_set_size(s_inbox_btn, GEO_PILL_W, GEO_PILL_H);
    lv_obj_set_style_radius(s_inbox_btn, GEO_PILL_H / 2, 0);  /* pill:
                                          clears the panel's round corner */
    lv_obj_align(s_inbox_btn, LV_ALIGN_BOTTOM_MID, 0, -GEO_PILL_BOTTOM);
    s_inbox_lbl = lv_obj_get_child(s_inbox_btn, 0);

    s_badge = lv_obj_create(s_inbox_btn);
    lv_obj_remove_style_all(s_badge);
    lv_obj_set_size(s_badge, GEO_BADGE_D, GEO_BADGE_D);
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

    /* the 2.16 drops the greeting: big tiles read across a room and the
     * heading cost them a full row of vertical budget */
    if (GEO_HOME_GREETING && g_ui.owner_name[0]) {
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
        lv_obj_set_size(cell, GEO_CELL_W, GEO_CELL_H);

        lv_obj_t *av = mk_round_button(cell, GEO_AVATAR_D,
                                       lv_color_hex(c->color),
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
            lv_obj_set_size(chip, txt ? LV_SIZE_CONTENT : GEO_CHIP_D,
                            GEO_CHIP_D);
            lv_obj_set_style_radius(chip, GEO_CHIP_D / 2, 0);
            lv_obj_set_style_bg_color(chip, COL_SURFACE2, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(chip, 2, 0);
            lv_obj_set_style_border_color(chip, COL_ACCENT, 0);
            lv_obj_set_style_pad_hor(chip, txt ? 10 : 0, 0);
            lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, GEO_CHIP_DX, GEO_CHIP_DY);
            if (txt) {
                lv_obj_t *l = lv_label_create(chip);
                lv_label_set_text(l, txt);
                lv_obj_set_style_text_font(l, FONT_SMALL, 0);
                lv_obj_set_style_text_color(l, COL_ACCENT, 0);
                lv_obj_center(l);
            } else {
                lv_obj_t *e = lv_image_create(chip);
                lv_image_set_src(e, ui_emoji_img(rb->key));
                /* baked emoji are 64 px; scale is 256ths */
                lv_image_set_scale(e, GEO_CHIP_EMOJI_PX * 4);
                lv_obj_set_size(e, GEO_CHIP_EMOJI_PX, GEO_CHIP_EMOJI_PX);
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
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, GEO_NAME_Y);
    }
}

/* what the status bar currently shows: it is re-evaluated every few
 * seconds and re-rendering identical content still costs a panel flush */
static ui_wifi_state_t s_shown_wifi = (ui_wifi_state_t)-1;
static uint8_t         s_shown_waves;
static char            s_shown_bat[16];
static char            s_shown_clock[8];
static bool            s_status_dirty = true;   /* colors changed (theme) */

/* status bar only: wifi/battery tick every few seconds - they must NOT
 * rebuild the contact grid (that reset an in-progress scroll) */
void scr_home_update_status(void)
{
    if (!s_scr) return;

    bool online = (g_ui.wifi == UI_WIFI_ONLINE);
    uint8_t waves = online
        ? rssi_waves(g_ui.cb.wifi_rssi ? g_ui.cb.wifi_rssi() : 0,
                     s_shown_waves) : 0;

    if (s_status_dirty || g_ui.wifi != s_shown_wifi || waves != s_shown_waves) {
        s_shown_wifi  = g_ui.wifi;
        s_shown_waves = waves;
        if (online) {
            lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_waves, LV_OBJ_FLAG_HIDDEN);
            lv_color_t col = ui_fg_color(true);
            lv_opa_t   lit = ui_fg_opa(true);
            lv_obj_set_style_bg_color(s_wave_dot, col, 0);
            lv_obj_set_style_bg_opa(s_wave_dot, lit, 0);
            for (int i = 0; i < 3; i++) {
                lv_obj_set_style_arc_color(s_wave[i], col, LV_PART_MAIN);
                /* unlit arcs stay faintly visible: the glyph keeps its
                 * shape, so weak signal reads as weak, not as broken */
                lv_obj_set_style_arc_opa(s_wave[i],
                                         i < waves ? lit : lit / 4,
                                         LV_PART_MAIN);
            }
        } else {
            lv_obj_add_flag(s_wifi_waves, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_wifi_lbl,
                (g_ui.wifi == UI_WIFI_PORTAL)     ? LV_SYMBOL_WARNING :
                (g_ui.wifi == UI_WIFI_CONNECTING) ? LV_SYMBOL_REFRESH
                                                  : LV_SYMBOL_CLOSE);
        }
    }

    char bat[sizeof(s_shown_bat)] = "";
    if (g_ui.battery_present)
        snprintf(bat, sizeof(bat), "%s %u%%",
                 g_ui.charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_3,
                 g_ui.battery_pct);
    if (s_status_dirty || strcmp(bat, s_shown_bat)) {
        strlcpy(s_shown_bat, bat, sizeof(s_shown_bat));
        lv_label_set_text(s_status_lbl, bat);
    }

    /* rides the same few-second tick as wifi/battery: the label repaints
     * only when the minute string actually changes */
    char clk[sizeof(s_shown_clock)];
    ui_clock_text(clk, sizeof(clk));
    if (s_status_dirty || strcmp(clk, s_shown_clock)) {
        strlcpy(s_shown_clock, clk, sizeof(s_shown_clock));
        lv_label_set_text(s_clock_lbl, clk);
    }
    s_status_dirty = false;
}

/* inbox pill + badge only: message arrivals don't touch the grid either */
void scr_home_update_inbox(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_opa(s_inbox_btn, UI_SURFACE_OPA, 0);
    /* quiet hours ride along on the pill: same row, a sleepy tail */
    const char *zzz = g_ui.dnd ? "   Zzz" : "";
    if (g_ui.unheard_count > 0) {
        lv_label_set_text_fmt(s_inbox_lbl, LV_SYMBOL_BELL "  Inbox%s", zzz);
        lv_label_set_text_fmt(s_badge_lbl, "%u", g_ui.unheard_count);
        lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_inbox_btn, COL_SURFACE2, 0);
    } else {
        lv_label_set_text_fmt(s_inbox_lbl, LV_SYMBOL_ENVELOPE "  Inbox%s", zzz);
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
    ui_fg_label(s_clock_lbl, true);
    s_status_dirty = true;           /* arc colors are set on next update */
    scr_home_refresh();              /* rebuilds greeting + contact names */
}
