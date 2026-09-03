/* Pip - application entry point.
 * Wires the UI callbacks to audio/storage/net modules and forwards module
 * events back to the UI under the LVGL lock. */
#include "audio.h"
#include "board.h"
#include "config.h"
#include "net_http.h"
#include "net_mqtt.h"
#include "net_wifi.h"
#include "ota.h"
#include "power.h"
#include "provisioning.h"
#include "storage.h"
#include "sync.h"
#include "theme.h"
#include "ui.h"
#include "voice.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app";

static char     s_rec_recipient[UI_ID_LEN];
static uint16_t s_rec_duration;
static bool     s_provisioning;

/* run a UI call under the LVGL lock; a timed-out update is dropped */
#define UI_LOCKED(stmt) do { \
        if (board_lock(200)) { stmt; board_unlock(); } \
        else ESP_LOGW(TAG, "UI update dropped (lock timeout): %s", #stmt); \
    } while (0)

static void reload_inbox_ui(void)
{
    static ui_message_t list[UI_MAX_MESSAGES];
    uint8_t n = storage_load_inbox(list, UI_MAX_MESSAGES);
    UI_LOCKED(ui_set_inbox(list, n));
}

/* ================= UI -> app ================= */
static void cb_record_start(const char *contact_id)
{
    strlcpy(s_rec_recipient, contact_id, sizeof(s_rec_recipient));
    power_hold(true);
    audio_record_start();
    /* non-blocking enqueue, not a network call: allowed here */
    net_mqtt_publish_presence(s_rec_recipient, true);
}

static void cb_record_stop(void)
{
    audio_record_stop();
    net_mqtt_publish_presence(s_rec_recipient, false);
}

