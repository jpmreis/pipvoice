/* Pip UI - internal shared definitions (not part of public API) */
#ifndef PIP_UI_INTERNAL_H
#define PIP_UI_INTERNAL_H

#include "ui.h"

/* ---------- design tokens (AMOLED: true black saves power) ---------- */
#define COL_BG        lv_color_hex(0x000000)
#define COL_SURFACE   lv_color_hex(0x16161C)
#define COL_SURFACE2  lv_color_hex(0x24242E)
#define COL_TEXT      lv_color_hex(0xF2F0EA)
#define COL_TEXT_DIM  lv_color_hex(0x8B8B96)
#define COL_ACCENT    lv_color_hex(0xFFB300)   /* warm amber - record/action */
#define COL_OK        lv_color_hex(0x3DDC84)   /* send / online              */
#define COL_DANGER    lv_color_hex(0xFF5252)   /* delete / recording dot     */

#define FONT_BIG      (&lv_font_montserrat_40)
#define FONT_TITLE    (&lv_font_montserrat_28)
#define FONT_BODY     (&lv_font_montserrat_20)
#define FONT_SMALL    (&lv_font_montserrat_16)

#define SCREEN_W 368
#define SCREEN_H 448

/* ---------- shared state ---------- */
typedef struct {
    ui_callbacks_t cb;

    char          owner_name[UI_NAME_LEN];   /* this device's own name */
    ui_contact_t  contacts[UI_MAX_CONTACTS];
    uint8_t       contact_count;
    ui_message_t  inbox[UI_MAX_MESSAGES];
    uint8_t       inbox_count;
    uint8_t       unheard_count;

    uint8_t         battery_pct;
    bool            charging;
    bool            battery_present;
    ui_wifi_state_t wifi;
    bool            dnd;              /* quiet hours: mail held on server */
    uint8_t         dnd_until_h;      /* local hour it resumes            */

    ui_theme_info_t themes[UI_MAX_THEMES];   /* picker options from server */
    uint8_t         theme_count;
    char            theme_name[UI_NAME_LEN]; /* active theme; "" = none    */
    char            theme_pending[UI_NAME_LEN]; /* downloading now; "" none */
    bool            theme_active;
    bool            theme_black_text;

    char selected_contact_id[UI_ID_LEN];
    char selected_contact_name[UI_NAME_LEN];
    char playing_msg_id[UI_ID_LEN];

    /* unseen reactions to sent messages, latest per contact */
    ui_reaction_t reaction_badges[UI_MAX_CONTACTS];
    uint8_t       reaction_badge_count;

    /* who is recording to this user right now ("" / false = nobody) */
    char peer_rec_from[UI_ID_LEN];
    char peer_rec_name[UI_NAME_LEN];
    bool peer_rec;
} ui_state_t;

extern ui_state_t g_ui;

/* ---------- navigation ---------- */
typedef enum {
    SCR_HOME, SCR_RECORD, SCR_INBOX, SCR_PLAYBACK,
    SCR_PINPAD, SCR_SETTINGS, SCR_THEME_PICKER, SCR_WIFI_SETUP, SCR_SPLASH,
    SCR_COUNT,
} ui_screen_id_t;

void nav_to(ui_screen_id_t id, lv_screen_load_anim_t anim);
/* which screen nav_to last selected (reliable mid-animation, unlike
 * lv_screen_active()) */
bool ui_screen_is(ui_screen_id_t id);

/* Each screen module: build once at init, refresh on show. */
lv_obj_t *scr_home_create(void);     void scr_home_refresh(void);
void scr_home_update_status(void);   void scr_home_update_inbox(void);
lv_obj_t *scr_record_create(void);   void scr_record_show(void);
lv_obj_t *scr_inbox_create(void);    void scr_inbox_refresh(void);
lv_obj_t *scr_playback_create(void); void scr_playback_show(const ui_message_t *msg);
lv_obj_t *scr_pinpad_create(void);   void scr_pinpad_reset(void);
/* Run on_ok() instead of opening Settings after the next correct PIN.
 * Arm it just before nav_to(SCR_PINPAD); the pad clears it when it is
 * left, whether the PIN was right or the user backed out. */
