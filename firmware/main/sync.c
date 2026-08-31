#include "sync.h"
#include "config.h"
#include "net_http.h"
#include "net_wifi.h"
#include "storage.h"
#include "theme.h"
#include "ui.h"
#include "voice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "sync";

#define POLL_INTERVAL_MS       (15 * 60 * 1000)
#define CONTACTS_INTERVAL_MS   (24 * 60 * 60 * 1000)

static sync_events_t     s_ev;
static SemaphoreHandle_t s_wake;
static volatile bool     s_contacts_force;   /* server says the list changed */
static volatile bool     s_inbox_first;      /* new mail: list it before the
                                                rest of the ladder runs   */
static ui_contact_t      s_contacts[UI_MAX_CONTACTS];
static uint8_t           s_contact_count;
static ui_theme_info_t   s_themes[UI_MAX_THEMES];
static uint8_t           s_theme_count;

static void contacts_sort(void)
{
    /* newest-first; ties keep server order (alphabetical). Insertion sort:
     * n <= UI_MAX_CONTACTS and the list is nearly sorted already. */
    for (uint8_t i = 1; i < s_contact_count; i++) {
        ui_contact_t c = s_contacts[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && s_contacts[j].last_used < c.last_used) {
            s_contacts[j + 1] = s_contacts[j];
            j--;
        }
        s_contacts[j + 1] = c;
    }
}

static void contacts_cache_save(void)
{
    FILE *f = fopen(CONTACTS_CACHE, "wb");
    if (!f) return;
    fwrite(&s_contact_count, 1, 1, f);
    fwrite(s_contacts, sizeof(ui_contact_t), s_contact_count, f);
    fclose(f);
}

static void contacts_cache_load(void)
{
    FILE *f = fopen(CONTACTS_CACHE, "rb");
    if (!f) return;
    uint8_t n = 0;
    if (fread(&n, 1, 1, f) == 1 && n <= UI_MAX_CONTACTS &&
        fread(s_contacts, sizeof(ui_contact_t), n, f) == n) {
        s_contact_count = n;
        ESP_LOGI(TAG, "loaded %u cached contacts", n);
        if (s_ev.contacts_changed) s_ev.contacts_changed();
    }
    fclose(f);
}

static void refresh_contacts(void)
{
    uint8_t n = 0;
    static ui_contact_t tmp[UI_MAX_CONTACTS];
    if (http_get_contacts(tmp, UI_MAX_CONTACTS, &n) && n) {
        /* keep last_used across refreshes: the server list has no memory */
        for (uint8_t i = 0; i < n; i++)
            for (uint8_t j = 0; j < s_contact_count; j++)
                if (!strcmp(tmp[i].id, s_contacts[j].id)) {
                    tmp[i].last_used = s_contacts[j].last_used;
                    break;
                }
        memcpy(s_contacts, tmp, sizeof(tmp));
        s_contact_count = n;
        contacts_sort();
        contacts_cache_save();
        if (s_ev.contacts_changed) s_ev.contacts_changed();
        ESP_LOGI(TAG, "contacts: %u", n);
    }
}

/* ---------------- per-device config + voice prompts ----------------
 * Fetched alongside the contacts refresh (same 24 h gate, same forced
 * kick via MQTT {"event":"voice"} -> sync_contacts_kick). The flag is
 * persisted to NVS so an offline reboot keeps listening; the prompt
 * manifest lives only here - the files themselves persist on flash and
 * are reconciled against the manifest each pass. */
#define VOICE_MAX_PROMPTS 20      /* 4 canned + one per contact (12) */
static http_prompt_t s_prompts[VOICE_MAX_PROMPTS];
static uint8_t       s_prompt_count;
static bool          s_have_manifest;

