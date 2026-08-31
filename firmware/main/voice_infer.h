/* voice_infer: microWakeWord streaming inference behind a C API.
 * Wraps esp-tflite-micro (C++) + the micro-speech feature frontend so
 * audio.c can stay C. Everything runs in the audio task; nothing here
 * is thread-safe and nothing here touches the codec.
 *
 * Feed 10 ms hops (160 samples @ 16 kHz) of RAW mic PCM - before
 * rec_dsp(): the models are trained on unprocessed speech and the
 * limiter's non-linearity is not in the training data.
 *
 * Memory: tensor arenas and the feature frontend state live in PSRAM /
 * heap allocated once at init; models are in flash (voice_models.h). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_HIT_WAKE    (1u << 0)
#define VOICE_HIT_CONFIRM (1u << 1)
#define VOICE_HIT_SPEECH  (1u << 2)   /* energy VAD: voiced 10 ms hop */

/* Build interpreters + frontend. False = wake model missing/broken
 * (voice control cannot run; safe to call feed anyway, it no-ops). */
bool voice_infer_init(void);

bool voice_infer_has_confirm(void);   /* confirm model embedded? */

/* Clear streaming state + sliding windows + VAD (new listening phase,
 * or after the mic was closed for playback). */
void voice_infer_reset(void);

/* Run the confirm model only inside answer windows (saves ~half the
 * inference work while idling on the wake word alone). Arming resets
 * the confirm model's state. */
void voice_infer_arm_confirm(bool on);

/* Process one hop of raw PCM; returns VOICE_HIT_* mask. */
uint32_t voice_infer_feed(const int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
