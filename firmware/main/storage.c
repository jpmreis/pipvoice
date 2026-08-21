#include "storage.h"
#include "config.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "storage";

/* identity stamp: which device_id+server owns the data on this partition.
 * A mismatch at boot (re-provisioned board, migrated server) means the
 * previous owner's messages must not carry over. */
#define OWNER_STAMP    STORAGE_ROOT "/.owner"

static void wipe_dir(const char *dir)
{
    /* unlink during readdir is unspecified on littlefs: restart the scan
     * after each delete (only runs on identity change, n is small) */
    for (;;) {
        DIR *d = opendir(dir);
        if (!d) return;
        struct dirent *e;
        char victim[320] = "";
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(victim, sizeof(victim), "%s/%s", dir, e->d_name);
            break;
        }
        closedir(d);
        if (!victim[0]) return;
        unlink(victim);
    }
}

static void identity_check(void)
{
    if (!g_cfg.device_id[0]) return;       /* unprovisioned: nothing owned */

    char want[160], have[160] = "";
    snprintf(want, sizeof(want), "%s\n%s\n", g_cfg.device_id,
             g_cfg.server_base);
    FILE *f = fopen(OWNER_STAMP, "r");
    if (f) {
        size_t n = fread(have, 1, sizeof(have) - 1, f);
        have[n] = 0;
        fclose(f);
    }
    if (!strcmp(want, have)) return;

    if (have[0]) {
        have[strcspn(have, "\n")] = 0;
        ESP_LOGW(TAG, "identity changed (%s -> %s): wiping user data",
                 have, g_cfg.device_id);
        wipe_dir(INBOX_DIR);
        wipe_dir(OUTBOX_DIR);
        wipe_dir(TRASH_DIR);
        wipe_dir(REACT_DIR);
        wipe_dir(RSEEN_DIR);
        wipe_dir(THUMBS_DIR);       /* thumbs come from the old server */
        unlink(CONTACTS_CACHE);
        unlink(REACTIONS_CACHE);
    }
    f = fopen(OWNER_STAMP, "w");
    if (f) { fputs(want, f); fclose(f); }
}

static SemaphoreHandle_t s_evict_mtx;

void storage_init(void)
{
    s_evict_mtx = xSemaphoreCreateMutex();
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_ROOT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));
    mkdir(INBOX_DIR, 0777);
    mkdir(OUTBOX_DIR, 0777);
    mkdir(TRASH_DIR, 0777);
    mkdir(REACT_DIR, 0777);
    mkdir(RSEEN_DIR, 0777);
    mkdir(THUMBS_DIR, 0777);
    identity_check();                     /* config_load() ran before us */

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "littlefs: %u / %u KB used", (unsigned)(used / 1024),
             (unsigned)(total / 1024));
}

size_t storage_free_kb(void)
{
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    return (total - used) / 1024;
}

/* ---------- meta helpers ---------- */
/* lines 6 (sender_id) and 7 (UTC epoch) were added in v0.1.6/7, line 8
 * (own reaction key) in v0.1.20; older files yield "" for them */
#define META_LINES 8
static bool meta_read(const char *path, char lines[META_LINES][40])
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    for (int i = 0; i < META_LINES; i++) {
        if (!fgets(lines[i], 40, f)) lines[i][0] = 0;
        lines[i][strcspn(lines[i], "\n")] = 0;
    }
    fclose(f);
    return true;
}

void storage_inbox_meta_write(const char *msg_id, const char *sender,
                              uint32_t color, const char *when,
                              uint16_t duration_s, bool heard,
                              const char *sender_id, uint32_t ts,
                              const char *reaction)
{
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.meta", msg_id);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%06lx\n%s\n%u\n%d\n%s\n%lu\n%s\n", sender,
            (unsigned long)color, when, duration_s, heard ? 1 : 0,
            sender_id ? sender_id : "", (unsigned long)ts,
            reaction ? reaction : "");
    fclose(f);
}

