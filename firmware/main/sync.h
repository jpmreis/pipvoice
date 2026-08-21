/* sync: the store-and-forward brain.
 * One task; wakes on WiFi-up, MQTT notify, new outbox item, or timer. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

typedef struct {
    void (*inbox_changed)(void);                 /* reload UI from storage  */
    void (*new_message)(const char *sender);     /* toast + chime           */
    void (*contacts_changed)(void);
    void (*themes_changed)(void);                /* background theme list   */
    void (*reactions_changed)(void);             /* sender-side badges      */
} sync_events_t;

void sync_init(const sync_events_t *ev);
void sync_kick(void);            /* something to do (notify/outbox/wifi-up) */
void sync_contacts_kick(void);   /* contact list changed server-side */

/* last fetched contact list (valid until the next sync pass) */
const ui_contact_t *sync_contacts(uint8_t *count);

/* last fetched background theme list (cached across boots) */
const ui_theme_info_t *sync_themes(uint8_t *count);

/* mark a contact as just-used (send or receive): resorts the list
 * newest-first, persists, and fires contacts_changed */
void sync_touch_contact(const char *contact_id);

/* unseen reactions to this user's sent messages, latest per contact
 * (cached across boots like contacts) */
const ui_reaction_t *sync_reactions(uint8_t *count);
/* merge a live MQTT reaction event; persists and fires reactions_changed */
void sync_reaction_add(const char *from, const char *from_name,
                       const char *key);
/* drop a contact's badge (persists; no event - the UI already updated) */
void sync_reactions_seen(const char *contact_id);