void scr_pinpad_arm(void (*on_ok)(void));
lv_obj_t *scr_settings_create(void);
lv_obj_t *scr_theme_picker_create(void); void scr_theme_picker_refresh(void);

/* re-apply background image + on-bg text colors (theme changed) */
void scr_home_apply_theme(void);
void scr_record_apply_theme(void);
void scr_inbox_apply_theme(void);
lv_obj_t *scr_wifi_setup_create(void);
void scr_wifi_setup_show(const char *ssid, const char *pass);
lv_obj_t *scr_splash_create(void);   void scr_splash_show(void);
void scr_splash_prewarm(void);       /* draw once cold, then animate */

/* record screen hooks used by ui_core dispatchers */
void scr_record_tick(uint16_t elapsed_s, uint16_t max_s);
void scr_record_limit(void);
void scr_record_peer_update(void);   /* re-evaluate the peer-rec indicator */

/* ---------- reactions (ui_react.c) ---------- */
/* modal emoji picker for a message (opened by long-pressing its row) */
void scr_react_open(const ui_message_t *m);
/* baked emoji image for a key (heart/up/down/joy), NULL for text keys */
const lv_image_dsc_t *ui_emoji_img(const char *key);
/* display text for the text keys ("Ha Ha", "!!", "?"), NULL otherwise */
const char *ui_reaction_text(const char *key);
/* unseen-reaction badge for a contact, NULL when none */
const ui_reaction_t *ui_react_badge_for(const char *contact_id);
/* drop a contact's badge from g_ui (no home refresh - safe inside that
 * tile's own event) and tell the app via cb.reactions_seen */
void ui_react_mark_seen(const char *contact_id);
void scr_playback_progress(uint16_t pos_s, uint16_t total_s);
void scr_playback_finished(void);

/* ---------- offline nag (ui_offline.c) ---------- */
/* re-decide whether the "No WiFi" overlay belongs on screen (wifi state,
 * grace period, snooze, home-screen-only) */
void ui_offline_eval(void);
void ui_offline_hide(void);          /* leaving home, or back online */
void ui_offline_defer(void);         /* restart the quiet period      */

/* ---------- small helpers (ui_theme.c) ---------- */
lv_obj_t *mk_screen(void);                                  /* black bg     */
lv_obj_t *mk_header(lv_obj_t *parent, const char *title,
                    bool back_btn, lv_event_cb_t on_back);

/* background image for themed screens (NULL when no theme active) */
const lv_image_dsc_t *ui_bg_image(void);
/* color a label that sits directly on the background: theme fg when a
 * theme is active (dim = translucent), else COL_TEXT / COL_TEXT_DIM */
void ui_fg_label(lv_obj_t *label, bool dim);
/* the same rules, for drawn art (no text style to inherit them) */
lv_color_t ui_fg_color(bool dim);
lv_opa_t   ui_fg_opa(bool dim);
/* recolor a mk_header bar's back arrow + title for the active theme */
void ui_fg_header(lv_obj_t *bar);
/* faded surfaces: rows/buttons keep contrast but let the bg peek through */
#define UI_SURFACE_OPA (g_ui.theme_active ? LV_OPA_80 : LV_OPA_COVER)
lv_obj_t *mk_button(lv_obj_t *parent, const char *txt, lv_color_t bg,
                    lv_event_cb_t cb, void *user_data);
lv_obj_t *mk_round_button(lv_obj_t *parent, int diameter, lv_color_t bg,
                          lv_event_cb_t cb, void *user_data);
void      ui_toast(const char *text, lv_color_t color);

#endif /* PIP_UI_INTERNAL_H */
