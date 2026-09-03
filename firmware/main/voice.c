#include "voice.h"
#include "voice_infer.h"
#include "audio.h"
#include "board.h"
#include "power.h"
#include "storage.h"
#include "sync.h"
#include "ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "voice";

#define ANSWER_WINDOW_MS   3500   /* how long a question waits for "yes"  */
#define CYCLE_LOOPS        2      /* full trips around the contact list   */
#define SPEECH_HOPS_YES    25     /* VAD fallback: 250 ms of voiced audio
                                     inside the window counts as "yes"    */

typedef enum {
    V_IDLE,
    V_ASK_PLAY, V_ASK_PLAY_WAIT,      /* "you have new messages - hear them?" */
    V_PLAYING,                        /* unheard messages, oldest first       */
    V_ASK_SEND, V_ASK_SEND_WAIT,      /* "send a message to <name>?"          */
    V_RECORDING,
    V_ASK_CONFIRM, V_ASK_CONFIRM_WAIT /* "send it?"                           */
} v_state_t;

static voice_app_t        s_app;
static SemaphoreHandle_t  s_mtx;
static esp_timer_handle_t s_timer;
static volatile bool      s_enabled;
static v_state_t          s_state;

static ui_contact_t s_contacts[UI_MAX_CONTACTS];
static uint8_t      s_contact_count;
static uint8_t      s_cursor;        /* contact being offered            */
static uint8_t      s_offered;       /* offers made this session         */
static char       (*s_unheard)[UI_ID_LEN];   /* PSRAM, oldest first      */
static uint8_t      s_unheard_count, s_play_idx;
static uint8_t      s_retries;       /* re-ask budget for current question */
static uint16_t     s_speech_hops;   /* voiced hops inside this window   */

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)
/* UI touches from these contexts (audio task / esp_timer) need the LVGL
 * lock; a timed-out update is dropped, same policy as app_main */
#define VUI(stmt) do { if (board_lock(200)) { stmt; board_unlock(); } } while (0)

/* ---------------- prompts ---------------- */
/* server-rendered clips: /data/prompts/<key>-<8hexver>.vmsg (sync.c
 * downloads them; any version of the key will do here) */
static bool prompt_path(const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    DIR *d = opendir(PROMPTS_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, key, klen) || e->d_name[klen] != '-')
            continue;
        if (strlen(e->d_name) != klen + 1 + 8 + 5) continue;  /* -<ver>.vmsg */
        snprintf(out, cap, PROMPTS_DIR "/%.72s", e->d_name);
        found = true;
    }
    closedir(d);
    return found;
}

/* Speak a prompt. A missing clip degrades to the cue chime + an
 * unopenable path, whose instant prompt_done opens the answer window
 * right away - the screen carries the question, the flow never stalls. */
static void say(const char *key)
{
    power_user_activity();     /* each question resets the 15 s dim timer:
                                  a full contact cycle outlives it */
    char path[96];
    if (!prompt_path(key, path, sizeof(path))) {
        ESP_LOGW(TAG, "prompt '%s' not cached, chiming instead", key);
        audio_play_chime(CHIME_PROMPT);
        snprintf(path, sizeof(path), PROMPTS_DIR "/none");
    }
    audio_play_prompt(path);
}

static void window_open(void)
{
    s_speech_hops = 0;
    voice_infer_arm_confirm(true);
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)ANSWER_WINDOW_MS * 1000);
}

static void window_close(void)
{
    voice_infer_arm_confirm(false);
    esp_timer_stop(s_timer);
}

static void session_end(void)
{
    window_close();
    s_state = V_IDLE;
    power_hold(false);
    VUI(ui_voice_close());
}

/* ---------------- flow steps (call with s_mtx held) ---------------- */
static void step_ask_send(void)
{
    const ui_contact_t *c = &s_contacts[s_cursor];
    char key[64];
    snprintf(key, sizeof(key), "ask_send-%s", c->id);
    s_state = V_ASK_SEND;
    VUI(ui_voice_show("Send a message to", c->name));
    say(key);
}

