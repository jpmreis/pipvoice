/* Pip UI - core: init, navigation, app-facing entry points */
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

ui_state_t g_ui;

static lv_obj_t      *s_screens[SCR_COUNT];
static ui_screen_id_t s_cur = SCR_HOME;

/* lv_screen_active() still reports the old screen while a load animation
 * runs, so navigation decisions use the target instead */
bool ui_screen_is(ui_screen_id_t id) { return s_cur == id; }

/* status bar (wifi strength, battery) + the offline nag's grace/snooze
 * timers; everything else is event-driven */
#define UI_TICK_MS 5000

static void ui_tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* ambient is a clock on black by construction: neither the status bar
     * nor the offline nag is on screen there (the clock runs its own
     * timers, ui_ambient.c), and the nag is home-only anyway. Both come
     * back on the next tick after we leave. */
    if (ui_screen_is(SCR_AMBIENT)) return;
    scr_home_update_status();
    ui_offline_eval();
}

void nav_to(ui_screen_id_t id, lv_screen_load_anim_t anim)
{
    /* refresh dynamic screens right before showing them */
    switch (id) {
        case SCR_HOME:   scr_home_refresh();  break;
        case SCR_INBOX:
            scr_inbox_refresh();
            if (g_ui.cb.inbox_opened) g_ui.cb.inbox_opened();
            break;
        case SCR_PINPAD:       scr_pinpad_reset();         break;
        case SCR_THEME_PICKER: scr_theme_picker_refresh(); break;
        default: break;
    }
    s_cur = id;
    if (id != SCR_HOME) ui_offline_hide();   /* the nag is home-only */
    lv_screen_load_anim(s_screens[id], anim, 180, 0, false);
    if (id == SCR_HOME) ui_offline_eval();   /* still offline? nag again */
}

void ui_init(const ui_callbacks_t *cbs)
{
    memset(&g_ui, 0, sizeof(g_ui));
    if (cbs) g_ui.cb = *cbs;
    g_ui.battery_pct = 100;

    s_screens[SCR_HOME]       = scr_home_create();
    s_screens[SCR_RECORD]     = scr_record_create();
    s_screens[SCR_INBOX]      = scr_inbox_create();
    s_screens[SCR_PLAYBACK]   = scr_playback_create();
    s_screens[SCR_PINPAD]     = scr_pinpad_create();
    s_screens[SCR_SETTINGS]   = scr_settings_create();
    s_screens[SCR_THEME_PICKER] = scr_theme_picker_create();
    s_screens[SCR_WIFI_SETUP] = scr_wifi_setup_create();
    s_screens[SCR_SPLASH]     = scr_splash_create();
    s_screens[SCR_AMBIENT]    = scr_ambient_create();
    s_screens[SCR_VOICE]      = scr_voice_create();

    scr_home_refresh();
    lv_screen_load(s_screens[SCR_HOME]);
    lv_timer_create(ui_tick_cb, UI_TICK_MS, NULL);
}

/* ---------------- physical-button navigation ---------------- */
/* shared with ui_voice.c: the voice flow must not yank these either */
bool nav_locked_out(void)
{
    /* don't yank an active recording or the provisioning portal */
    lv_obj_t *act = lv_screen_active();
    return act == s_screens[SCR_RECORD] || act == s_screens[SCR_WIFI_SETUP];
}

