/* voice: hands-free accessibility flow ("Hey Pip").
 *
 * Wake word -> offer to play unheard messages -> cycle contacts by
 * spoken name -> "yes" -> record (VAD auto-stop) -> "yes" to send.
 * Dormant unless the server-set per-device flag (admin "Voice control")
 * is on; the flag is fetched by sync and persisted in NVS (voice_en).
 *
 * The state machine is driven by audio-task events (wake/confirm hits,
 * prompt/record completion) plus one esp_timer for answer windows; a
 * mutex serializes the two contexts. Spoken prompts are server-rendered
 * .vmsg files under /data/prompts (synced like theme assets); a missing
 * prompt degrades to a chime + on-screen text, never a dead end. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* app_main-provided actions (the same code paths the touch UI uses) */
typedef struct {
    void (*record_start)(const char *contact_id);  /* VAD auto-stop mode */
    bool (*record_send)(void);
    void (*record_cancel)(void);
    void (*mark_heard)(const char *msg_id);
} voice_app_t;

void voice_init(const voice_app_t *app);

/* server flag (sync task) / boot restore. Enabling starts the always-on
 * listener; disabling stops it and aborts any session. */
void voice_set_enabled(bool on);
bool voice_enabled(void);

/* true while a voice session (wake..send) is in flight: app_main routes
 * record_done to voice_on_record_done instead of the record screen */
bool voice_session_active(void);

/* abort the in-flight session (the X on the voice screen); leaves the
 * screen to the caller, safe to call from the LVGL task */
void voice_cancel(void);

/* events (audio-task context) */
void voice_on_hits(uint32_t mask);          /* VOICE_HIT_* (voice_infer.h) */
void voice_on_prompt_done(void);
void voice_on_record_done(uint16_t duration_s);
void voice_on_timeout(void);                /* audio_events_t.voice_timeout */