static void step_next_contact(void)
{
    if (++s_offered >= s_contact_count * CYCLE_LOOPS) {
        ESP_LOGI(TAG, "no takers, going back to sleep");
        session_end();
        return;
    }
    s_cursor = (uint8_t)((s_cursor + 1) % s_contact_count);
    step_ask_send();
}

static void step_play_next(void)
{
    if (s_play_idx >= s_unheard_count) {
        s_cursor = 0;                      /* messages done: offer a reply */
        s_offered = 0;
        step_ask_send();
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), INBOX_DIR "/%s.vmsg",
             s_unheard[s_play_idx]);
    power_user_activity();
    s_state = V_PLAYING;
    VUI(ui_voice_show("Playing message", ""));
    audio_play_prompt(path);
}

static void step_record(void)
{
    const ui_contact_t *c = &s_contacts[s_cursor];
    s_state = V_RECORDING;
    VUI(ui_voice_show("Recording for", c->name));
    audio_play_chime(CHIME_PROMPT);        /* audible "go" - queued ahead
                                              of the record command       */
    s_app.record_start(c->id);
}

static void step_wake(void)
{
    /* snapshot the app-layer contact list (MRU order, same source the
     * home grid renders from); no contacts = nothing to drive */
    uint8_t n = 0;
    const ui_contact_t *list = sync_contacts(&n);
    if (n == 0) return;
    memcpy(s_contacts, list, n * sizeof(ui_contact_t));
    s_contact_count = n;
    s_cursor = 0;
    s_offered = 0;
    s_retries = 0;

    /* unheard messages, oldest first (storage lists newest first) */
    s_unheard_count = 0;
    s_play_idx = 0;
    ui_message_t *inbox = s_unheard ? heap_caps_malloc(
        UI_MAX_MESSAGES * sizeof(*inbox), MALLOC_CAP_SPIRAM) : NULL;
    if (inbox) {
        uint8_t in = storage_load_inbox(inbox, UI_MAX_MESSAGES);
        for (int i = in - 1; i >= 0; i--)
            if (!inbox[i].heard && s_unheard_count < UI_MAX_MESSAGES)
                strlcpy(s_unheard[s_unheard_count++], inbox[i].id, UI_ID_LEN);
        free(inbox);
    }

    power_user_activity();                 /* light the panel, drop shield */
    power_hold(true);                      /* and keep it lit: a long
                                              message outlives the dim timer */
    audio_play_chime(CHIME_PROMPT);        /* wake ack */
    ESP_LOGI(TAG, "session: %u contacts, %u unheard", n, s_unheard_count);
    if (s_unheard_count) {
        s_state = V_ASK_PLAY;
        VUI(ui_voice_show("New messages!", "Hear them?"));
        say("ask_play");
    } else {
        step_ask_send();
    }
}

static void answer_yes(void)
{
    window_close();
    s_retries = 0;
    switch (s_state) {
    case V_ASK_PLAY_WAIT:
        step_play_next();
        break;
    case V_ASK_SEND_WAIT:
        step_record();
        break;
    case V_ASK_CONFIRM_WAIT:
        if (s_app.record_send()) {         /* queues upload + sent chime */
            VUI(ui_voice_show("Sent!", s_contacts[s_cursor].name));
        } else {
            s_app.record_cancel();
        }
        session_end();
        break;
    default: break;
    }
}

static void answer_timeout(void)
{
    window_close();
    switch (s_state) {
    case V_ASK_PLAY_WAIT:
        if (s_retries++ == 0) {            /* ask once more, then move on */
            s_state = V_ASK_PLAY;
            say("ask_play");
        } else {
            s_retries = 0;
            s_cursor = 0;
            step_ask_send();
        }
        break;
    case V_ASK_SEND_WAIT:
        step_next_contact();
        break;
    case V_ASK_CONFIRM_WAIT:
        if (s_retries++ == 0) {
            s_state = V_ASK_CONFIRM;
            say("ask_confirm");
        } else {
            s_app.record_cancel();
            say("cancelled");
            session_end();
        }
        break;
    default: break;
    }
}

