#include "audio.h"
#include "opus_file.h"
#include "storage.h"
#include "config.h"
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
               CMD_PLAY, CMD_STOP, CMD_CHIME } cmd_id_t;
typedef struct { cmd_id_t id; char path[96]; chime_t chime; } cmd_t;

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

/* ---------------- recording ---------------- */
static void do_record(void)
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

    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, 0) == pdTRUE) {
            if (c.id == CMD_REC_STOP) break;
            if (c.id == CMD_REC_CANCEL) { cancelled = true; break; }
        }
        if (frames >= max_frames) break;

        if (esp_codec_dev_read(s_mic, pcm, sizeof(pcm)) != 0) break;
        rec_dsp(pcm, VMSG_FRAME_SAMPLES);
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
static void do_play(const char *path)
{
    uint16_t total = 0;
    vmsg_reader_t *r = vmsg_reader_open(path, &total);
    if (!r || esp_codec_dev_open(s_spk, &s_fs) != 0) {
        if (r) vmsg_reader_close(r);
        if (s_ev.play_done) s_ev.play_done();
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
            if (s_ev.play_progress) s_ev.play_progress(sec, total);
        }
    }

    esp_codec_dev_close(s_spk);
    vmsg_reader_close(r);
    if (!interrupted && s_ev.play_done) s_ev.play_done();
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
    const float rise[2] = { 880.0f, 1320.0f };
    const float fall[2] = { 1320.0f, 880.0f };
    const float *freqs = (which == CHIME_SENT) ? rise : fall;
    const float onset2 = 0.150f;                 /* second note start, s   */
    const int total_frames = 650 / VMSG_FRAME_MS;   /* ~650ms incl. ring-out */
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

/* ---------------- task ---------------- */
static void audio_task(void *arg)
{
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.id) {
            case CMD_REC_START: do_record();       break;
            case CMD_PLAY:      do_play(c.path);   break;
            case CMD_CHIME:     do_chime(c.chime); break;
            case CMD_REC_CANCEL:                   /* recording already done:
                                                      discard the finished file */
                remove(OUTBOX_DIR "/rec_tmp.vmsg");
                break;
            default: break;   /* stray STOPs ignored when idle */
        }
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
