/* net_http: REST client for the VPS.
 * JSON is parsed with cJSON (bundled with ESP-IDF). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

typedef struct {
    char     id[UI_ID_LEN];
    char     sender_id[UI_ID_LEN];
    char     sender_name[UI_NAME_LEN];
    uint32_t sender_color;
    char     when[20];
    uint32_t ts;               /* UTC epoch secs; 0 = server sent none */
    uint16_t duration_s;
    char     reaction[UI_REACT_KEY_LEN];  /* own reaction; "" = none    */
} http_inbox_item_t;

bool http_get_contacts(ui_contact_t *out, uint8_t cap, uint8_t *count);
bool http_get_themes(ui_theme_info_t *out, uint8_t cap, uint8_t *count);
bool http_download_theme(const char *name, const char *dest_path);
bool http_download_theme_thumb(const char *name, const char *dest_path);
bool http_upload_message(const char *vmsg_path, const char *recipient_id,
                         uint16_t duration_s);
bool http_get_inbox(http_inbox_item_t *out, uint8_t cap, uint8_t *count);
bool http_download_audio(const char *msg_id, const char *dest_path);
bool http_ack_message(const char *msg_id);
bool http_delete_message(const char *msg_id);

/* reactions. The POSTs treat 404 as success: the message (or its reaction
 * row) is gone server-side, so retrying forever would be pointless. */
bool http_post_reaction(const char *msg_id, const char *key); /* ""=clear */
bool http_post_reactions_seen(const char *username);
/* unseen reactions to messages this user sent, newest first, already
 * reduced to one (the latest) per sender */
bool http_get_reactions(ui_reaction_t *out, uint8_t cap, uint8_t *count);

/* geolocate our public IP and set the TZ env accordingly; after this,
 * localtime() renders wall-clock time. Retry until it returns true. */
bool http_set_tz_from_ip(void);
