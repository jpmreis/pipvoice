/**
 * Pip UI - public API
 *
 * The UI layer owns all LVGL screens and widgets. The application layer
 * (audio, networking, storage) talks to it only through this header:
 *   - app -> UI: ui_* functions below (call from the LVGL task/lock)
 *   - UI -> app: ui_callbacks_t function pointers, registered at init
 *
 * Target: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, SH8601/FT3168), LVGL 9.
 */
#ifndef PIP_UI_H
#define PIP_UI_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_CONTACTS   12
#define UI_MAX_MESSAGES   60
#define UI_MAX_THEMES     12
#define UI_ID_LEN         40   /* fits 32-hex server message ids + NUL */
#define UI_NAME_LEN       24

typedef struct {
    char     id[UI_ID_LEN];       /* device id of the contact            */
    char     name[UI_NAME_LEN];   /* display name                        */
    uint32_t color;               /* 0xRRGGBB avatar color from server   */
    uint32_t last_used;           /* epoch secs of last send/receive;
                                     list is kept sorted newest-first    */
} ui_contact_t;

#define UI_REACT_KEY_LEN  8    /* longest key is "quest" */

typedef struct {
    char     id[UI_ID_LEN];       /* message id                          */
    char     sender_id[UI_ID_LEN];/* device id of the sender (for reply) */
    char     sender_name[UI_NAME_LEN];
    uint32_t sender_color;        /* 0xRRGGBB                            */
    char     when[20];            /* preformatted, e.g. "Tue 14:05"      */
    uint16_t duration_s;
    bool     heard;
    char     reaction[UI_REACT_KEY_LEN]; /* own reaction key; "" = none  */
} ui_message_t;

/* an unseen reaction to a message this user sent (badge on the contact) */
typedef struct {
    char from[UI_ID_LEN];         /* reactor's username                  */
    char from_name[UI_NAME_LEN];
    char key[UI_REACT_KEY_LEN];   /* heart/up/down/haha/bang/quest/joy   */
} ui_reaction_t;

typedef enum {
    UI_WIFI_OFFLINE,
    UI_WIFI_CONNECTING,
    UI_WIFI_ONLINE,
    UI_WIFI_PORTAL,     /* on WiFi but a captive portal wants a sign-in */
} ui_wifi_state_t;

#define UI_THEME_VER_LEN  12   /* 8-hex server content hash + NUL */

/* picker thumbnail geometry: 3-column grid on the 368px panel, same
 * 368:448 aspect as the panel; must match the server's THUMB_W/H */
#define UI_THEME_THUMB_W     108
#define UI_THEME_THUMB_H     132
#define UI_THEME_THUMB_BYTES (UI_THEME_THUMB_W * UI_THEME_THUMB_H * 2)

typedef struct {
    char name[UI_NAME_LEN];       /* server id, e.g. "sea"               */
    char label[UI_NAME_LEN];      /* shown in the picker, e.g. "Sea"     */
    char ver[UI_THEME_VER_LEN];   /* content hash; keys the thumb cache  */
    bool black_text;              /* text color when drawn on this bg    */
} ui_theme_info_t;

/* ---- UI -> app callbacks (all invoked from the LVGL task) ---- */
typedef struct {
    /* recording */
    void (*record_start)(const char *contact_id);
    void (*record_stop)(void);              /* stop capture, keep buffer   */
    bool (*record_send)(void);              /* queue buffered msg to outbox;
                                               false = not sent (error shown) */
    void (*record_cancel)(void);            /* discard buffer              */
    /* playback */
    void (*play_message)(const char *msg_id);
    void (*stop_playback)(void);
    void (*delete_message)(const char *msg_id);
    void (*message_heard)(const char *msg_id);  /* mark heard / ack        */
    void (*inbox_opened)(void);             /* inbox screen shown (pull)   */
    /* reactions */
    void (*react)(const char *msg_id, const char *key);   /* "" = clear   */
    void (*reactions_seen)(const char *contact_id);       /* badge tapped */
    /* settings & provisioning */
    bool (*pin_check)(const char *pin);     /* true if PIN correct         */
    /* AP signal in dBm for the status-bar strength icon, 0 = unknown;
     * pulled from the UI's own status tick, not pushed */
    int8_t (*wifi_rssi)(void);
    void (*volume_changed)(uint8_t pct);    /* 0..100                      */
    void (*brightness_changed)(uint8_t val);/* 0..255                      */
    /* background picked in settings; name "" = none. The app downloads the
     * image and calls ui_set_background() when it's ready. Returns false
     * when the pick was ignored (a download is already running). */
    bool (*theme_selected)(const char *name, bool black_text);
    /* picker thumbnail pixels (UI_THEME_THUMB_W x _H RGB565, PSRAM,
     * app-owned, stays valid) or NULL when not cached yet */
    const uint16_t *(*theme_thumb)(const char *name, const char *ver);
    /* Enter WiFi setup: app starts SoftAP + portal (relaying the phone
     * to the network when one is connected - hotel sign-ins included)
     * and fills ssid/pass (caller-owned buffers, >= 33 / 65 bytes).
     * Return false on failure. */
    bool (*wifi_setup_enter)(char *ap_ssid, char *ap_pass);
    void (*wifi_setup_exit)(void);
} ui_callbacks_t;