/* ---------- inbox ---------- */
uint8_t storage_load_inbox(ui_message_t *out, uint8_t cap)
{
    uint8_t n = 0;
    DIR *d = opendir(INBOX_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".meta")) continue;

        char id[UI_ID_LEN] = {0};
        size_t idlen = (size_t)(dot - e->d_name);
        if (idlen >= sizeof(id)) continue;
        memcpy(id, e->d_name, idlen);

        char path[320], lines[META_LINES][40];
        snprintf(path, sizeof(path), INBOX_DIR "/%s", e->d_name);
        if (!meta_read(path, lines)) continue;

        ui_message_t *m = &out[n++];
        strlcpy(m->id, id, sizeof(m->id));
        strlcpy(m->sender_name, lines[0], sizeof(m->sender_name));
        m->sender_color = (uint32_t)strtoul(lines[1], NULL, 16);
        m->duration_s = (uint16_t)atoi(lines[3]);
        m->heard = atoi(lines[4]) != 0;
        strlcpy(m->sender_id, lines[5], sizeof(m->sender_id));
        strlcpy(m->reaction, lines[7], sizeof(m->reaction));

        /* render the clock at load time so TZ changes apply retroactively;
         * fall back to the server-formatted string for pre-epoch metas */
        uint32_t ts = (uint32_t)strtoul(lines[6], NULL, 10);
        if (ts) {
            time_t t = (time_t)ts;
            struct tm lt;
            localtime_r(&t, &lt);
            strftime(m->when, sizeof(m->when), "%a %H:%M", &lt);
        } else {
            strlcpy(m->when, lines[2], sizeof(m->when));
        }
    }
    closedir(d);

    /* newest first: msg ids are server-side sortable (time-ordered) */
    for (uint8_t i = 0; i + 1 < n; i++)
        for (uint8_t j = 0; j + 1 < n - i; j++)
            if (strcmp(out[j].id, out[j + 1].id) < 0) {
                ui_message_t t = out[j]; out[j] = out[j + 1]; out[j + 1] = t;
            }
    return n;
}

bool storage_inbox_has(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.meta", msg_id);
    struct stat st;
    return stat(path, &st) == 0;
}

void storage_inbox_mark_heard(const char *msg_id)
{
    char path[96], lines[META_LINES][40];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.meta", msg_id);
    if (!meta_read(path, lines)) return;
    storage_inbox_meta_write(msg_id, lines[0],
                             (uint32_t)strtoul(lines[1], NULL, 16),
                             lines[2], (uint16_t)atoi(lines[3]), true,
                             lines[5], (uint32_t)strtoul(lines[6], NULL, 10),
                             lines[7]);
}

bool storage_inbox_set_reaction(const char *msg_id, const char *key)
{
    char path[96], lines[META_LINES][40];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.meta", msg_id);
    if (!meta_read(path, lines)) return false;
    if (!strcmp(lines[7], key ? key : "")) return false;
    storage_inbox_meta_write(msg_id, lines[0],
                             (uint32_t)strtoul(lines[1], NULL, 16),
                             lines[2], (uint16_t)atoi(lines[3]),
                             atoi(lines[4]) != 0,
                             lines[5], (uint32_t)strtoul(lines[6], NULL, 10),
                             key);
    return true;
}

void storage_inbox_delete(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.vmsg", msg_id); unlink(path);
    snprintf(path, sizeof(path), INBOX_DIR "/%s.meta", msg_id); unlink(path);
}

