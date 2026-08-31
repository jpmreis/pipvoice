#include "audio.h"
#include "opus_file.h"
#include "storage.h"
#include "config.h"
#include "voice_infer.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio";

typedef enum { CMD_REC_START, CMD_REC_STOP, CMD_REC_CANCEL,
               CMD_PLAY, CMD_STOP, CMD_CHIME,
               CMD_NOP /* wake the task so it re-reads s_listen_want */
} cmd_id_t;
#define AF_PROMPT 0x01   /* play: completion -> prompt_done, no progress */
#define AF_VAD    0x02   /* record: auto-stop on trailing silence        */
typedef struct { cmd_id_t id; char path[96]; chime_t chime;
                 uint8_t flags; } cmd_t;

static QueueHandle_t       s_q;
static SemaphoreHandle_t   s_rec_done;
static audio_events_t      s_ev;
static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_codec_ok;

bool audio_codec_ok(void) { return s_codec_ok; }

/* not const: esp_codec_dev_open() takes a mutable pointer */
static esp_codec_dev_sample_info_t s_fs = {
    .bits_per_sample = 16,
    .channel = 1,
    .sample_rate = VMSG_SAMPLE_RATE,
    .mclk_multiple = 256,
};

/* ES8311 analog mic gain. The chip supports 0..42 dB in 6 dB steps; the
 * official Waveshare recording demo uses 18 dB, which leaves speech quiet
 * at arm's length - tune on hardware if recordings clip or stay faint.
 * Bench: 24 dB -> -34 dBFS RMS speech, 30 dB -> -32; analog alone can't
 * reach a comfortable level, so digital gain with soft limiting is applied
 * on top (see rec_dsp).
 * Prod finding 2026-08-12: at 36 dB, close-range speech (how the boxes are
 * actually used) ran -8 dBFS RMS with 1-2% of samples ADC-clipped - the
 * "hitting a ceiling" sound. Analog clipping is unrecoverable, so keep
 * analog low for headroom and make the level up digitally. */
#define MIC_GAIN_DB      24.0f
#define REC_DIGITAL_GAIN 3.0f     /* +9.5 dB */

/* ---- record-path DSP: high-pass -> gain -> limiter ----
 * 42 dB of mic gain amplifies room rumble along with speech; a 120 Hz
 * Butterworth high-pass drops everything below the voice band. Runs per
 * sample at 16 kHz on the FPU - well under 1% CPU.
 * A noise gate (downward expander) was tried here and removed: it pumped
 * on syllable gaps and made speech harder to hear (bench, 2026-08-11).
 * If between-word hiss ever matters, the fix is a proper spectral
 * suppressor (WebRTC NS + esp-dsp FFT), not a broadband gate. */
static float hp_x1, hp_x2, hp_y1, hp_y2;   /* biquad state             */

static void rec_dsp_reset(void)
{
    hp_x1 = hp_x2 = hp_y1 = hp_y2 = 0.0f;
}

static void rec_dsp(int16_t *pcm, int n)
{
    /* Butterworth high-pass, fc = 120 Hz @ 16 kHz */
    const float b0 = 0.967224f, b1 = -1.934449f, b2 = 0.967224f,
                a1 = -1.933375f, a2 = 0.935521f;

    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i];
        float y = b0 * x + b1 * hp_x1 + b2 * hp_x2
                         - a1 * hp_y1 - a2 * hp_y2;
        hp_x2 = hp_x1; hp_x1 = x;
        hp_y2 = hp_y1; hp_y1 = y;

        /* gain + soft knee: linear to 22000, 3:1 above, clamp. Gentler
         * than the old 6:1@24000 - with 12 dB more analog headroom the
         * knee only rounds occasional peaks instead of crushing speech */
        float v = y * REC_DIGITAL_GAIN;
        float mag = fabsf(v);
        if (mag > 22000.0f) mag = 22000.0f + (mag - 22000.0f) / 3.0f;
        if (mag > 32767.0f) mag = 32767.0f;
        pcm[i] = (int16_t)(v < 0.0f ? -mag : mag);
    }
}

/* voice-flow recording end-pointing (AF_VAD): post-DSP RMS per 20 ms
 * frame. Values are post-gain (x3), so close speech sits well above the
 * threshold; tune on hardware with the serial log if the cut is early. */
#define REC_VAD_SPEECH_RMS   1200.0f
#define REC_VAD_TRAIL_MS     1500    /* stop after this much trailing hush */
#define REC_VAD_GIVEUP_MS    5000    /* nothing said at all: give up      */

