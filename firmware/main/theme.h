/* theme: background image download + apply.
 * The selected theme name lives in NVS (config); the rendered 368x448
 * RGB565 image is cached in LittleFS and held in PSRAM while active. */
#pragma once
#include <stdbool.h>

/* Restore the persisted background (if any) from the LittleFS cache.
 * Call after storage_init() + ui_init(); takes the LVGL lock itself. */
void theme_init(void);

/* User picked a theme ("" = none). Called from the UI (LVGL task):
 * "none" applies immediately; otherwise the download is queued for the
 * sync task (one TLS session at a time on this board - a parallel
 * handshake next to MQTT's persistent connection starves internal RAM). */
bool theme_select(const char *name, bool black_text);

/* Perform a queued download, if any. Called from the sync task. */
void theme_poll(void);

#include "ui.h"

/* Ensure LittleFS holds a current thumbnail per server theme (files named
 * <name>-<ver>.bin, so a server-side master change re-downloads) and drop
 * stale ones. Called from the sync task (TLS). True if anything changed. */
bool theme_thumbs_sync(const ui_theme_info_t *list, uint8_t count);

/* Thumbnail pixels for the picker: loads <name>-<ver>.bin into a kept
 * PSRAM slot on first use. NULL until the thumb has been downloaded.
 * Called from the LVGL task (file read only, no network). */
const uint16_t *theme_thumb_pixels(const char *name, const char *ver);