static void refresh_device_cfg(void)
{
    bool ven = false;
    static http_prompt_t tmp[VOICE_MAX_PROMPTS];
    uint8_t n = 0;
    if (!http_get_device_config(&ven, tmp, VOICE_MAX_PROMPTS, &n)) return;
    memcpy(s_prompts, tmp, n * sizeof(http_prompt_t));
    s_prompt_count = n;
    s_have_manifest = true;
    if (ven != g_cfg.voice_enabled) {
        config_save_voice(ven);
        voice_set_enabled(ven);
    }
}

static void prompt_file(char *buf, size_t cap, const http_prompt_t *p)
{
    snprintf(buf, cap, PROMPTS_DIR "/%.47s-%.11s.vmsg", p->key, p->ver);
}

/* mirror the manifest: fetch missing clips, drop stale ones (renamed
 * contact, new TTS voice, or voice control switched off entirely).
 * Same shape as theme_thumbs_sync; runs in this task (one-TLS rule). */
static void prompts_sync(void)
{
    if (!s_have_manifest) return;
    char path[128];

    for (uint8_t i = 0; i < s_prompt_count; i++) {
        struct stat st;
        prompt_file(path, sizeof(path), &s_prompts[i]);
        if (stat(path, &st) == 0 && st.st_size > 0) continue;
        if (!http_download_prompt(s_prompts[i].key, s_prompts[i].ver,
                                  PROMPTS_DIR "/tmp.bin"))
            continue;                    /* retry on a later sync pass */
        if (stat(PROMPTS_DIR "/tmp.bin", &st) != 0 || st.st_size == 0) {
            unlink(PROMPTS_DIR "/tmp.bin");
            continue;
        }
        rename(PROMPTS_DIR "/tmp.bin", path);
        ESP_LOGI(TAG, "prompt %s-%s cached", s_prompts[i].key,
                 s_prompts[i].ver);
    }

    DIR *d = opendir(PROMPTS_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            bool wanted = false;
            for (uint8_t i = 0; i < s_prompt_count && !wanted; i++) {
                prompt_file(path, sizeof(path), &s_prompts[i]);
                wanted = !strcmp(path + sizeof(PROMPTS_DIR), e->d_name);
            }
            if (!wanted) {
                snprintf(path, sizeof(path), PROMPTS_DIR "/%.60s",
                         e->d_name);
                unlink(path);
            }
        }
        closedir(d);
    }
}

/* ---------------- background themes ---------------- */
static void themes_cache_save(void)
{
    FILE *f = fopen(THEMES_CACHE, "wb");
    if (!f) return;
    fwrite(&s_theme_count, 1, 1, f);
    fwrite(s_themes, sizeof(ui_theme_info_t), s_theme_count, f);
    fclose(f);
}

static void themes_cache_load(void)
{
    FILE *f = fopen(THEMES_CACHE, "rb");
    if (!f) return;
    uint8_t n = 0;
    if (fread(&n, 1, 1, f) == 1 && n <= UI_MAX_THEMES &&
        fread(s_themes, sizeof(ui_theme_info_t), n, f) == n) {
        s_theme_count = n;
        if (s_ev.themes_changed) s_ev.themes_changed();
    }
    fclose(f);
}

static void refresh_themes(void)
{
    uint8_t n = 0;
    static ui_theme_info_t tmp[UI_MAX_THEMES];
    if (http_get_themes(tmp, UI_MAX_THEMES, &n) && n) {
        memcpy(s_themes, tmp, sizeof(tmp));
        s_theme_count = n;
        themes_cache_save();
        if (s_ev.themes_changed) s_ev.themes_changed();
        ESP_LOGI(TAG, "themes: %u", n);
    }
}

/* ---------------- sender-side reaction badges ---------------- */
static ui_reaction_t s_reactions[UI_MAX_CONTACTS];
static uint8_t       s_reaction_count;

static void reactions_cache_save(void)
{
    FILE *f = fopen(REACTIONS_CACHE, "wb");
    if (!f) return;
    fwrite(&s_reaction_count, 1, 1, f);
    fwrite(s_reactions, sizeof(ui_reaction_t), s_reaction_count, f);
    fclose(f);
}

