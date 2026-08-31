# Wake-word models ("Hey Pip" voice control)

Training, validation and export tooling for the two on-device keyword
models used by the voice-control accessibility feature:

- **wake** — "hey pip", always armed while voice control is enabled
- **confirm** — "yes", armed only during short answer windows

Both are [microWakeWord](https://github.com/OHF-Voice/micro-wake-word)
streaming models (int8 TFLite, ~50-100 KB) running on
`esp-tflite-micro` inside the firmware's audio task. The models are
**committed** here and embedded into the app binary by
`tflite_to_c.py` — they are build inputs, not artifacts.

## Current state

`models/hey_jarvis.tflite` is the **pretrained stand-in** (from
[esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models),
Apache-2.0) so the firmware integration can be benched by saying
"Hey Jarvis" before our own models exist. There is no pretrained
"yes": until a confirm model is dropped in, the firmware falls back to
energy-VAD ("any speech in the answer window = yes"), which is also
the permanent fallback if the confirm model ever misbehaves.

## Training (Google Colab, GPU)

Hardware: a free Colab T4 is enough; a full run (sample generation +
training) is roughly 1-2 h per word. Nothing here needs a local GPU.

1. Open the microWakeWord
   [basic_training_notebook](https://github.com/OHF-Voice/micro-wake-word/blob/main/notebooks/basic_training_notebook.ipynb)
   on Colab (GPU runtime). If the upstream notebook has dependency
   bit-rot, the community wrapper
   [microwakeword-trainer](https://github.com/alfiedennen/microwakeword-trainer)
   patches the known issues.
2. Set the target phrase: `hey pip` (then a second run for `yes`).
   Piper-sample-generator synthesizes ~2000-4000 utterances across many
   English voices with pitch/speed/noise/room augmentation; negatives
   come from the pre-computed spectrogram sets on Hugging Face that the
   notebook downloads.
3. Thresholds: train as-is; the *runtime* cutoff is set at export
   (below). Aim strict for "hey pip" (always armed), permissive for
   "yes" (window-gated, a false accept costs little).
4. Download the quantized **streaming** `.tflite` into `models/` as
   `hey_pip.tflite` / `yes.tflite`, with a sidecar `.json` noting the
   notebook settings (copy the shape of `models/hey_jarvis.json`).

### Personal fine-tune (only if real-world detection disappoints)

Record the actual user saying each phrase ~50-100 times **on a Pip
box** (same ES8311 mic + gain chain the model will hear through):
hold BOOT, say the phrase, send to any contact. Then on the server:
collect the `.vmsg` files, decode to 16 kHz wavs with
`server/app/vmsg.py` (`_vmsg_to_pcm`), add them to the notebook's
positive-sample set, retrain.

## Validation (Mac, CPU)

```
python3 -m venv .venv && . .venv/bin/activate
pip install pymicro-features ai-edge-litert numpy
python validate.py models/hey_pip.tflite positives/ negatives/
```

`validate.py` streams wavs through the identical feature frontend +
model the firmware runs. Ship gates: >90% detection on held-out
positives, <0.5 false accepts/hour over hours of podcast/TV audio in
`negatives/`. It prints the detection rate per threshold so the
firmware cutoff can be picked from data.

## Export to firmware

```
python tflite_to_c.py models/hey_jarvis.tflite wake 0.97 > ../../firmware/main/voice_model_wake.c
python tflite_to_c.py models/yes.tflite confirm 0.85 > ../../firmware/main/voice_model_confirm.c
```

(word slot = `wake` | `confirm`; last arg = runtime probability
cutoff 0..1.) The firmware reads only `voice_model_<slot>_data/len/
cutoff/name` — regenerating the file swaps the word with no other
code change. A zero-length confirm model keeps the VAD fallback.
