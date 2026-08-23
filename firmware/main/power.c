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
/* Ambient: medium, ~35% of full drive. One compile-time constant so it can
 * be tuned on the bench; g_cfg.brightness stays the "awake" level only.
 * Raising it means shortening AMBIENT_SHIFT_MS (ui_ambient.c) to keep the
 * per-pixel dose where the burn-in argument put it. */
#define AMBIENT_LEVEL     90

/* Sleeping with unheard mail lands on PWR_AMBIENT instead of PWR_OFF: a
 * black panel with one small icon rather than a dark one. It is a state of
 * its own and not just "PWR_OFF with the backlight up" because PWR_OFF
 * leaves the whole home screen rendered underneath - contact grid, inbox
 * pill, badge - and lighting that dimly is the worst case for both burn-in
 * and legibility. */
typedef enum { PWR_FULL, PWR_DIM, PWR_AMBIENT, PWR_OFF } pwr_state_t;

static battery_cb_t s_bat_cb;
static pwr_state_t  s_state = PWR_FULL;
static uint8_t      s_full_brightness = 200;
static volatile bool s_hold;
static lv_obj_t    *s_shield;   /* topmost transparent layer while dimmed/off:
                                   the wake-up tap must not reach the UI */

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
            /* unlike PWR_OFF the panel is lit here, so blank it before the
             * screen swap - the icon must not be seen sliding away */
            board_set_brightness(0);
            ui_ambient_exit();       /* we are the LVGL task with the lock
                                        held; no board_lock() needed        */
        }
        ui_splash_show();            /* full sleep gets the hello again;
                                        paints while the panel is dark so
                                        the old screen never flashes */
        ota_kick();                  /* someone's here: good moment to pick
                                        up a pending firmware update */
    }
    net_wifi_retry_now();            /* their hotspot may be awake again */
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

/* Leaving ambient always blanks first: unlike PWR_OFF the panel *is* lit,
 * so the swap back to home would otherwise be visible. The caller sets the
 * new brightness afterwards. */
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
        case PWR_AMBIENT: board_set_brightness(AMBIENT_LEVEL);     break;
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
        if (board_lock(50)) {
            if (s_hold) lv_display_trigger_activity(NULL);
            idle = lv_display_get_inactive_time(NULL);
            want_ambient = ui_ambient_wanted();
            board_unlock();
        }
        /* Re-decided every second, so a message arriving while the panel is
         * fully off lights the icon within 1 s, and hearing the last one
         * drops ambient back to off just as quickly. */
        if (idle > OFF_AFTER_MS)      apply(want_ambient ? PWR_AMBIENT : PWR_OFF);
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
    xTaskCreate(power_task, "power", 4096, NULL, 2, NULL);
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
