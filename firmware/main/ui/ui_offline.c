/* Pip UI - "No WiFi" nag.
 *
 * An offline box still records and queues (the outbox drains on the next
 * connection), so this is a nag, not a wall:
 *   - it waits out short drops; the reconnect backoff recovers most of
 *     them well inside the grace period,
 *   - it only ever covers the home screen - never a recording, the
 *     inbox, the setup QR or the boot splash,
 *   - dismissing snoozes it for 5 minutes, and it comes back if WiFi is
 *     still out when the snooze ends.
 *
 * The captive-portal state is left to its own toast: the box IS on WiFi
 * there, and "No WiFi" would send people to the wrong fix.
 */
#include "ui_internal.h"

#define GRACE_MS   (60 * 1000)        /* offline this long before nagging */
#define SNOOZE_MS  (5 * 60 * 1000)

static lv_obj_t *s_scrim;
static uint32_t  s_down_since;        /* tick of the drop; 0 = not down   */
static uint32_t  s_snoozed_at;
static bool      s_snoozed;

static void close_overlay(void)
{
    if (!s_scrim) return;
    /* async: we may be inside an event of one of its children */
    lv_obj_delete_async(s_scrim);
    s_scrim = NULL;
}

void ui_offline_hide(void) { close_overlay(); }

/* The user just worked on the connection (came out of WiFi setup): give
 * them the full quiet period again instead of nagging the moment they
 * land back on home. */
void ui_offline_defer(void)
{
    s_down_since = lv_tick_get() | 1;
    s_snoozed    = false;
}

static void snooze(void)
{
    s_snoozed    = true;
    s_snoozed_at = lv_tick_get();
    close_overlay();
}

static void dismiss_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    snooze();
}

static void scrim_clicked(lv_event_t *e)
{
    /* only a direct tap on the backdrop counts as a dismiss */
    if (lv_event_get_target(e) == lv_event_get_current_target(e))
        snooze();
}

static void setup_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    /* same PIN gate as Settings, then straight into the setup QR instead
     * of the settings list */
    snooze();                       /* no second nag on the way back */
    scr_pinpad_arm(ui_open_wifi_setup);
    nav_to(SCR_PINPAD, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

static void open_overlay(void)
{
    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_scrim, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_70, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_pad_row(panel, 14, 0);
    lv_obj_set_size(panel, SCREEN_W - 40, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  No WiFi");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);

    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_text(body, "Pip can't reach the internet.\n"
                            "Messages will send when it's back.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_font(body, FONT_SMALL, 0);
    lv_obj_set_style_text_color(body, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *fix = mk_button(panel, LV_SYMBOL_WIFI "  Set up WiFi",
                              COL_ACCENT, setup_clicked, NULL);
    lv_obj_set_width(fix, LV_PCT(100));
    lv_obj_set_style_text_color(lv_obj_get_child(fix, 0), COL_BG, 0);

    lv_obj_t *later = mk_button(panel, "Not now", COL_SURFACE,
                                dismiss_clicked, NULL);
    lv_obj_set_width(later, LV_PCT(100));
    lv_obj_set_style_text_color(lv_obj_get_child(later, 0), COL_TEXT, 0);
}

void ui_offline_eval(void)
{
    if (g_ui.wifi != UI_WIFI_OFFLINE && g_ui.wifi != UI_WIFI_CONNECTING) {
        s_down_since = 0;
        s_snoozed    = false;        /* back online: start fresh next time */
        close_overlay();
        return;
    }

    /* |1 keeps the stamp non-zero (0 is the "not down" marker); the 1 ms
     * it can add to the grace period is noise */
    if (!s_down_since) s_down_since = lv_tick_get() | 1;

    if (!ui_screen_is(SCR_HOME)) { close_overlay(); return; }
    if (s_scrim) return;                                  /* already up  */
    if (lv_tick_elaps(s_down_since) < GRACE_MS) return;   /* just a blip */
    if (s_snoozed && lv_tick_elaps(s_snoozed_at) < SNOOZE_MS) return;

    s_snoozed = false;
    open_overlay();
}