/* ---- lifecycle ---- */
void ui_init(const ui_callbacks_t *cbs);    /* build screens, show home    */

/* ---- data from app ---- */
void ui_set_owner_name(const char *name);   /* device owner, for greetings */
void ui_settings_set_levels(uint8_t volume_pct, uint8_t brightness);
void ui_set_contacts(const ui_contact_t *list, uint8_t count);
void ui_set_inbox(const ui_message_t *list, uint8_t count);
void ui_set_battery(uint8_t pct, bool charging, bool present);
void ui_set_wifi(ui_wifi_state_t state);
/* quiet hours: incoming mail is being held until until_hour (local).
 * Shows the sleeping-inbox banner on home and in the inbox. */
void ui_set_dnd(bool on, uint8_t until_hour);
void ui_set_themes(const ui_theme_info_t *list, uint8_t count);

/* Apply (or clear, px == NULL) the background on home/record/inbox.
 * px: 368x448 RGB565 little-endian, caller-owned, must stay alive while
 * set (it is referenced, not copied). */
void ui_set_background(const uint16_t *px, const char *name, bool black_text);

/* A background is being fetched (name) or the fetch ended (""): the
 * picker rings that tile and spins on it until it lands. */
void ui_theme_pending(const char *name);

/* unseen reactions to sent messages, latest per contact (chips on the
 * home grid); refreshes the home screen */
void ui_set_reaction_badges(const ui_reaction_t *list, uint8_t count);

/* ---- async events from app ---- */
void ui_notify_new_message(const char *sender_name); /* toast + badge      */
void ui_notify_reaction(const char *from_name, const char *key); /* toast  */
/* "<from> is recording to you right now" (record screen indicator);
 * active=true refreshes the 20 s display TTL */
void ui_set_peer_recording(const char *from_id, const char *from_name,
                           bool active);
void ui_record_tick(uint16_t elapsed_s, uint16_t max_s);
void ui_record_limit_reached(void);                  /* auto-stop at max   */
void ui_playback_progress(uint16_t pos_s, uint16_t total_s);
void ui_playback_finished(void);
void ui_flash_error(const char *text);               /* brief error toast  */
void ui_notice(const char *text);                    /* brief info toast   */
void ui_wifi_setup_finished(void);   /* setup/sign-in worked: toast +
                                        leave the wifi screen             */
void ui_open_wifi_setup(void);       /* start setup mode + QR screen (also
                                        auto-entered when no networks)    */

/* physical-button navigation (call under the LVGL lock); no-ops while on
 * the record or wifi-setup screens to avoid yanking an active session */
void ui_open_record_recent(void);    /* record screen, most recent contact */
void ui_open_inbox(void);

/* ---- voice-control status screen (call under the LVGL lock) ----
 * Big-type display of what the voice flow is asking; driven by voice.c.
 * show() navigates to the screen unless the record/wifi screens hold the
 * nav lock (the flow still runs; the question just isn't on the panel);
 * close() returns home only if the voice screen is the one showing. */
void ui_voice_show(const char *line1, const char *line2);
void ui_voice_close(void);

/* Length of the boot greeting. app_main keeps the heavy parts of boot
 * (flash reads, network bring-up) out of this window: the animation
 * shares a CPU and the LVGL lock with whatever else is running. */
#define UI_SPLASH_MS 2100

/* Animated "Hello <owner>" splash; returns to home when it finishes.
 * Instant and synchronous: it is on the panel when this returns, so the
 * caller can raise the backlight onto it. Call under the LVGL lock
 * (boot, or wake from full display-off). */
void ui_splash_show(void);

/* ---- ambient (sleeping clock) screen ----
 * A sleeping box shows a dim amber clock wandering on black instead of
 * going fully dark; with unheard mail the splash wordmark's dot docks
 * onto it as its full stop and the panel brightens. power.c owns the
 * brightness and the state ladder. All of these run in the LVGL
 * task's world: call them from the LVGL task or under board_lock(). */
bool ui_ambient_wanted(void);   /* clock known or mail waits; nav not locked */
bool ui_ambient_mail(void);     /* unheard mail, not DND: dot + brighter panel */
bool ui_ambient_enter(void);    /* swap to it + start the shift timer;
                                   false = refused, caller should go dark */
void ui_ambient_exit(void);     /* stop the timer, restore home; idempotent */
void ui_ambient_wake_to_splash(void); /* touch-wake, panel stays lit: the
                                   dot glides home into the splash wordmark
                                   while the letters fall in */

#ifdef __cplusplus
}
#endif
#endif /* PIP_UI_H */
