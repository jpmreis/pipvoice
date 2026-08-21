/* audio: ES8311 record/playback engine.
 * Owns one audio task; commands arrive via a queue. Progress is reported
 * through callbacks that app_main forwards to the UI under the LVGL lock. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*record_tick)(uint16_t elapsed_s, uint16_t max_s);
    void (*record_done)(uint16_t duration_s);     /* stopped or limit hit  */
    void (*play_progress)(uint16_t pos_s, uint16_t total_s);
    void (*play_done)(void);
} audio_events_t;

void audio_init(const audio_events_t *ev);
void audio_set_volume(uint8_t pct);
bool audio_codec_ok(void);   /* probe result for the PIP-HW line */

/* recording writes to OUTBOX_DIR/rec_tmp.vmsg */
void audio_record_start(void);
void audio_record_stop(void);
/* Discard the recording: the audio task deletes rec_tmp.vmsg after closing
 * it (never delete the file from another task - it may still be open). */
void audio_record_cancel(void);
/* Blocks until the recorder has finalized the file (or timeout).
 * Returns true when the recording is complete and safe to move/send. */
bool audio_wait_record_done(uint32_t timeout_ms);

void audio_play_file(const char *path);
void audio_stop(void);

/* two-pip brand sound; the PWA synthesizes the same motif (app.js) */
typedef enum { CHIME_RECEIVED, CHIME_SENT } chime_t;
void audio_play_chime(chime_t which);