void ui_open_record_recent(void)
{
    if (nav_locked_out() || g_ui.contact_count == 0) return;
    lv_display_trigger_activity(NULL);
    strncpy(g_ui.selected_contact_id, g_ui.contacts[0].id, UI_ID_LEN - 1);
    strncpy(g_ui.selected_contact_name, g_ui.contacts[0].name, UI_NAME_LEN - 1);
    ui_react_mark_seen(g_ui.selected_contact_id);
    scr_record_show();
    nav_to(SCR_RECORD, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

void ui_open_inbox(void)
{
    if (nav_locked_out()) return;
    lv_display_trigger_activity(NULL);
    if (lv_screen_active() != s_screens[SCR_INBOX])
        nav_to(SCR_INBOX, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

void ui_splash_show(void)
{
    /* Both callers (boot, and waking from display-off) run with the panel
     * dark, so the splash must be ON the panel before brightness returns
     * - otherwise the light comes up on whatever was there before, which
     * is a frame of home. nav_to() won't do: its 180 ms load duration
     * defers the actual screen switch to the next anim tick even for
     * ANIM_NONE. lv_screen_load() is time-0 -> switches now. */
    if (nav_locked_out()) return;
    scr_splash_show();
    s_cur = SCR_SPLASH;
    ui_offline_hide();
    lv_screen_load(s_screens[SCR_SPLASH]);
    lv_refr_now(NULL);
}

/* ---------------- ambient (sleeping clock) screen ---------------- */

/* Render "HH:MM" into buf; false (and an empty string) until SNTP has
 * answered - anything older than 2023-11 means it hasn't (same convention
 * as sync.c's clock_set). Callers that only want the verdict pass NULL. */
#define UI_CLOCK_SANE_EPOCH 1700000000

bool ui_clock_text(char *buf, size_t len)
{
    if (buf && len) buf[0] = '\0';
    time_t now = time(NULL);
    if (now < UI_CLOCK_SANE_EPOCH) return false;
    if (buf && len) {
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(buf, len, "%H:%M", &lt);
    }
    return true;
}

bool ui_ambient_mail(void)
{
    /* DND holds mail on the server and the bedroom dark: no dot, no
     * brightness step while quiet hours run */
    return g_ui.unheard_count > 0 && !g_ui.dnd;
}

bool ui_ambient_wanted(void)
{
    /* nav_locked_out(): a recording in progress and the provisioning QR are
     * the two screens that must never be replaced by the sleep screen */
    if (nav_locked_out()) return false;
    /* the clock face needs a sane wall clock; the mail dot does not - a
     * box that never learned the time keeps the old dot-or-dark behavior */
    return ui_ambient_mail() || ui_clock_text(NULL, 0);
}

bool ui_ambient_enter(void)
{
    if (!ui_ambient_wanted()) return false;
    if (ui_screen_is(SCR_AMBIENT)) return true;

    /* lv_layer_top() sits over every screen, so anything parked there would
     * be lit right along with the icon - and a nag card or a toast is the
     * opposite of what this screen is for. Both are synchronous deletes:
     * the lv_refr_now() below would otherwise still paint them once. */
    ui_offline_hide();
    ui_toast_clear();

    scr_ambient_show();
    s_cur = SCR_AMBIENT;
    lv_screen_load(s_screens[SCR_AMBIENT]);   /* time-0, like ui_splash_show */
    lv_refr_now(NULL);                        /* on the panel before the
                                                 caller raises brightness   */
    return true;
}

void ui_ambient_exit(void)
{
    if (!ui_screen_is(SCR_AMBIENT)) return;   /* idempotent: waking from a
                                                 fully dark panel lands here */
    scr_ambient_hide();
    scr_home_refresh();
    s_cur = SCR_HOME;
    lv_screen_load(s_screens[SCR_HOME]);
    lv_refr_now(NULL);   /* the caller is about to light the panel, or a
                            splash is about to paint over this - either way
                            no frame of the icon may survive */
}

void ui_ambient_wake_to_splash(void)
{
    /* The touch-wake handoff. Unlike ui_ambient_exit + ui_splash_show this
     * runs with the panel LIT, so it must go ambient -> splash directly
     * (the exit path's detour through home would flash) and the splash's
     * dot must start exactly where the ambient dot stands - it then glides
     * home into the wordmark while the letters fall in as usual. */
    if (!ui_screen_is(SCR_AMBIENT)) { ui_splash_show(); return; }
    int32_t x, y;
    scr_ambient_dot_pos(&x, &y);
    scr_ambient_hide();
    scr_splash_show_from(x, y);
    s_cur = SCR_SPLASH;
    ui_offline_hide();
    lv_screen_load(s_screens[SCR_SPLASH]);    /* time-0: the next frame is
                                                 the splash with the dot in
                                                 the same place - seamless */
    lv_refr_now(NULL);
}

/* ---------------- data setters ---------------- */
void ui_set_owner_name(const char *name)
{
    strncpy(g_ui.owner_name, name, UI_NAME_LEN - 1);
    scr_home_refresh();
}

void ui_set_contacts(const ui_contact_t *list, uint8_t count)
{
    if (count > UI_MAX_CONTACTS) count = UI_MAX_CONTACTS;
    memcpy(g_ui.contacts, list, count * sizeof(ui_contact_t));
    g_ui.contact_count = count;
    scr_home_refresh();
}

void ui_set_inbox(const ui_message_t *list, uint8_t count)
{
    if (count > UI_MAX_MESSAGES) count = UI_MAX_MESSAGES;
    memcpy(g_ui.inbox, list, count * sizeof(ui_message_t));
    g_ui.inbox_count = count;
    g_ui.unheard_count = 0;
    for (uint8_t i = 0; i < count; i++)
        if (!g_ui.inbox[i].heard) g_ui.unheard_count++;
    scr_home_update_inbox();      /* pill only: don't reset a grid scroll */
    scr_inbox_refresh();
    /* a sleeping box docks/undocks the clock's dot within a second;
     * power.c retunes the brightness on its own 1 Hz ladder */
    if (ui_screen_is(SCR_AMBIENT)) scr_ambient_refresh();
}

void ui_set_battery(uint8_t pct, bool charging, bool present)
{
    g_ui.battery_pct = pct;
    g_ui.charging = charging;
    g_ui.battery_present = present;
    scr_home_update_status();     /* periodic: must not rebuild the grid */
}

void ui_set_wifi(ui_wifi_state_t state)
{
    g_ui.wifi = state;
    scr_home_update_status();
    ui_offline_eval();
}

void ui_set_dnd(bool on, uint8_t until_hour)
{
    if (g_ui.dnd == on && g_ui.dnd_until_h == until_hour) return;
    g_ui.dnd = on;
    g_ui.dnd_until_h = until_hour;
    scr_home_update_inbox();      /* pill only: don't reset a grid scroll */
    scr_inbox_refresh();
    if (ui_screen_is(SCR_AMBIENT)) scr_ambient_refresh();
}

void ui_set_themes(const ui_theme_info_t *list, uint8_t count)
{
    if (count > UI_MAX_THEMES) count = UI_MAX_THEMES;
    memcpy(g_ui.themes, list, count * sizeof(ui_theme_info_t));
    g_ui.theme_count = count;
    scr_theme_picker_refresh();
}

void ui_theme_pending(const char *name)
{
    strlcpy(g_ui.theme_pending, name ? name : "", sizeof(g_ui.theme_pending));
    scr_theme_picker_refresh();
}

void ui_set_reaction_badges(const ui_reaction_t *list, uint8_t count)
{
    if (count > UI_MAX_CONTACTS) count = UI_MAX_CONTACTS;
    memcpy(g_ui.reaction_badges, list, count * sizeof(ui_reaction_t));
    g_ui.reaction_badge_count = count;
    scr_home_refresh();
}

void ui_set_peer_recording(const char *from_id, const char *from_name,
                           bool active)
{
    if (active) {
        strncpy(g_ui.peer_rec_from, from_id, UI_ID_LEN - 1);
        strncpy(g_ui.peer_rec_name, from_name, UI_NAME_LEN - 1);
        g_ui.peer_rec = true;
    } else {
        /* a stop only clears the indicator it started */
        if (strcmp(g_ui.peer_rec_from, from_id)) return;
        g_ui.peer_rec = false;
    }
    scr_record_peer_update();
}

/* ---------------- async events ---------------- */
void ui_notify_new_message(const char *sender_name)
{
    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_BELL "  New message from %s", sender_name);
    ui_toast(buf, COL_ACCENT);
    /* badge counts are refreshed when the app calls ui_set_inbox() */
}

void ui_flash_error(const char *text) { ui_toast(text, COL_DANGER); }
void ui_notice(const char *text)      { ui_toast(text, COL_ACCENT); }

void ui_wifi_setup_finished(void)
{
    ui_toast(LV_SYMBOL_OK "  Connected!", COL_OK);
    if (lv_screen_active() == s_screens[SCR_WIFI_SETUP])
        nav_to(SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

void ui_record_tick(uint16_t elapsed_s, uint16_t max_s) { scr_record_tick(elapsed_s, max_s); }
void ui_record_limit_reached(void)                      { scr_record_limit(); }
void ui_playback_progress(uint16_t p, uint16_t t)       { scr_playback_progress(p, t); }
void ui_playback_finished(void)                         { scr_playback_finished(); }
