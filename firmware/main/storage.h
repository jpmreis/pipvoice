/* storage: LittleFS on the "storage" partition.
 * Layout:
 *   /data/inbox/<msgid>.vmsg          opus audio container
 *   /data/inbox/<msgid>.meta          "sender_name\ncolorhex\nwhen\nduration\nheard"
 *   /data/outbox/<uuid>.vmsg          audio waiting for upload
 *   /data/outbox/<uuid>.meta          "recipient_id\nduration"
 *   /data/trash/<msgid>               server-side delete pending (empty file)
 *   /data/react/<msgid>               reaction POST pending (content = key)
 *   /data/rseen/<username>            reactions-seen POST pending (empty)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

#define STORAGE_ROOT   "/data"
#define INBOX_DIR      STORAGE_ROOT "/inbox"
#define OUTBOX_DIR     STORAGE_ROOT "/outbox"
#define TRASH_DIR      STORAGE_ROOT "/trash"
#define REACT_DIR      STORAGE_ROOT "/react"
#define RSEEN_DIR      STORAGE_ROOT "/rseen"
#define CONTACTS_CACHE  STORAGE_ROOT "/contacts.bin"
#define THEMES_CACHE    STORAGE_ROOT "/themes.bin"    /* picker options    */
#define THEME_BG_FILE   STORAGE_ROOT "/theme_bg.bin"  /* active background */
#define THUMBS_DIR      STORAGE_ROOT "/thumbs"        /* picker thumbnails,
                                       one <name>-<ver>.bin per theme      */
#define REACTIONS_CACHE STORAGE_ROOT "/reactions.bin" /* unseen, as sender */

void storage_init(void);

/* inbox */
uint8_t storage_load_inbox(ui_message_t *out, uint8_t cap);   /* newest first */
bool storage_inbox_has(const char *msg_id);
void storage_inbox_meta_write(const char *msg_id, const char *sender,
                              uint32_t color, const char *when,
                              uint16_t duration_s, bool heard,
                              const char *sender_id, uint32_t ts,
                              const char *reaction);
void storage_inbox_mark_heard(const char *msg_id);
/* set the recipient's own reaction (line 8); true if the value changed */
bool storage_inbox_set_reaction(const char *msg_id, const char *key);
void storage_inbox_delete(const char *msg_id);
void storage_evict_if_needed(void);            /* oldest heard first */

/* outbox */
void storage_outbox_meta_write(const char *uuid, const char *recipient,
                               uint16_t duration_s);
bool storage_outbox_next(char *uuid, size_t cap,
                         char *recipient, size_t rcap,
                         uint16_t *duration_s);
void storage_outbox_delete(const char *uuid);

/* trash: message ids whose server-side delete is still pending. The UI
 * queues them (fast, local); the sync task performs the HTTP delete. */
void storage_trash_add(const char *msg_id);
bool storage_trash_has(const char *msg_id);
bool storage_trash_next(char *msg_id, size_t cap);
void storage_trash_remove(const char *msg_id);

/* react: reactions whose server POST is still pending (same idiom as
 * trash; the file content is the reaction key, "" = clear) */
void storage_react_add(const char *msg_id, const char *key);
bool storage_react_next(char *msg_id, size_t cap, char *key, size_t kcap);
void storage_react_remove(const char *msg_id);

/* rseen: per-contact "reactions seen" acks pending upload (empty files) */
void storage_rseen_add(const char *username);
bool storage_rseen_next(char *username, size_t cap);
void storage_rseen_remove(const char *username);

size_t storage_free_kb(void);
