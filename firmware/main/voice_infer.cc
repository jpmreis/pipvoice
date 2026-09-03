/* microWakeWord streaming inference: micro-speech frontend -> two int8
 * streaming models (wake + confirm). Mirrors ESPHome's micro_wake_word
 * runtime (streaming_model.cpp), which these models were built for:
 * 40 spectrogram features per 10 ms hop, int8-scaled by *256/666-128,
 * model invoked once per `stride` hops, detection = mean of the last
 * WINDOW probabilities over the model's cutoff. validate.py in
 * tools/wakeword replicates this loop bit-for-bit on the Mac. */
#include "voice_infer.h"
#include "voice_models.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

#include "frontend.h"
#include "frontend_util.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "voice_infer";

#define FEATURE_SIZE     40          /* filterbank channels            */
#define HOP_SAMPLES      160         /* 10 ms @ 16 kHz                 */
#define WINDOW           5           /* sliding probability window     */
#define REFRACTORY_MS    2000        /* mute after a detection         */
#define ARENA_BYTES      (48 * 1024) /* per model, PSRAM (manifest says
                                        ~23 KB; headroom is cheap)     */
#define VAR_ARENA_BYTES  (4 * 1024)  /* streaming state variables      */

/* ---- energy VAD (speech gate for the confirm fallback and the
 * recorder's auto-stop). Raw-PCM RMS against an adaptive noise floor:
 * fixed thresholds failed across quiet bedrooms vs. kitchens. */
#define VAD_FLOOR_INIT   200.0f
#define VAD_RATIO        3.0f        /* speech = rms > floor * ratio   */
#define VAD_FLOOR_DECAY  0.995f      /* floor tracks quiet hops        */

struct model_slot {
    const unsigned char *data;
    unsigned int         len;
    float                cutoff;
    const char          *name;

    const tflite::Model             *model;
    tflite::MicroInterpreter        *interpreter;
    tflite::MicroResourceVariables  *mrv;
    uint8_t   *arena;
    uint8_t   *var_arena;
    int        stride;               /* feature frames per invoke      */
    int        pending;              /* frames written since invoke    */
    float      probs[WINDOW];
    int        prob_n;
    int64_t    mute_until_us;
    bool       ok;
};

static struct FrontendConfig s_fe_cfg;
static struct FrontendState  s_fe;
static bool                  s_fe_ok;
static model_slot            s_wake, s_confirm;
static bool                  s_confirm_armed;
static float                 s_vad_floor = VAD_FLOOR_INIT;

/* the 20 ops microWakeWord streaming models use (ESPHome's resolver) */
static tflite::MicroMutableOpResolver<20> *resolver(void)
{
    static tflite::MicroMutableOpResolver<20> r;
    static bool done;
    if (!done) {
        done = true;
        r.AddCallOnce();        r.AddVarHandle();
        r.AddReshape();         r.AddReadVariable();
        r.AddStridedSlice();    r.AddConcatenation();
        r.AddAssignVariable();  r.AddConv2D();
        r.AddMul();             r.AddAdd();
        r.AddMean();            r.AddFullyConnected();
        r.AddLogistic();        r.AddQuantize();
        r.AddDepthwiseConv2D(); r.AddAveragePool2D();
        r.AddMaxPool2D();       r.AddPad();
        r.AddPack();            r.AddSplitV();
    }
    return &r;
}

static void slot_reset(model_slot *s)
{
    if (!s->ok) return;
    s->interpreter->Reset();
    s->pending = 0;
    s->prob_n = 0;
    s->mute_until_us = 0;
}

static bool slot_init(model_slot *s, const unsigned char *data,
                      unsigned int len, float cutoff, const char *name)
{
    s->data = data; s->len = len; s->cutoff = cutoff; s->name = name;
    if (!len) return false;

    s->model = tflite::GetModel(data);
    if (s->model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "%s: schema %lu != %d", name,
                 (unsigned long)s->model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    s->arena = (uint8_t *)heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
    s->var_arena = (uint8_t *)heap_caps_malloc(VAR_ARENA_BYTES,
                                               MALLOC_CAP_SPIRAM);
    if (!s->arena || !s->var_arena) {
        ESP_LOGE(TAG, "%s: arena alloc failed", name);
        return false;
    }
    tflite::MicroAllocator *ma =
        tflite::MicroAllocator::Create(s->var_arena, VAR_ARENA_BYTES);
    s->mrv = tflite::MicroResourceVariables::Create(ma, 20);
    s->interpreter = new tflite::MicroInterpreter(
        s->model, *resolver(), s->arena, ARENA_BYTES, s->mrv);
    if (s->interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "%s: AllocateTensors failed", name);
        return false;
    }
    TfLiteTensor *in = s->interpreter->input(0);
    if (in->dims->size != 3 || in->dims->data[2] != FEATURE_SIZE ||
        in->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "%s: unexpected input tensor", name);
        return false;
    }
    s->stride = in->dims->data[1];
    s->ok = true;
    ESP_LOGI(TAG, "%s ready: %u B model, stride %d, cutoff %.2f, "
             "arena used %u", name, len, s->stride, (double)s->cutoff,
             (unsigned)s->interpreter->arena_used_bytes());
    slot_reset(s);
    return true;
}

