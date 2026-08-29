#include "power.h"
#include "board.h"
#include "config.h"
#include "net_wifi.h"
#include "ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui.h"

#define DIM_AFTER_MS      (15 * 1000)
#define OFF_AFTER_MS      (45 * 1000)
#define BATTERY_POLL_MS   (30 * 1000)
#define DIM_LEVEL         40
/* Ambient runs at two levels, picked by the mail state (ui_ambient_mail):
 * a quiet night's clock keeps the old medium, ~35% of full drive; unheard
 * mail steps up to medium-high so the waiting dot carries across a room.
 * Compile-time constants so they can be tuned on the bench;
 * g_cfg.brightness stays the "awake" level only. */
#define AMBIENT_LEVEL       90
#define AMBIENT_MAIL_LEVEL  150

/* Sleeping lands on PWR_AMBIENT instead of PWR_OFF: a black panel with a
 * dim clock (plus the mail dot when something waits) rather than a dark
 * one. It is a state of its own and not just "PWR_OFF with the backlight
 * up" because PWR_OFF leaves the whole home screen rendered underneath -
 * contact grid, inbox pill, badge - and lighting that dimly is the worst
 * case for both burn-in and legibility. PWR_OFF remains for a box that
 * can't show the clock yet (no SNTP) and has no mail, and as apply()'s
 * fallback when the ambient enter is refused. */
typedef enum { PWR_FULL, PWR_DIM, PWR_AMBIENT, PWR_OFF } pwr_state_t;

static battery_cb_t s_bat_cb;
static pwr_state_t  s_state = PWR_FULL;
static uint8_t      s_full_brightness = 200;
static uint8_t      s_ambient_level = AMBIENT_LEVEL;   /* current mood */
static volatile bool s_hold;
static lv_obj_t    *s_shield;   /* topmost transparent layer while dimmed/off:
                                   the wake-up tap must not reach the UI */

/* touch-wake from ambient: the panel stays lit, so brightness eases from
 * AMBIENT_LEVEL to full over the splash instead of stepping */
static void bright_exec(void *var, int32_t v)
{
    LV_UNUSED(var);
    board_set_brightness((uint8_t)v);
}

static void brightness_ramp(uint8_t from, uint8_t to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_full_brightness);   /* stable address as anim id */
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_exec_cb(&a, bright_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void shield_pressed(lv_event_t *e)
{
    /* LVGL task context (lock held): wake and swallow the tap */
    LV_UNUSED(e);
    bool was_ambient = (s_state == PWR_AMBIENT);
    bool was_asleep  = (s_state == PWR_OFF) || was_ambient;
    s_state = PWR_FULL;
    if (s_shield) {
        lv_obj_delete_async(s_shield);
        s_shield = NULL;
    }
    if (was_asleep) {
        if (was_ambient) {
            /* the panel is lit and stays lit: no blank, no home flash -
             * the wandering dot glides into the splash wordmark while
             * brightness eases up (we are the LVGL task with the lock
             * held; no board_lock() needed) */
            ui_ambient_wake_to_splash();
            brightness_ramp(s_ambient_level, s_full_brightness);
        } else {
            ui_splash_show();        /* full sleep gets the hello again;
                                        paints while the panel is dark so
                                        the old screen never flashes */
        }
        ota_kick();                  /* someone's here: good moment to pick
                                        up a pending firmware update */
    }
    net_wifi_retry_now();            /* their hotspot may be awake again */
    if (!was_ambient)
        board_set_brightness(s_full_brightness);
}

static void shield_set(bool on)   /* caller holds the LVGL lock */
{
    if (on && !s_shield) {
        s_shield = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_shield, shield_pressed, LV_EVENT_PRESSED, NULL);
    } else if (!on && s_shield) {
        lv_obj_delete(s_shield);
        s_shield = NULL;
    }
}

/* Every way out of ambient except the touch-wake (shield_pressed, which
 * choreographs the dot into the splash on a lit panel) blanks first:
 * unlike PWR_OFF the panel *is* lit, so the swap back to home would
 * otherwise be visible. The caller sets the new brightness afterwards. */
static void leave_ambient(void)
{
    board_set_brightness(0);
    if (board_lock(200)) {
        ui_ambient_exit();
        board_unlock();
    }
}