static void reactions_cache_load(void)
{
    FILE *f = fopen(REACTIONS_CACHE, "rb");
    if (!f) return;
    uint8_t n = 0;
    if (fread(&n, 1, 1, f) == 1 && n <= UI_MAX_CONTACTS &&
        fread(s_reactions, sizeof(ui_reaction_t), n, f) == n) {
        s_reaction_count = n;
        if (s_ev.reactions_changed) s_ev.reactions_changed();
    }
    fclose(f);
}

const ui_reaction_t *sync_reactions(uint8_t *count)
{
    *count = s_reaction_count;
    return s_reactions;
}

void sync_reaction_add(const char *from, const char *from_name,
                       const char *key)
{
    for (uint8_t i = 0; i < s_reaction_count; i++)
        if (!strcmp(s_reactions[i].from, from)) {
            strlcpy(s_reactions[i].from_name, from_name, UI_NAME_LEN);
            strlcpy(s_reactions[i].key, key, UI_REACT_KEY_LEN);
            goto done;
        }
    if (s_reaction_count >= UI_MAX_CONTACTS) return;  /* can't happen: one
                                                         badge per contact */
    strlcpy(s_reactions[s_reaction_count].from, from, UI_ID_LEN);
    strlcpy(s_reactions[s_reaction_count].from_name, from_name, UI_NAME_LEN);
    strlcpy(s_reactions[s_reaction_count].key, key, UI_REACT_KEY_LEN);
    s_reaction_count++;
done:
    reactions_cache_save();
    if (s_ev.reactions_changed) s_ev.reactions_changed();
}

void sync_reactions_seen(const char *contact_id)
{
    for (uint8_t i = 0; i < s_reaction_count; i++) {
        if (strcmp(s_reactions[i].from, contact_id)) continue;
        for (uint8_t j = i; j + 1 < s_reaction_count; j++)
            s_reactions[j] = s_reactions[j + 1];
        s_reaction_count--;
        reactions_cache_save();
        return;
    }
}

static void fetch_reactions(void)
{
    ui_reaction_t tmp[UI_MAX_CONTACTS];   /* ~0.9 KB, fits the 8 KB stack */
    memset(tmp, 0, sizeof(tmp));          /* strlcpy leaves tails: zero them
                                             or the memcmp below misfires */
    uint8_t n = 0;
    if (!http_get_reactions(tmp, UI_MAX_CONTACTS, &n)) return;
    if (n == s_reaction_count &&
        !memcmp(tmp, s_reactions, n * sizeof(ui_reaction_t))) return;
    memcpy(s_reactions, tmp, n * sizeof(ui_reaction_t));
    s_reaction_count = n;
    reactions_cache_save();
    /* silent badge refresh: the live chime+toast came over MQTT; this
     * path is offline catch-up and must not replay notifications */
    if (s_ev.reactions_changed) s_ev.reactions_changed();
}

static void drain_reactions(void)
{
    char id[UI_ID_LEN], key[UI_REACT_KEY_LEN];
    while (net_wifi_is_connected() &&
           storage_react_next(id, sizeof(id), key, sizeof(key))) {
        if (http_post_reaction(id, key)) {
            storage_react_remove(id);
        } else {
            break;   /* network trouble: retry after next wake/backoff */
        }
    }
}

static void drain_rseen(void)
{
    char user[UI_ID_LEN];
    while (net_wifi_is_connected() && storage_rseen_next(user, sizeof(user))) {
        if (http_post_reactions_seen(user)) {
            storage_rseen_remove(user);
        } else {
            break;   /* network trouble: retry after next wake/backoff */
        }
    }
}

