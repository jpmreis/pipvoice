#include "theme.h"
#include "board.h"
#include "config.h"
#include "net_http.h"
#include "storage.h"
#include "sync.h"
#include "ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "theme";

#define THEME_BG_BYTES (368 * 448 * 2)   /* raw RGB565, panel-sized */
#define THEME_TMP_FILE STORAGE_ROOT "/theme_bg.tmp"

static uint16_t *s_px;                   /* PSRAM, allocated once, kept */
static char      s_pending_name[24];
static bool      s_pending_black;
static volatile bool s_busy;

/* read the cached image into PSRAM; true if a full image landed */
static bool load_cached(void)
{
    FILE *f = fopen(THEME_BG_FILE, "rb");
    if (!f) return false;
    if (!s_px) s_px = heap_caps_malloc(THEME_BG_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_px) { fclose(f); return false; }
    size_t r = fread(s_px, 1, THEME_BG_BYTES, f);
    fclose(f);
    return r == THEME_BG_BYTES;
}

void theme_init(void)
{
    if (!g_cfg.theme_name[0]) return;
    if (load_cached()) {
        if (board_lock(500)) {
            ui_set_background(s_px, g_cfg.theme_name, g_cfg.theme_black_text);
            board_unlock();
        }
        ESP_LOGI(TAG, "restored background '%s'", g_cfg.theme_name);
    } else {
        /* cache missing/corrupt (e.g. evicted or interrupted write):
         * quietly fall back to none; the user can re-pick in settings */
        ESP_LOGW(TAG, "background '%s' cache unusable, clearing",
                 g_cfg.theme_name);
        config_save_theme("", false);
    }
}

void theme_poll(void)
{
    if (!s_busy) return;

    ESP_LOGI(TAG, "downloading '%s' (heap: %u internal, %u largest)",
             s_pending_name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(4000));
        ok = http_download_theme(s_pending_name, THEME_TMP_FILE);
        struct stat st;
        ok = ok && stat(THEME_TMP_FILE, &st) == 0
                && st.st_size == THEME_BG_BYTES;
    }
    if (ok) {
        rename(THEME_TMP_FILE, THEME_BG_FILE);
        ok = load_cached();
    }
    if (ok) {
        config_save_theme(s_pending_name, s_pending_black);
        if (board_lock(500)) {
            ui_set_background(s_px, s_pending_name, s_pending_black);
            board_unlock();
        }
        ESP_LOGI(TAG, "background '%s' applied", s_pending_name);
    } else {
        unlink(THEME_TMP_FILE);
        ESP_LOGW(TAG, "background '%s' download failed (heap: %u internal, "
                 "%u largest)", s_pending_name,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (board_lock(500)) {
            ui_flash_error("Background download failed");
            board_unlock();
        }
    }
    s_busy = false;
}

/* ---------------- picker thumbnails ---------------- */
static void thumb_file(char *buf, size_t cap, const char *name,
                       const char *ver)
{
    snprintf(buf, cap, THUMBS_DIR "/%s-%s.bin", name, ver);
}

bool theme_thumbs_sync(const ui_theme_info_t *list, uint8_t count)
{
    bool changed = false;
    char path[96];

    /* fetch missing thumbs (cheap stat when everything is current) */
    for (uint8_t i = 0; i < count; i++) {
        struct stat st;
        thumb_file(path, sizeof(path), list[i].name, list[i].ver);
        if (stat(path, &st) == 0 && st.st_size == UI_THEME_THUMB_BYTES)
            continue;
        if (!http_download_theme_thumb(list[i].name, THUMBS_DIR "/tmp.bin"))
            continue;                    /* retry on a later sync pass */
        if (stat(THUMBS_DIR "/tmp.bin", &st) != 0 ||
            st.st_size != UI_THEME_THUMB_BYTES) {
            unlink(THUMBS_DIR "/tmp.bin");
            continue;
        }
        rename(THUMBS_DIR "/tmp.bin", path);
        changed = true;
        ESP_LOGI(TAG, "thumb %s-%s cached", list[i].name, list[i].ver);
    }

    /* drop thumbs no longer in the list (renamed theme or new version) */
    DIR *d = opendir(THUMBS_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            bool wanted = false;
            for (uint8_t i = 0; i < count && !wanted; i++) {
                thumb_file(path, sizeof(path), list[i].name, list[i].ver);
                wanted = !strcmp(path + sizeof(THUMBS_DIR), e->d_name);
            }
            if (!wanted) {
                snprintf(path, sizeof(path), THUMBS_DIR "/%.60s", e->d_name);
                unlink(path);
                changed = true;
            }
        }
        closedir(d);
    }
    return changed;
}

/* PSRAM cache: one kept slot per theme so the picker never re-reads
 * LittleFS once a thumb is loaded (28.5 KB per slot, PSRAM only) */
typedef struct {
    char      name[24];
    char      ver[UI_THEME_VER_LEN];
    uint16_t *px;
} thumb_slot_t;
static thumb_slot_t s_thumbs[UI_MAX_THEMES];

const uint16_t *theme_thumb_pixels(const char *name, const char *ver)
{
    thumb_slot_t *slot = NULL;
    for (int i = 0; i < UI_MAX_THEMES; i++) {
        if (!strcmp(s_thumbs[i].name, name)) { slot = &s_thumbs[i]; break; }
        if (!slot && !s_thumbs[i].name[0]) slot = &s_thumbs[i];
    }
    if (!slot) return NULL;
    if (slot->px && !strcmp(slot->name, name) && !strcmp(slot->ver, ver))
        return slot->px;

    char path[96];
    thumb_file(path, sizeof(path), name, ver);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (!slot->px)
        slot->px = heap_caps_malloc(UI_THEME_THUMB_BYTES, MALLOC_CAP_SPIRAM);
    if (!slot->px) { fclose(f); return NULL; }
    size_t r = fread(slot->px, 1, UI_THEME_THUMB_BYTES, f);
    fclose(f);
    if (r != UI_THEME_THUMB_BYTES) return NULL;
    strlcpy(slot->name, name, sizeof(slot->name));
    strlcpy(slot->ver, ver, sizeof(slot->ver));
    return slot->px;
}

bool theme_select(const char *name, bool black_text)
{
    if (!name || !name[0]) {
        unlink(THEME_BG_FILE);
        config_save_theme("", false);
        ui_set_background(NULL, "", false);   /* already in the LVGL task */
        return true;
    }
    if (s_busy) return false;                 /* one download at a time */
    strlcpy(s_pending_name, name, sizeof(s_pending_name));
    s_pending_black = black_text;
    s_busy = true;
    sync_kick();                              /* sync task runs theme_poll */
    return true;
}