void storage_evict_if_needed(void)
{
    /* callers: sync task (per downloaded message) and audio task (before
     * recording) - the mutex keeps them off the shared static list */
    static ui_message_t list[UI_MAX_MESSAGES];
    if (xSemaphoreTake(s_evict_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return;

    /* bounded loop: also reclaims space when something other than one
     * inbox message filled the partition */
    for (int pass = 0; pass < 10; pass++) {
        uint8_t n = storage_load_inbox(list, UI_MAX_MESSAGES);
        if (n == 0 || (n < UI_MAX_MESSAGES && storage_free_kb() > 512))
            break;

        /* delete oldest heard message (list is newest-first); when all
         * are unheard, drop the oldest anyway */
        int victim = n - 1;
        for (int i = n - 1; i >= 0; i--)
            if (list[i].heard) { victim = i; break; }
        ESP_LOGI(TAG, "evicting %s%s", list[victim].id,
                 list[victim].heard ? "" : " (unheard)");
        storage_inbox_delete(list[victim].id);
    }
    xSemaphoreGive(s_evict_mtx);
}

/* ---------- outbox ---------- */
void storage_outbox_meta_write(const char *uuid, const char *recipient,
                               uint16_t duration_s)
{
    char path[96];
    snprintf(path, sizeof(path), OUTBOX_DIR "/%s.meta", uuid);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%u\n", recipient, duration_s);
    fclose(f);
}

bool storage_outbox_next(char *uuid, size_t cap,
                         char *recipient, size_t rcap, uint16_t *duration_s)
{
    DIR *d = opendir(OUTBOX_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) && !found) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".meta")) continue;
        size_t idlen = (size_t)(dot - e->d_name);
        if (idlen >= cap) continue;
        memcpy(uuid, e->d_name, idlen);
        uuid[idlen] = 0;

        char path[320];
        FILE *f;
        snprintf(path, sizeof(path), OUTBOX_DIR "/%s", e->d_name);
        if ((f = fopen(path, "r"))) {
            char line[40] = {0};
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                strlcpy(recipient, line, rcap);
                if (fgets(line, sizeof(line), f)) *duration_s = (uint16_t)atoi(line);
                found = true;
            }
            fclose(f);
        }
    }
    closedir(d);
    return found;
}

void storage_outbox_delete(const char *uuid)
{
    char path[96];
    snprintf(path, sizeof(path), OUTBOX_DIR "/%s.vmsg", uuid); unlink(path);
    snprintf(path, sizeof(path), OUTBOX_DIR "/%s.meta", uuid); unlink(path);
}

/* ---------- trash (pending server-side deletes) ---------- */
void storage_trash_add(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), TRASH_DIR "/%s", msg_id);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

bool storage_trash_has(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), TRASH_DIR "/%s", msg_id);
    struct stat st;
    return stat(path, &st) == 0;
}

bool storage_trash_next(char *msg_id, size_t cap)
{
    DIR *d = opendir(TRASH_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= cap) continue;
        strlcpy(msg_id, e->d_name, cap);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

void storage_trash_remove(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), TRASH_DIR "/%s", msg_id);
    unlink(path);
}

/* ---------- react (pending reaction POSTs; content = key) ---------- */
void storage_react_add(const char *msg_id, const char *key)
{
    char path[96];
    snprintf(path, sizeof(path), REACT_DIR "/%s", msg_id);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(key ? key : "", f);     /* re-reacting overwrites: latest wins */
    fclose(f);
}

bool storage_react_next(char *msg_id, size_t cap, char *key, size_t kcap)
{
    DIR *d = opendir(REACT_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) && !found) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= cap) continue;
        char path[320];
        snprintf(path, sizeof(path), REACT_DIR "/%s", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(key, 1, kcap - 1, f);
        key[n] = 0;
        key[strcspn(key, "\n")] = 0;
        fclose(f);
        strlcpy(msg_id, e->d_name, cap);
        found = true;
    }
    closedir(d);
    return found;
}

void storage_react_remove(const char *msg_id)
{
    char path[96];
    snprintf(path, sizeof(path), REACT_DIR "/%s", msg_id);
    unlink(path);
}

/* ---------- rseen (pending "reactions seen" acks) ---------- */
void storage_rseen_add(const char *username)
{
    char path[96];
    snprintf(path, sizeof(path), RSEEN_DIR "/%s", username);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

bool storage_rseen_next(char *username, size_t cap)
{
    DIR *d = opendir(RSEEN_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= cap) continue;
        strlcpy(username, e->d_name, cap);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

void storage_rseen_remove(const char *username)
{
    char path[96];
    snprintf(path, sizeof(path), RSEEN_DIR "/%s", username);
    unlink(path);
}