static void drain_outbox(void)
{
    char uuid[UI_ID_LEN], recipient[UI_ID_LEN];
    uint16_t dur;
    while (net_wifi_is_connected() &&
           storage_outbox_next(uuid, sizeof(uuid), recipient,
                               sizeof(recipient), &dur)) {
        char path[96];
        snprintf(path, sizeof(path), OUTBOX_DIR "/%s.vmsg", uuid);
        struct stat st;
        if (stat(path, &st) != 0) {
            /* orphaned .meta (audio never landed): drop it or the outbox
             * would retry this same item forever and never drain */
            ESP_LOGW(TAG, "dropping orphaned outbox item %s", uuid);
            storage_outbox_delete(uuid);
            continue;
        }
        ESP_LOGI(TAG, "uploading %s -> %s (%us)", uuid, recipient, dur);
        if (http_upload_message(path, recipient, dur)) {
            storage_outbox_delete(uuid);
        } else {
            break;   /* network trouble: retry after next wake/backoff */
        }
    }
}

static void drain_trash(void)
{
    char id[UI_ID_LEN];
    while (net_wifi_is_connected() && storage_trash_next(id, sizeof(id))) {
        if (http_delete_message(id)) {
            storage_trash_remove(id);
        } else {
            break;   /* network trouble: retry after next wake/backoff */
        }
    }
}

static bool touch_quiet(const char *contact_id)
{
    for (uint8_t i = 0; i < s_contact_count; i++)
        if (!strcmp(s_contacts[i].id, contact_id)) {
            s_contacts[i].last_used = (uint32_t)time(NULL);
            return true;
        }
    return false;
}

void sync_touch_contact(const char *contact_id)
{
    if (!touch_quiet(contact_id)) return;
    contacts_sort();
    contacts_cache_save();
    if (s_ev.contacts_changed) s_ev.contacts_changed();
}

/* Download one message and announce it. Shared by the inbox poll and the
 * MQTT fast path below, so both write the same .meta and chime the same
 * way. Returns true when something landed. */
static bool deliver_one(const http_inbox_item_t *m, bool *touched)
{
    if (storage_inbox_has(m->id)) return false;
    if (storage_trash_has(m->id)) return false;   /* deleted locally,
                                     server delete still pending */
    storage_evict_if_needed();
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%.*s.vmsg", UI_ID_LEN - 1, m->id);
    if (!http_download_audio(m->id, path)) return false;

    storage_inbox_meta_write(m->id, m->sender_name, m->sender_color,
                             m->when, m->duration_s, false,
                             m->sender_id, m->ts, m->reaction);
    *touched |= touch_quiet(m->sender_id);
    /* Chime here, not after the ack. The message is on flash and its meta
     * is written - it has arrived, as far as anyone in the house is
     * concerned. The ack is bookkeeping for the server, and it used to run
     * first: a whole TLS handshake of silence after the audio had already
     * landed, which on weak wifi is exactly the delay this release is
     * about. It still runs, just behind the news. */
    if (s_ev.new_message) s_ev.new_message(m->sender_name);
    ESP_LOGI(TAG, "downloaded %s from %s", m->id, m->sender_name);
    http_ack_message(m->id);
    return true;
}

/* ---------------- fast path: the notify said what arrived ----------------
 * A new-message MQTT notify carries the message's metadata, so the box can
 * skip GET /inbox and fetch the audio straight away - one TLS handshake off
 * the gap between the sender letting go of the button and this box
 * chiming. Hints are advisory in every direction: a full queue drops them,
 * a failed download leaves them, quiet hours discard them. The poll behind
 * this still lists the inbox and still reconciles, so nothing is ever lost
 * by ignoring a hint. */
#define HINT_MAX 4
static http_inbox_item_t  s_hints[HINT_MAX];
static volatile uint8_t   s_hint_n;   /* volatile for drain_hints' unlocked
                                         "anything waiting?" peek */
static SemaphoreHandle_t  s_hint_lock;     /* MQTT task fills, sync drains */

