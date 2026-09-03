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
    /* voice control (all fired from the audio task, may be NULL):
     * detections while listening (VOICE_HIT_* in voice_infer.h),
     * completion of an audio_play_prompt() playback, and the answer-
     * window timeout relayed via audio_voice_timeout() */
    void (*voice_hits)(uint32_t mask);
    void (*prompt_done)(void);
    void (*voice_timeout)(void);
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

/* two-pip brand sound; the PWA synthesizes the same motif (app.js).
 * CHIME_PROMPT is the voice flow's single-note cue (box-only). */
typedef enum { CHIME_RECEIVED, CHIME_SENT, CHIME_PROMPT } chime_t;
void audio_play_chime(chime_t which);

/* ---- voice control (wake-word) ----
 * While on, the audio task holds the mic open between commands and
 * streams it through voice_infer; commands (play/record/chime) preempt
 * within 10 ms and listening resumes when they finish. */
void audio_voice_listen(bool on);
/* Play a spoken prompt (or an inbox message inside the voice flow):
 * completion fires events.prompt_done - even for an unopenable path -
 * and no play_progress ticks are emitted. */
void audio_play_prompt(const char *path);
/* Start a recording that auto-stops on trailing silence (voice flow);
 * reported through record_done like any other recording. */
void audio_record_start_vad(void);
/* Relay a voice answer-window timeout onto the audio task, which fires
 * events.voice_timeout there. The esp_timer task's stack is far too
 * small for the prompt/UI work a timeout kicks off (a session under the
 * sleep screen once overflowed it mid-leave_ambient). */
void audio_voice_timeout(void);