static void apply(pwr_state_t st)
{
    if (st == s_state) return;

    /* one place for ambient -> off and ambient -> full alike */
    if (s_state == PWR_AMBIENT) leave_ambient();

    if (st == PWR_AMBIENT) {
        board_set_brightness(0);          /* swap under a dark panel */
        bool ok = false;
        if (board_lock(200)) {
            ok = ui_ambient_enter();      /* false = the UI refused it */
            board_unlock();
        }
        /* A refusal or a lock timeout must leave the panel dark, never lit
         * on the wrong screen. Going to PWR_OFF (rather than holding the
         * old state) also means the 1 Hz ladder retries the enter a second
         * later, since the state then differs from the one it asks for. */
        if (!ok) st = PWR_OFF;
    }

    s_state = st;
    switch (st) {
        case PWR_FULL:    board_set_brightness(s_full_brightness); break;
        case PWR_DIM:     board_set_brightness(DIM_LEVEL);         break;
        case PWR_AMBIENT: board_set_brightness(s_ambient_level);   break;
        case PWR_OFF:     board_set_brightness(0);                 break;
    }
    if (board_lock(50)) {
        shield_set(st != PWR_FULL);
        board_unlock();
    }
}

static void power_task(void *arg)
{
    (void)arg;
    uint32_t bat_elapsed = BATTERY_POLL_MS;   /* poll immediately at boot */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* inactivity: LVGL tracks last input event for us. While recording
         * or playing we keep refreshing the activity timer, so the countdown
         * only starts once the recording/playback actually ends. */
        uint32_t idle = 0;
        bool     want_ambient = false;
        bool     ambient_mail = false;
        if (board_lock(50)) {
            if (s_hold) lv_display_trigger_activity(NULL);
            idle = lv_display_get_inactive_time(NULL);
            want_ambient = ui_ambient_wanted();
            ambient_mail = ui_ambient_mail();
            board_unlock();
        }
        /* Re-decided every second, so a message arriving while the box
         * sleeps brightens the clock and docks the dot within 1 s (the
         * dot itself is scr_ambient_refresh's job, from ui_set_inbox),
         * and hearing the last one settles it back down just as quickly. */
        if (idle > OFF_AFTER_MS) {
            uint8_t alevel = ambient_mail ? AMBIENT_MAIL_LEVEL : AMBIENT_LEVEL;
            if (want_ambient && s_state == PWR_AMBIENT &&
                alevel != s_ambient_level)
                board_set_brightness(alevel);   /* mood change, same state */
            s_ambient_level = alevel;           /* what apply() will set   */
            apply(want_ambient ? PWR_AMBIENT : PWR_OFF);
        }
        else if (idle > DIM_AFTER_MS) apply(PWR_DIM);
        else                          apply(PWR_FULL);

        bat_elapsed += 1000;
        if (bat_elapsed >= BATTERY_POLL_MS) {
            bat_elapsed = 0;
            if (s_bat_cb)
                s_bat_cb(board_battery_pct(), board_battery_charging(),
                         board_battery_present());
        }
    }
}

void power_init(battery_cb_t bat_cb)
{
    s_bat_cb = bat_cb;
    s_full_brightness = g_cfg.brightness;
    board_set_brightness(s_full_brightness);
    /* 8 KB, not 4: entering ambient paints the sleep screen with
     * lv_refr_now() on THIS task's stack (apply -> ui_ambient_enter), and
     * since the clock that render rasterizes a FONT_BIG label rather than
     * just the 14 px dot - the deeper label draw path overflowed the old
     * 4 KB stack (canary panic, so the box rebooted at the sleep timer). */
    xTaskCreate(power_task, "power", 8192, NULL, 2, NULL);
}

void power_user_activity(void)
{
    /* touching the screen already resets LVGL's inactivity timer;
     * this hook exists for the physical buttons (and IMU lift later) */
    if (s_state == PWR_OFF || s_state == PWR_AMBIENT)
        ota_kick();                       /* wake-from-sleep: check updates */
    net_wifi_retry_now();                 /* hotspot may be awake again     */
    /* No splash here, on purpose (the user pressed a button to reach a
     * specific screen), but apply() still has to leave ambient first so the
     * icon is never briefly lit at full brightness before the caller's
     * ui_open_record_recent() / ui_open_inbox() swaps the screen. */
    apply(PWR_FULL);
}

void power_hold(bool on)
{
    s_hold = on;
    if (on) apply(PWR_FULL);
}

void power_set_brightness(uint8_t v)
{
    s_full_brightness = v;
    if (s_state == PWR_FULL) board_set_brightness(v);
}