void sync_message_hint(const http_inbox_item_t *m)
{
    if (!s_hint_lock) return;
    xSemaphoreTake(s_hint_lock, portMAX_DELAY);
    if (s_hint_n < HINT_MAX) s_hints[s_hint_n++] = *m;
    xSemaphoreGive(s_hint_lock);
    xSemaphoreGive(s_wake);
}

static bool hint_take(http_inbox_item_t *out)
{
    bool have;
    xSemaphoreTake(s_hint_lock, portMAX_DELAY);
    have = s_hint_n > 0;
    if (have) {
        *out = s_hints[0];
        for (uint8_t i = 1; i < s_hint_n; i++) s_hints[i - 1] = s_hints[i];
        s_hint_n--;
    }
    xSemaphoreGive(s_hint_lock);
    return have;
}

static void hints_clear(void)
{
    xSemaphoreTake(s_hint_lock, portMAX_DELAY);
    s_hint_n = 0;
    xSemaphoreGive(s_hint_lock);
}

static void drain_hints(void)
{
    if (!s_hint_n) return;
    if (sync_quiet_hold()) {   /* the morning's fetch_inbox collects them */
        hints_clear();
        return;
    }
    bool touched = false, changed = false;
    http_inbox_item_t m;
    while (hint_take(&m))
        if (deliver_one(&m, &touched)) changed = true;
    if (touched) {
        contacts_sort();
        contacts_cache_save();
        if (s_ev.contacts_changed) s_ev.contacts_changed();
    }
    if (changed && s_ev.inbox_changed) s_ev.inbox_changed();
}

static void fetch_inbox(void)
{
    static http_inbox_item_t items[UI_MAX_MESSAGES];
    uint8_t n = 0;
    if (!http_get_inbox(items, UI_MAX_MESSAGES, &n)) return;

    bool touched = false;
    bool changed = false;
    for (uint8_t i = 0; i < n; i++) {
        if (storage_inbox_has(items[i].id)) {
            /* already downloaded, but the reaction may have changed
             * since (this user reacting from their PWA): sync line 8 */
            if (storage_inbox_set_reaction(items[i].id, items[i].reaction))
                changed = true;
            continue;
        }
        if (deliver_one(&items[i], &touched)) changed = true;
    }
    if (touched) {          /* one resort/save/notify for the whole batch */
        contacts_sort();
        contacts_cache_save();
        if (s_ev.contacts_changed) s_ev.contacts_changed();
    }

    /* reconcile: the server response is the authoritative inbox, so any
     * local message absent from it was deleted server-side. If the fetch
     * filled the cap it may be truncated; ids sort chronologically, so
     * only local entries within the returned window are decidable. */
    ui_message_t *local =
        heap_caps_malloc(UI_MAX_MESSAGES * sizeof(*local), MALLOC_CAP_SPIRAM);
    if (local) {
        uint8_t ln = storage_load_inbox(local, UI_MAX_MESSAGES);
        for (uint8_t i = 0; i < ln; i++) {
            if (n == UI_MAX_MESSAGES &&
                strcmp(local[i].id, items[n - 1].id) < 0)
                continue;             /* older than the fetch window */
            bool present = false;
            for (uint8_t j = 0; j < n && !present; j++)
                present = !strcmp(local[i].id, items[j].id);
            if (!present) {
                ESP_LOGI(TAG, "reconcile: %s gone server-side, dropping",
                         local[i].id);
                storage_inbox_delete(local[i].id);
                changed = true;
            }
        }
        free(local);
    }

    if (changed && s_ev.inbox_changed) s_ev.inbox_changed();
}

/* anything older than 2023-11 means SNTP hasn't answered yet */
#define CLOCK_SANE_EPOCH 1700000000
/* how long a fresh boot waits for the clock before delivering anyway */
#define CLOCK_WAIT_US    (2 * 60 * 1000000LL)

static bool clock_set(void) { return time(NULL) >= CLOCK_SANE_EPOCH; }