/* push one 40-value int8 feature frame; true on detection */
static bool slot_feed(model_slot *s, const int8_t *feat)
{
    if (!s->ok) return false;
    TfLiteTensor *in = s->interpreter->input(0);
    memcpy(in->data.int8 + s->pending * FEATURE_SIZE, feat, FEATURE_SIZE);
    if (++s->pending < s->stride) return false;
    s->pending = 0;

    if (s->interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "%s: invoke failed", s->name);
        return false;
    }
    float p = s->interpreter->output(0)->data.uint8[0] / 255.0f;
    if (s->prob_n < WINDOW) s->probs[s->prob_n++] = p;
    else {
        memmove(s->probs, s->probs + 1, (WINDOW - 1) * sizeof(float));
        s->probs[WINDOW - 1] = p;
    }
    if (s->prob_n < WINDOW) return false;

    float mean = 0;
    for (int i = 0; i < WINDOW; i++) mean += s->probs[i];
    mean /= WINDOW;

    int64_t now = esp_timer_get_time();
    if (mean > s->cutoff && now >= s->mute_until_us) {
        s->mute_until_us = now + (int64_t)REFRACTORY_MS * 1000;
        s->prob_n = 0;
        ESP_LOGI(TAG, "%s detected (p=%.2f)", s->name, (double)mean);
        return true;
    }
    return false;
}

extern "C" bool voice_infer_init(void)
{
    FrontendFillConfigWithDefaults(&s_fe_cfg);
    s_fe_cfg.window.size_ms = 30;
    s_fe_cfg.window.step_size_ms = 10;
    s_fe_cfg.filterbank.num_channels = FEATURE_SIZE;
    s_fe_cfg.filterbank.lower_band_limit = 125.0f;
    s_fe_cfg.filterbank.upper_band_limit = 7500.0f;
    s_fe_cfg.noise_reduction.smoothing_bits = 10;
    s_fe_cfg.noise_reduction.even_smoothing = 0.025f;
    s_fe_cfg.noise_reduction.odd_smoothing = 0.06f;
    s_fe_cfg.noise_reduction.min_signal_remaining = 0.05f;
    s_fe_cfg.pcan_gain_control.enable_pcan = 1;
    s_fe_cfg.pcan_gain_control.strength = 0.95f;
    s_fe_cfg.pcan_gain_control.offset = 80.0f;
    s_fe_cfg.pcan_gain_control.gain_bits = 21;
    s_fe_cfg.log_scale.enable_log = 1;
    s_fe_cfg.log_scale.scale_shift = 6;
    s_fe_ok = FrontendPopulateState(&s_fe_cfg, &s_fe, 16000) != 0;
    if (!s_fe_ok) {
        ESP_LOGE(TAG, "frontend init failed");
        return false;
    }
    bool wake = slot_init(&s_wake, voice_model_wake_data,
                          voice_model_wake_len, voice_model_wake_cutoff,
                          voice_model_wake_name);
    slot_init(&s_confirm, voice_model_confirm_data,
              voice_model_confirm_len, voice_model_confirm_cutoff,
              voice_model_confirm_name);   /* optional: VAD fallback */
    return wake;
}

extern "C" bool voice_infer_has_confirm(void) { return s_confirm.ok; }

extern "C" void voice_infer_reset(void)
{
    if (s_fe_ok) FrontendReset(&s_fe);
    slot_reset(&s_wake);
    slot_reset(&s_confirm);
    s_vad_floor = VAD_FLOOR_INIT;
}

extern "C" void voice_infer_arm_confirm(bool on)
{
    if (on && s_confirm.ok) slot_reset(&s_confirm);
    s_confirm_armed = on;
}

extern "C" void voice_infer_confirm_restart(void)
{
    slot_reset(&s_confirm);
}

extern "C" bool voice_infer_feed_confirm(const int16_t *pcm, size_t n)
{
    if (!s_fe_ok || !s_confirm.ok || n == 0) return false;
    bool hit = false;
    size_t consumed = 0;
    while (consumed < n) {
        size_t read = 0;
        struct FrontendOutput out = FrontendProcessSamples(
            &s_fe, pcm + consumed, n - consumed, &read);
        consumed += read;
        if (read == 0) break;
        if (out.size != FEATURE_SIZE) continue;
        int8_t feat[FEATURE_SIZE];
        for (size_t i = 0; i < FEATURE_SIZE; i++) {
            int32_t v = ((int32_t)out.values[i] * 256) / 666 - 128;
            feat[i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
        if (slot_feed(&s_confirm, feat)) hit = true;
    }
    return hit;
}

extern "C" uint32_t voice_infer_feed(const int16_t *pcm, size_t n)
{
    if (!s_fe_ok || !s_wake.ok || n == 0) return 0;
    uint32_t hits = 0;

    /* energy VAD on the raw hop */
    float acc = 0;
    for (size_t i = 0; i < n; i++)
        acc += (float)pcm[i] * (float)pcm[i];
    float rms = sqrtf(acc / (float)n);
    if (rms > s_vad_floor * VAD_RATIO) hits |= VOICE_HIT_SPEECH;
    else s_vad_floor = s_vad_floor * VAD_FLOOR_DECAY
                     + rms * (1.0f - VAD_FLOOR_DECAY);

    /* frontend -> feature frames -> models */
    size_t consumed = 0;
    while (consumed < n) {
        size_t read = 0;
        struct FrontendOutput out = FrontendProcessSamples(
            &s_fe, pcm + consumed, n - consumed, &read);
        consumed += read;
        if (read == 0) break;              /* defensive: never spin */
        if (out.size != FEATURE_SIZE) continue;

        int8_t feat[FEATURE_SIZE];
        for (size_t i = 0; i < FEATURE_SIZE; i++) {
            /* ESPHome scaling: (value * 256 / 666) - 128, clamped */
            int32_t v = ((int32_t)out.values[i] * 256) / 666 - 128;
            feat[i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
        if (slot_feed(&s_wake, feat)) hits |= VOICE_HIT_WAKE;
        if (s_confirm_armed && slot_feed(&s_confirm, feat))
            hits |= VOICE_HIT_CONFIRM;
    }
    return hits;
}