/* ---------------- recording ---------------- */
static void do_record(uint8_t flags)
{
    if (storage_free_kb() < 512) {
        /* a full inbox must not block recording - evict before refusing */
        storage_evict_if_needed();
        if (storage_free_kb() < 512) {
            ESP_LOGW(TAG, "storage low, refusing to record");
            if (s_ev.record_done) s_ev.record_done(0);
            return;
        }
    }

    vmsg_writer_t *w = vmsg_writer_open(OUTBOX_DIR "/rec_tmp.vmsg");
    if (!w || esp_codec_dev_open(s_mic, &s_fs) != 0) {
        if (w) vmsg_writer_close(w, 0);
        if (s_ev.record_done) s_ev.record_done(0);
        return;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);

    int16_t pcm[VMSG_FRAME_SAMPLES];

    /* the codec pops on power-up; drop the first 40 ms so the message
     * doesn't start with a click at near-full-scale */
    for (int i = 0; i < 2; i++)
        esp_codec_dev_read(s_mic, pcm, sizeof(pcm));
    rec_dsp_reset();

    uint32_t frames = 0;
    const uint32_t max_frames =
        (uint32_t)g_cfg.max_message_s * 1000 / VMSG_FRAME_MS;
    uint16_t last_tick = 0xFFFF;
    bool cancelled = false;
    bool     spoke = false;          /* AF_VAD: any speech yet?          */
    uint32_t hush_frames = 0;        /* AF_VAD: consecutive quiet frames */

    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, 0) == pdTRUE) {
            if (c.id == CMD_REC_STOP) break;
            if (c.id == CMD_REC_CANCEL) { cancelled = true; break; }
        }
        if (frames >= max_frames) break;

        if (esp_codec_dev_read(s_mic, pcm, sizeof(pcm)) != 0) break;
        rec_dsp(pcm, VMSG_FRAME_SAMPLES);
        if (flags & AF_VAD) {
            float acc = 0;
            for (int i = 0; i < VMSG_FRAME_SAMPLES; i++)
                acc += (float)pcm[i] * (float)pcm[i];
            bool loud = sqrtf(acc / (float)VMSG_FRAME_SAMPLES)
                        > REC_VAD_SPEECH_RMS;
            hush_frames = loud ? 0 : hush_frames + 1;
            if (loud) spoke = true;
            if (spoke && hush_frames * VMSG_FRAME_MS >= REC_VAD_TRAIL_MS)
                break;                       /* they finished talking     */
            if (!spoke && frames * VMSG_FRAME_MS >= REC_VAD_GIVEUP_MS)
                break;                       /* nothing but room tone:
                                                reported as dur 0 below   */
        }
        if (!vmsg_writer_frame(w, pcm)) break;
        frames++;

        uint16_t sec = (uint16_t)(frames * VMSG_FRAME_MS / 1000);
        if (sec != last_tick) {
            last_tick = sec;
            if (s_ev.record_tick) s_ev.record_tick(sec, g_cfg.max_message_s);
        }
    }

    esp_codec_dev_close(s_mic);
    uint16_t dur = (uint16_t)(frames * VMSG_FRAME_MS / 1000);
    if ((flags & AF_VAD) && !spoke) dur = 0;   /* only room tone captured */
    vmsg_writer_close(w, dur);
    if (cancelled) {
        remove(OUTBOX_DIR "/rec_tmp.vmsg");
        ESP_LOGI(TAG, "recording cancelled");
    } else {
        if (s_ev.record_done) s_ev.record_done(dur);
        ESP_LOGI(TAG, "recorded %us (stack headroom %u)", dur,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    xSemaphoreGive(s_rec_done);
}

/* ---------------- playback ---------------- */
/* AF_PROMPT (voice flow): completion goes to prompt_done instead of
 * play_done and no progress ticks - the playback screen isn't open.
 * An unopenable path still fires prompt_done: voice.c leans on that to
 * fall through to the answer window when a prompt clip is missing. */
static void do_play(const char *path, uint8_t flags)
{
    bool prompt = (flags & AF_PROMPT) != 0;
    uint16_t total = 0;
    vmsg_reader_t *r = vmsg_reader_open(path, &total);
    if (!r || esp_codec_dev_open(s_spk, &s_fs) != 0) {
        if (r) vmsg_reader_close(r);
        if (prompt) { if (s_ev.prompt_done) s_ev.prompt_done(); }
        else if (s_ev.play_done) s_ev.play_done();
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, g_cfg.speaker_volume);

    int16_t pcm[VMSG_FRAME_SAMPLES];
    uint32_t frames = 0;
    uint16_t last = 0xFFFF;
    bool interrupted = false;

    while (vmsg_reader_frame(r, pcm)) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, 0) == pdTRUE && c.id == CMD_STOP) {
            interrupted = true;
            break;
        }
        esp_codec_dev_write(s_spk, pcm, sizeof(pcm));
        frames++;
        uint16_t sec = (uint16_t)(frames * VMSG_FRAME_MS / 1000);
        if (sec != last) {
            last = sec;
            if (!prompt && s_ev.play_progress) s_ev.play_progress(sec, total);
        }
    }

    esp_codec_dev_close(s_spk);
    vmsg_reader_close(r);
    if (prompt) { if (s_ev.prompt_done) s_ev.prompt_done(); }
    else if (!interrupted && s_ev.play_done) s_ev.play_done();
}