bool sync_dnd_active(void)
{
    if (!clock_set()) return false;        /* no clock: no verdict */
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_hour >= DND_START_H || lt.tm_hour < DND_END_H;
}

bool sync_quiet_hold(void)
{
    /* A box that just powered on has no clock until SNTP answers, and
     * "no clock" is not "daytime": a box plugged in at night delivered
     * its held messages, chime and all, in the seconds before the time
     * landed. Hold while we can't tell - but not forever, or a network
     * that blocks NTP would swallow the inbox for good. */
    if (!clock_set()) return esp_timer_get_time() < CLOCK_WAIT_US;
    return sync_dnd_active();
}

static void sync_task(void *arg)
{
    (void)arg;
    TickType_t last_contacts = 0;
    bool tz_set = false;
    for (;;) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(POLL_INTERVAL_MS));
        if (!net_wifi_is_connected()) continue;

        /* set TZ before formatting any timestamps; retry until it works.
         * Once it lands, re-render the inbox so on-screen clocks jump
         * from UTC to local without waiting for a new message. */
        if (!tz_set) {
            tz_set = http_set_tz_from_ip();
            if (tz_set && s_ev.inbox_changed) s_ev.inbox_changed();
        }

        /* Incoming mail jumps the queue. Everything below holds the one
         * TLS slot for as long as it takes - a contacts refresh, an outbox
         * upload, a theme download - and a message the family is waiting
         * on used to queue behind all of it. */
        drain_hints();
        bool did_inbox = false;
        if (s_inbox_first) {      /* notify without usable metadata */
            s_inbox_first = false;
            if (!sync_quiet_hold()) {
                fetch_inbox();
                did_inbox = true;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (s_contacts_force || last_contacts == 0 ||
            (now - last_contacts) > pdMS_TO_TICKS(CONTACTS_INTERVAL_MS)) {
            s_contacts_force = false;
            refresh_contacts();
            refresh_themes();
            refresh_device_cfg();
            last_contacts = now;
        }
        drain_outbox();
        drain_trash();
        drain_reactions();
        drain_rseen();       /* before fetch_reactions, so a just-seen
                                badge can't be resurrected by the fetch */
        /* quiet hours: leave new mail on the server until morning. Still
         * runs after a fast-path delivery above: this listing is also what
         * reconciles server-side deletions. */
        if (!did_inbox && !sync_quiet_hold()) fetch_inbox();
        fetch_reactions();
        theme_poll();   /* queued background download; serialized here so
                           only one TLS session runs at a time */
        /* picker thumbnails: cheap stats when current, downloads (in this
         * task - same one-TLS rule) when the server list/version moved */
        if (theme_thumbs_sync(s_themes, s_theme_count) &&
            s_ev.themes_changed)
            s_ev.themes_changed();
        prompts_sync();   /* voice clips: cheap stats when current */
    }
}

void sync_init(const sync_events_t *ev)
{
    s_ev = *ev;
    s_wake = xSemaphoreCreateBinary();
    s_hint_lock = xSemaphoreCreateMutex();
    contacts_cache_load();                 /* device is usable offline */
    themes_cache_load();
    reactions_cache_load();                /* badges survive reboots   */
    xTaskCreatePinnedToCore(sync_task, "sync", 8192, NULL, 4, NULL, 0);
}

void sync_kick(void) { xSemaphoreGive(s_wake); }

void sync_inbox_kick(void)
{
    s_inbox_first = true;                /* jump the ladder once */
    xSemaphoreGive(s_wake);
}

void sync_contacts_kick(void)
{
    s_contacts_force = true;             /* bypass the 24 h gate once */
    xSemaphoreGive(s_wake);
}

const ui_contact_t *sync_contacts(uint8_t *count)
{
    *count = s_contact_count;
    return s_contacts;
}

const ui_theme_info_t *sync_themes(uint8_t *count)
{
    *count = s_theme_count;
    return s_themes;
}