static bool cb_record_send(void)
{
    if (!audio_wait_record_done(2000)) {
        UI_LOCKED(ui_flash_error("Still saving - try again"));
        return false;
    }
    if (s_rec_duration == 0) {
        UI_LOCKED(ui_flash_error("Nothing recorded"));
        return false;
    }
    char uuid[UI_ID_LEN];
    snprintf(uuid, sizeof(uuid), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    char final_path[96];
    snprintf(final_path, sizeof(final_path), OUTBOX_DIR "/%s.vmsg", uuid);
    if (rename(OUTBOX_DIR "/rec_tmp.vmsg", final_path) != 0) {
        ESP_LOGE(TAG, "outbox rename failed");
        UI_LOCKED(ui_flash_error("Could not save message"));
        return false;
    }
    storage_outbox_meta_write(uuid, s_rec_recipient, s_rec_duration);
    sync_touch_contact(s_rec_recipient);
    sync_kick();
    audio_play_chime(CHIME_SENT);
    return true;
}

/* voice-flow twins of the record callbacks: same recipient/presence
 * bookkeeping, but the capture auto-stops on trailing silence */
static void cb_voice_record_start(const char *contact_id)
{
    strlcpy(s_rec_recipient, contact_id, sizeof(s_rec_recipient));
    power_hold(true);
    audio_record_start_vad();
    net_mqtt_publish_presence(s_rec_recipient, true);
}

static void cb_record_cancel(void)
{
    s_rec_duration = 0;
    power_hold(false);
    audio_record_cancel();   /* audio task deletes the file once closed */
    net_mqtt_publish_presence(s_rec_recipient, false);  /* extra stop after
                                a normal stop is harmless server-side */
}

static void cb_play(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.vmsg", msg_id);
    power_hold(true);
    audio_play_file(path);
}

static void cb_stop_playback(void)
{
    power_hold(false);
    audio_stop();
}

static void cb_delete(const char *msg_id)
{
    static char id[UI_ID_LEN];
    strlcpy(id, msg_id, sizeof(id));
    storage_inbox_delete(id);
    storage_trash_add(id);            /* sync task performs the HTTP delete;
                                         no network calls in UI callbacks */
    reload_inbox_ui();
    sync_kick();
}

static void cb_heard(const char *msg_id)
{
    storage_inbox_mark_heard(msg_id);
    reload_inbox_ui();
}

static void cb_inbox_opened(void) { sync_kick(); }

static void cb_react(const char *msg_id, const char *key)
{
    static char id[UI_ID_LEN];
    strlcpy(id, msg_id, sizeof(id));
    storage_inbox_set_reaction(id, key);  /* instant local feedback */
    storage_react_add(id, key);           /* sync task does the POST;
                                             no network in UI callbacks */
    reload_inbox_ui();
    sync_kick();
}

static void cb_reactions_seen(const char *contact_id)
{
    /* the UI already dropped its badge (ui_react_mark_seen) */
    storage_rseen_add(contact_id);        /* sync task tells the server */
    sync_reactions_seen(contact_id);      /* keep the cache in step */
    sync_kick();
}

static bool cb_pin(const char *pin) { return config_check_pin(pin); }

static int8_t cb_wifi_rssi(void) { return net_wifi_rssi(); }

static void cb_volume(uint8_t v)
{
    config_save_volume(v);
    audio_set_volume(v);
}

static void cb_brightness(uint8_t v)
{
    config_save_brightness(v);
    power_set_brightness(v);
}

static bool cb_theme(const char *name, bool black_text)
{
    return theme_select(name, black_text);
}

static const uint16_t *cb_theme_thumb(const char *name, const char *ver)
{
    return theme_thumb_pixels(name, ver);
}

static bool cb_wifi_enter(char *ssid, char *pass)
{
    s_provisioning = true;
    return provisioning_start(ssid, pass);
}

static void cb_wifi_exit(void)
{
    s_provisioning = false;
    provisioning_stop();
}

/* ================= module events -> UI ================= */
static void ev_record_tick(uint16_t s, uint16_t max)
{
    /* presence keepalive: the server expires entries after 20 s, so
     * refresh every 10 s while recording (audio-task context, enqueue) */
    if (s > 0 && s % 10 == 0)
        net_mqtt_publish_presence(s_rec_recipient, true);
    UI_LOCKED(ui_record_tick(s, max));
}

static void ev_record_done(uint16_t dur)
{
    s_rec_duration = dur;
    power_hold(false);
    net_mqtt_publish_presence(s_rec_recipient, false);  /* harmless dup
                                after a manual stop; ends the VAD case */
    if (voice_session_active()) {       /* voice flow owns this recording:
                                           no record-screen side effects */
        voice_on_record_done(dur);
        return;
    }
    if (dur == 0) UI_LOCKED(ui_flash_error("Recording failed"));
    else UI_LOCKED(ui_record_limit_reached());  /* no-op unless limit hit */
}

static void ev_play_progress(uint16_t p, uint16_t t)
{
    UI_LOCKED(ui_playback_progress(p, t));
}

static void ev_play_done(void)
{
    power_hold(false);
    UI_LOCKED(ui_playback_finished());
}

static void ev_inbox_changed(void) { reload_inbox_ui(); }

static void ev_new_message(const char *sender)
{
    static char name[UI_NAME_LEN];
    strlcpy(name, sender, sizeof(name));
    /* a sync burst (e.g. catching up after being offline) delivers many
     * messages back to back - chime once per burst, not once per message */
    static int64_t last_chime_us;
    int64_t now = esp_timer_get_time();
    if (now - last_chime_us > 10 * 1000 * 1000) {
        last_chime_us = now;
        audio_play_chime(CHIME_RECEIVED);
    }
    UI_LOCKED(ui_notify_new_message(name));
}

static void ev_contacts_changed(void)
{
    uint8_t n = 0;
    const ui_contact_t *list = sync_contacts(&n);
    UI_LOCKED(ui_set_contacts(list, n));
}

static void ev_themes_changed(void)
{
    uint8_t n = 0;
    const ui_theme_info_t *list = sync_themes(&n);
    UI_LOCKED(ui_set_themes(list, n));
}

static void ev_reactions_changed(void)
{
    uint8_t n = 0;
    const ui_reaction_t *list = sync_reactions(&n);
    UI_LOCKED(ui_set_reaction_badges(list, n));
}

static void ev_battery(uint8_t pct, bool charging, bool present)
{
    UI_LOCKED(ui_set_battery(pct, charging, present));

    /* the battery poll doubles as the 30 s housekeeping tick: keep the
     * quiet-hours banner honest, and fetch the held night's messages the
     * moment the window closes (the 15 min poll would dawdle) */
    static bool dnd_known, dnd_was;
    bool dnd = sync_dnd_active();
    if (!dnd_known || dnd != dnd_was) {
        dnd_known = true;
        dnd_was = dnd;
        UI_LOCKED(ui_set_dnd(dnd, DND_END_H));
        if (!dnd) sync_kick();
    }
}

static void ev_wifi(net_state_t st)
{
    switch (st) {
    case NET_STATE_ONLINE:
        if (s_provisioning) {
            /* setup finished: the new network just worked, or a portal
             * sign-in went through the relay. Give the phone's /status
             * poll a moment to show "Connected!" before the AP drops. */
            s_provisioning = false;
            vTaskDelay(pdMS_TO_TICKS(4000));
            provisioning_stop();
            UI_LOCKED(ui_wifi_setup_finished());
        }
        UI_LOCKED(ui_set_wifi(UI_WIFI_ONLINE));
        net_mqtt_start();
        sync_kick();
        break;
    case NET_STATE_PORTAL:
        UI_LOCKED(ui_set_wifi(UI_WIFI_PORTAL));
        if (s_provisioning)
            provisioning_relay_now();  /* hand the sign-in to the phone */
        else
            UI_LOCKED(ui_notice("This WiFi needs a sign-in (Settings)"));
        break;
    case NET_STATE_DOWN:
        UI_LOCKED(ui_set_wifi(s_provisioning ? UI_WIFI_OFFLINE
                                             : UI_WIFI_CONNECTING));
        break;
    }
}

static void ev_mqtt_notify(const char *payload)
{
    /* events: {"msg_id":...} new message (implicit), {"event":"firmware"},
     * {"event":"reaction",...}, {"event":"recording",...},
     * {"event":"contacts"} perms changed server-side. Anything
     * unparseable or unknown degrades to a sync - that keeps this
     * forward-compatible the same way old firmware handles our events. */
    cJSON *root = payload ? cJSON_Parse(payload) : NULL;
    const cJSON *ev = root ? cJSON_GetObjectItem(root, "event") : NULL;
    const char *event = cJSON_IsString(ev) ? ev->valuestring : "";

    if (!strcmp(event, "firmware")) {
        ota_kick();
    } else if (!strcmp(event, "contacts")) {
        sync_contacts_kick();
    } else if (!strcmp(event, "voice")) {
        /* admin toggled voice control / prompts re-rendered: the flag
         * and manifest ride the contacts refresh (sync.c) */
        sync_contacts_kick();
    } else if (!strcmp(event, "recording")) {
        const cJSON *fr = cJSON_GetObjectItem(root, "from");
        const cJSON *fn = cJSON_GetObjectItem(root, "from_name");
        const cJSON *st = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(fr) && cJSON_IsString(st))
            UI_LOCKED(ui_set_peer_recording(
                fr->valuestring,
                cJSON_IsString(fn) ? fn->valuestring : fr->valuestring,
                !strcmp(st->valuestring, "start")));
    } else if (!strcmp(event, "reaction")) {
        const cJSON *fr = cJSON_GetObjectItem(root, "from");
        const cJSON *fn = cJSON_GetObjectItem(root, "from_name");
        const cJSON *re = cJSON_GetObjectItem(root, "reaction");
        if (cJSON_IsString(fr) && cJSON_IsString(re)) {
            const char *name = cJSON_IsString(fn) ? fn->valuestring
                                                  : fr->valuestring;
            /* quiet hours cover reactions too - the badge still lands
             * below, it just arrives without a chime in the dark */
            if (!sync_quiet_hold()) {
                audio_play_chime(CHIME_RECEIVED);
                UI_LOCKED(ui_notify_reaction(name, re->valuestring));
            }
            /* fires reactions_changed -> home badge (MQTT task: file
             * I/O is fine here, this is not a UI callback) */
            sync_reaction_add(fr->valuestring, name, re->valuestring);
        }
    } else {
        /* new message. Servers from 0.1.34 on put the sender, colour,
         * timestamp and duration in the notify, which is everything the
         * .meta needs - so sync can download the audio straight away
         * instead of asking for the inbox listing first. An older server,
         * or a payload too long for the buffer above, leaves us with no
         * metadata and we fall back to listing. */
        const cJSON *mid = cJSON_GetObjectItem(root, "msg_id");
        const cJSON *fr  = cJSON_GetObjectItem(root, "from");
        const cJSON *fn  = cJSON_GetObjectItem(root, "from_name");
        const cJSON *co  = cJSON_GetObjectItem(root, "color");
        const cJSON *ts  = cJSON_GetObjectItem(root, "ts");
        const cJSON *du  = cJSON_GetObjectItem(root, "dur");
        if (cJSON_IsString(mid) && cJSON_IsString(fr) && cJSON_IsNumber(ts)) {
            http_inbox_item_t m = {0};
            strlcpy(m.id, mid->valuestring, UI_ID_LEN);
            strlcpy(m.sender_id, fr->valuestring, UI_ID_LEN);
            strlcpy(m.sender_name,
                    cJSON_IsString(fn) ? fn->valuestring : fr->valuestring,
                    UI_NAME_LEN);
            m.sender_color = cJSON_IsString(co)
                           ? (uint32_t)strtoul(co->valuestring, NULL, 16)
                           : 0x999999;
            /* m.when stays empty: storage renders the clock from ts at
             * load time, so the string only matters for pre-epoch metas */
            m.ts = (uint32_t)ts->valuedouble;
            m.duration_s = cJSON_IsNumber(du) ? (uint16_t)du->valueint : 0;
            sync_message_hint(&m);
        } else {
            sync_inbox_kick();
        }
    }
    cJSON_Delete(root);
}