/* One struck-tine note (celesta / music-box family): a stack of partials
 * whose overtones decay faster than the fundamental - the signature of a
 * real struck instrument, vs. the pure gated sine this replaced. The 5.4x
 * partial is inharmonic on purpose: it's the metallic "glint" of the strike
 * and is gone within ~50 ms. 5 ms raised-cosine attack kills the onset
 * click. Mirrored in the PWA (app.js playPips) - keep the two in sync. */
static float chime_note(float t, float f)
{
    if (t < 0.0f) return 0.0f;
    const float w = 2.0f * (float)M_PI * f * t;
    float atk = (t < 0.005f)
        ? 0.5f - 0.5f * cosf((float)M_PI * t / 0.005f) : 1.0f;
    return atk * (        expf(-t / 0.150f) * sinf(w)
                 + 0.30f * expf(-t / 0.090f) * sinf(2.0f * w)
                 + 0.12f * expf(-t / 0.055f) * sinf(3.0f * w)
                 + 0.10f * expf(-t / 0.025f) * sinf(5.4f * w));
}

static void do_chime(chime_t which)
{
    /* two overlapping notes a fifth apart - the second strikes while the
     * first still rings, like two kalimba tines. sent rises, received
     * falls. Peak sum measured -11 dBFS at amp 6500.
     * Pre-rendered BEFORE the codec opens: synthesizing live (~12
     * transcendental calls/sample) raced the 20 ms frame deadline and
     * garbled whenever wifi preempted this core. 21 KB @16k -> PSRAM. */
    /* CHIME_PROMPT (voice flow: wake ack / "go" cue / missing-prompt
     * fallback) is a single struck tine - box-only, no PWA twin. */
    const float rise[2] = { 880.0f, 1320.0f };
    const float fall[2] = { 1320.0f, 880.0f };
    const float one[2]  = { 990.0f, 0.0f };
    const float *freqs = (which == CHIME_SENT) ? rise
                       : (which == CHIME_PROMPT) ? one : fall;
    const float onset2 = 0.150f;                 /* second note start, s   */
    const int total_frames = (which == CHIME_PROMPT ? 350 : 650)
                             / VMSG_FRAME_MS;       /* incl. ring-out        */
    const int n = total_frames * VMSG_FRAME_SAMPLES;
    int16_t *pcm = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / VMSG_SAMPLE_RATE;
        pcm[i] = (int16_t)(6500.0f *
            (chime_note(t, freqs[0]) + chime_note(t - onset2, freqs[1])));
    }
    if (esp_codec_dev_open(s_spk, &s_fs) == 0) {
        esp_codec_dev_set_out_vol(s_spk, g_cfg.speaker_volume);
        for (int f = 0; f < total_frames; f++)
            esp_codec_dev_write(s_spk, pcm + f * VMSG_FRAME_SAMPLES,
                                VMSG_FRAME_SAMPLES * sizeof(int16_t));
        esp_codec_dev_close(s_spk);
    }
    heap_caps_free(pcm);
}

/* ---------------- wake-word listening ---------------- */
/* Voice control keeps the mic open between commands and streams 10 ms
 * hops through voice_infer (wake/"yes" models + energy VAD). Runs in
 * this task because mic and speaker are two codec-dev handles on the
 * same ES8311 over one I2S duplex pair: whoever owns the chip must be
 * the one to stop listening before opening the speaker. Commands still
 * preempt within one hop (queue polled per iteration, like do_record).
 * The models want RAW pcm - rec_dsp()'s limiter stays out of this path. */
static volatile bool s_listen_want;
static void handle_cmd(const cmd_t *c);

static void do_listen(void)
{
    static bool infer_ready, infer_failed;
    if (!infer_ready) {
        if (infer_failed || !(infer_ready = voice_infer_init())) {
            /* broken model: stop asking, or this would spin */
            if (!infer_failed) ESP_LOGE(TAG, "voice models unusable");
            infer_failed = true;
            s_listen_want = false;
            return;
        }
    }
    if (esp_codec_dev_open(s_mic, &s_fs) != 0) {
        ESP_LOGW(TAG, "listen: mic open failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);

    int16_t pcm[VMSG_FRAME_SAMPLES / 2];        /* one 10 ms hop */
    for (int i = 0; i < 4; i++)                 /* power-up pop */
        esp_codec_dev_read(s_mic, pcm, sizeof(pcm));
    voice_infer_reset();

    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, 0) == pdTRUE) {
            /* release the codec BEFORE the command runs: it may open
             * the speaker (prompt/chime) or reopen the mic (record) */
            esp_codec_dev_close(s_mic);
            handle_cmd(&c);
            return;
        }
        if (!s_listen_want) {
            esp_codec_dev_close(s_mic);
            return;
        }
        if (esp_codec_dev_read(s_mic, pcm, sizeof(pcm)) != 0) {
            esp_codec_dev_close(s_mic);
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }
        uint32_t hits = voice_infer_feed(pcm, VMSG_FRAME_SAMPLES / 2);
        if (hits && s_ev.voice_hits) s_ev.voice_hits(hits);
    }
}