/* ---------------- events ---------------- */
void voice_on_hits(uint32_t mask)
{
    LOCK();
    if (s_state == V_IDLE) {
        if ((mask & VOICE_HIT_WAKE) && s_enabled) step_wake();
        UNLOCK();
        return;
    }
    bool wait = s_state == V_ASK_PLAY_WAIT || s_state == V_ASK_SEND_WAIT ||
                s_state == V_ASK_CONFIRM_WAIT;
    if (wait) {
        bool yes = (mask & VOICE_HIT_CONFIRM) != 0;
        if (!voice_infer_has_confirm() && (mask & VOICE_HIT_SPEECH))
            yes = ++s_speech_hops >= SPEECH_HOPS_YES;
        if (yes) answer_yes();
    }
    UNLOCK();
}

void voice_on_prompt_done(void)
{
    LOCK();
    switch (s_state) {
    case V_ASK_PLAY:    s_state = V_ASK_PLAY_WAIT;    window_open(); break;
    case V_ASK_SEND:    s_state = V_ASK_SEND_WAIT;    window_open(); break;
    case V_ASK_CONFIRM: s_state = V_ASK_CONFIRM_WAIT; window_open(); break;
    case V_PLAYING:
        s_app.mark_heard(s_unheard[s_play_idx]);
        s_play_idx++;
        step_play_next();
        break;
    default: break;   /* "cancelled" farewell etc: nothing follows */
    }
    UNLOCK();
}

void voice_on_record_done(uint16_t duration_s)
{
    LOCK();
    if (s_state == V_RECORDING) {
        if (duration_s == 0) {             /* silence or failure */
            say("cancelled");
            session_end();
        } else {
            s_retries = 0;
            s_state = V_ASK_CONFIRM;
            VUI(ui_voice_show("Send it?", s_contacts[s_cursor].name));
            say("ask_confirm");
        }
    }
    UNLOCK();
}

void voice_on_timeout(void)
{
    LOCK();
    answer_timeout();
    UNLOCK();
}

static void timer_cb(void *arg)
{
    /* esp_timer task: its stack is a few KB and answer_timeout() plays
     * prompts, updates LVGL, and can climb out of the sleep screen -
     * that once overflowed it. Relay to the audio task and do it there. */
    (void)arg;
    audio_voice_timeout();
}

/* ---------------- lifecycle ---------------- */
bool voice_enabled(void)        { return s_enabled; }
bool voice_session_active(void) { return s_state != V_IDLE; }

void voice_cancel(void)
{
    /* The X on the voice screen (LVGL task context). Ends the session
     * without touching the screen - the tapper is already closing it.
     * audio_stop() cuts a prompt mid-play; its prompt_done then arrives
     * in V_IDLE and falls through the switch. */
    LOCK();
    if (s_state != V_IDLE) {
        ESP_LOGI(TAG, "session dismissed");
        if (s_state == V_RECORDING || s_state == V_ASK_CONFIRM ||
            s_state == V_ASK_CONFIRM_WAIT)
            s_app.record_cancel();
        window_close();
        s_state = V_IDLE;
        power_hold(false);
        audio_stop();
    }
    UNLOCK();
}

void voice_set_enabled(bool on)
{
    LOCK();
    if (on != s_enabled) ESP_LOGI(TAG, "voice control %s", on ? "on" : "off");
    s_enabled = on;
    if (!on && s_state != V_IDLE) {
        if (s_state == V_RECORDING || s_state == V_ASK_CONFIRM ||
            s_state == V_ASK_CONFIRM_WAIT)
            s_app.record_cancel();
        session_end();
    }
    UNLOCK();
    audio_voice_listen(on);
}

void voice_init(const voice_app_t *app)
{
    s_app = *app;
    s_mtx = xSemaphoreCreateMutex();
    s_unheard = heap_caps_malloc(UI_MAX_MESSAGES * UI_ID_LEN,
                                 MALLOC_CAP_SPIRAM);
    const esp_timer_create_args_t targs = {
        .callback = timer_cb, .name = "voice_win",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));
}