/* ================= physical buttons ================= */
/* BOOT (top) -> record to most recent contact; PWR key (bottom) -> inbox.
 * The PWR press latches in the PMU, so 100 ms polling never misses it;
 * BOOT is a plain GPIO sampled on the same cadence. */
static void button_task(void *arg)
{
    (void)arg;
    bool boot_was = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool boot_now = board_boot_btn_pressed();
        if (boot_now && !boot_was) {
            power_user_activity();
            UI_LOCKED(ui_open_record_recent());
        }
        boot_was = boot_now;
        if (board_pwr_key_event()) {
            power_user_activity();
            UI_LOCKED(ui_open_inbox());
        }
    }
}

/* ================= boot ================= */
#define HEAPLOG(phase) ESP_LOGI(TAG, "heap after " phase ": %u internal (%u largest)", \
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))

void app_main(void)
{
    ESP_LOGI(TAG, "Pip starting");

    config_load();
    HEAPLOG("config");
    board_init();                       /* display + touch + LVGL running */
    /* Panel dark until the greeting is ready. Everything between here and
     * the splash reads flash and rebuilds widgets; none of it is worth
     * watching, and showing it meant the animation had to share the CPU -
     * and the LVGL lock - with the heaviest part of boot. */
    board_set_brightness(0);
    HEAPLOG("board");
    storage_init();

    static const ui_callbacks_t ui_cbs = {
        .record_start = cb_record_start,
        .record_stop = cb_record_stop,
        .record_send = cb_record_send,
        .record_cancel = cb_record_cancel,
        .play_message = cb_play,
        .stop_playback = cb_stop_playback,
        .delete_message = cb_delete,
        .message_heard = cb_heard,
        .inbox_opened = cb_inbox_opened,
        .react = cb_react,
        .reactions_seen = cb_reactions_seen,
        .voice_cancel = voice_cancel,
        .pin_check = cb_pin,
        .wifi_rssi = cb_wifi_rssi,
        .volume_changed = cb_volume,
        .brightness_changed = cb_brightness,
        .theme_selected = cb_theme,
        .theme_thumb = cb_theme_thumb,
        .wifi_setup_enter = cb_wifi_enter,
        .wifi_setup_exit = cb_wifi_exit,
    };
    UI_LOCKED(ui_init(&ui_cbs));
    UI_LOCKED(ui_set_owner_name(g_cfg.device_name));
    UI_LOCKED(ui_settings_set_levels(g_cfg.speaker_volume, g_cfg.brightness));
    reload_inbox_ui();
    theme_init();      /* 330 KB out of LittleFS, then a full re-theme
                          under the LVGL lock: the one step that froze
                          the greeting outright when they overlapped */
    HEAPLOG("ui");

    static const audio_events_t audio_ev = {
        .record_tick = ev_record_tick,
        .record_done = ev_record_done,
        .play_progress = ev_play_progress,
        .play_done = ev_play_done,
        .voice_hits = voice_on_hits,       /* audio task -> voice flow  */
        .prompt_done = voice_on_prompt_done,
        .voice_timeout = voice_on_timeout,
    };
    audio_init(&audio_ev);
    HEAPLOG("audio");

    static const voice_app_t voice_app = {
        .record_start = cb_voice_record_start,
        .record_send = cb_record_send,
        .record_cancel = cb_record_cancel,
        .mark_heard = cb_heard,
    };
    voice_init(&voice_app);
    if (g_cfg.voice_enabled)   /* server flag cached in NVS: listen from
                                  boot, refreshed on the next sync */
        voice_set_enabled(true);
    HEAPLOG("voice");

    static const sync_events_t sync_ev = {
        .inbox_changed = ev_inbox_changed,
        .new_message = ev_new_message,
        .contacts_changed = ev_contacts_changed,
        .themes_changed = ev_themes_changed,
        .reactions_changed = ev_reactions_changed,
    };
    sync_init(&sync_ev);

    /* Greeting. ui_splash_show() puts it on the panel synchronously, so
     * the light comes back on the splash itself - turning the backlight
     * up first showed a frame of home before the animation started.
     * Nothing else starts until the animation is done. */
    UI_LOCKED(ui_splash_show());
    board_set_brightness(g_cfg.brightness);
    vTaskDelay(pdMS_TO_TICKS(UI_SPLASH_MS + 300));

    /* Both poll the PMU over I2C and power_init's first battery read
     * fires immediately; neither is worth sharing the greeting with,
     * and nothing can interrupt a 2 s animation anyway. */
    power_init(ev_battery);             /* battery poll + inactivity dimming */
    xTaskCreate(button_task, "buttons", 3072, NULL, 3, NULL);

    /* Network last. esp_wifi_start() does RF calibration and brings up
     * driver tasks at priority 23, which preempt the LVGL task in bursts;
     * overlapping the greeting cost it frames for no benefit - nothing
     * can arrive before the box is on home anyway. */
    net_mqtt_init(ev_mqtt_notify);
    HEAPLOG("mqtt");
    net_wifi_init(ev_wifi);
    net_wifi_start();

    ota_init();

    ESP_LOGI(TAG, "boot complete; heap: %u internal (%u largest), %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* machine-readable hardware verdict, matched by the web flasher's
     * verify step (display is ok by construction - board_init aborts) */
    ESP_LOGI(TAG, "PIP-HW display=ok touch=%s pmu=%s codec=%s",
             board_touch_ok() ? "ok" : "missing",
             board_pmu_ok()   ? "ok" : "missing",
             audio_codec_ok() ? "ok" : "err");

    /* fresh box (web-flashed, no WiFi yet): open WiFi setup with its QR
     * instead of sitting on "connecting". The splash is already over by
     * here - the network block above waits it out. */
    wifi_cred_t net0;
    if (config_wifi_list(&net0, 1) == 0)
        UI_LOCKED(ui_open_wifi_setup());
}