/* ---------------- task ---------------- */
static void handle_cmd(const cmd_t *c)
{
    switch (c->id) {
        case CMD_REC_START: do_record(c->flags);          break;
        case CMD_PLAY:      do_play(c->path, c->flags);   break;
        case CMD_CHIME:     do_chime(c->chime);           break;
        case CMD_REC_CANCEL:                   /* recording already done:
                                                  discard the finished file */
            remove(OUTBOX_DIR "/rec_tmp.vmsg");
            break;
        default: break;   /* stray STOPs / NOPs ignored when idle */
    }
}

static void audio_task(void *arg)
{
    for (;;) {
        if (s_listen_want && s_codec_ok) {
            do_listen();          /* returns after handling one command
                                     or when listening was switched off */
            continue;
        }
        cmd_t c;
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) continue;
        handle_cmd(&c);
    }
}

void audio_init(const audio_events_t *ev)
{
    s_ev = *ev;
    s_q = xQueueCreate(4, sizeof(cmd_t));
    s_rec_done = xSemaphoreCreateBinary();
    /* Known upstream issue: bsp_audio_codec_speaker_init() can assert in
     * i2c_ctrl_if on some boards (waveshareteam/ESP32-S3-Touch-AMOLED-1.8
     * PR #6). If codec init ever boot-loops, build the I2S channels and
     * ES8311 codec-dev explicitly as their 12_i2s_codec example does. */
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_spk || !s_mic)
        ESP_LOGE(TAG, "codec init failed (spk=%p mic=%p)", s_spk, s_mic);
    s_codec_ok = s_spk && s_mic;
    /* libopus (SILK, fixed-point) burns ~25 KB of stack in opus_encode -
     * 8 KB overflowed the moment recording started. Must stay internal
     * RAM: this task also writes flash (LittleFS) with cache disabled.
     * Statically allocated: boot-time internal heap has no contiguous
     * 32 KB block left once LVGL is up, so a heap-backed stack fails. */
    static StaticTask_t audio_tcb;
    static StackType_t  audio_stack[32768];
    if (!xTaskCreateStaticPinnedToCore(audio_task, "audio",
                                       sizeof(audio_stack), NULL, 6,
                                       audio_stack, &audio_tcb, 0)) {
        ESP_LOGE(TAG, "audio task create FAILED - no recording or playback");
    }
}

void audio_set_volume(uint8_t pct)
{
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, pct);
}

static void post(cmd_id_t id, const char *path)
{
    cmd_t c = { .id = id };
    if (path) strlcpy(c.path, path, sizeof(c.path));
    xQueueSend(s_q, &c, 0);
}

void audio_record_start(void)
{
    xSemaphoreTake(s_rec_done, 0);          /* clear any stale signal */
    post(CMD_REC_START, NULL);
}

bool audio_wait_record_done(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;
    xSemaphoreGive(s_rec_done);             /* stay signalled until next start */
    return true;
}

void audio_record_stop(void)         { post(CMD_REC_STOP, NULL); }
void audio_record_cancel(void)       { post(CMD_REC_CANCEL, NULL); }
void audio_play_file(const char *p)  { post(CMD_PLAY, p); }
void audio_stop(void)                { post(CMD_STOP, NULL); }
void audio_play_chime(chime_t which)
{
    cmd_t c = { .id = CMD_CHIME, .chime = which };
    xQueueSend(s_q, &c, 0);
}

/* ---------------- voice control ---------------- */
void audio_voice_listen(bool on)
{
    s_listen_want = on;
    /* the task may be blocked on the queue: poke it so it re-evaluates */
    if (on) post(CMD_NOP, NULL);
}

void audio_play_prompt(const char *path)
{
    cmd_t c = { .id = CMD_PLAY, .flags = AF_PROMPT };
    strlcpy(c.path, path, sizeof(c.path));
    xQueueSend(s_q, &c, 0);
}

void audio_record_start_vad(void)
{
    xSemaphoreTake(s_rec_done, 0);          /* clear any stale signal */
    cmd_t c = { .id = CMD_REC_START, .flags = AF_VAD };
    xQueueSend(s_q, &c, 0);
}
