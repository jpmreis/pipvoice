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

`models/hey_pip.tflite` and `models/yes.tflite` are **our trained
models** (2026-09-03, full-length schedules: wake 30k+20k steps, confirm
25k+20k). The wake word is pronunciation-tolerant by construction — the
positive set mixes canonical "hey pip" with *hey peep*, *hey peap*, and
short/long pauses between the words (weighted, see the notebook config
cell); "hey pap" and "hey pop" are trained as must-rejects. The confirm
model accepts yes/yeah/yep with "no"/"nope"/"not yet" as hard negatives.
`models/hey_jarvis.json`/`.tflite` remain as the pretrained reference
the integration was first benched against
([esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models),
Apache-2.0). If the confirm model is ever removed (zero-length array),
the firmware falls back to energy-VAD ("any speech in the answer
window = yes").

**Lesson burned in from v1.3.0–v1.3.2**: an early-stopped model *looks*
fine in the notebook's validation but produces **soft probabilities** on
real hardware — the first wake model (stopped 28k/45k) peaked at
0.4–0.6 on a real "hey pip" through the box mic, unreachable under the
0.97 cutoff the manifest suggests, and the box went deaf in production.
Let training run to completion, and never ship a manifest cutoff
unbenched (see Thresholds below).

## Training (Google Colab, GPU)

Everything is packaged in **`train_pip_models.ipynb`** — upload it to
[colab.research.google.com](https://colab.research.google.com) (or open
via GitHub), pick a **GPU runtime**, and **Run all**. It trains *both*
models ("hey pip" wake + "yes/yeah/yep" confirm, with "no" trained as a
hard negative), and ends by downloading `pip_voice_models.zip`.

Hardware: an **A100 + High-RAM** (Colab Pro) does both words in
~3-4 h at the full-length schedules (~20 compute units). A free **T4**
works but is slow and can run out of system RAM during validation — set
`LOW_RESOURCE = True` in the config cell and train one word per session
(`TRAIN_SLOTS`), ~3-4 h each. Nothing needs a local GPU. **Do not
early-stop** — see the lesson in Current state.

The notebook is adapted from the OHF-Voice
[basic_training_notebook](https://github.com/OHF-Voice/micro-wake-word/blob/main/notebooks/basic_training_notebook.ipynb)
with the working fixes from the community
[microwakeword-trainer](https://github.com/alfiedennen/microwakeword-trainer)
baked in (no kernel restart, patched train.py, per-word confusable
negatives). Piper synthesizes ~20k voiced utterances per word from IPA;
negatives are kahrendt's pre-computed spectrogram sets from Hugging
Face plus the confusables.

Back on the Mac: unzip into `models/` and run
**`./install_models.sh <wake_cutoff> <confirm_cutoff>`** — it
regenerates both firmware C arrays and rebuilds.

### Thresholds — always benched, never the manifest defaults

The `.json` manifests say 0.97/0.85; **ignore them**. Pick cutoffs from
real audio recorded **through a Pip box** (hold BOOT, say the phrase
several times, send; fetch the `.m4a` from the server, `ffmpeg` to
16 kHz mono wav) and run it through `validate.py` alongside negatives
(ordinary speech recorded the same way, TTS of near-miss phrases). Rules
of thumb learned on hardware: the live listen path scores somewhat lower
than a box *recording* of the same phrase (the recording passes the
recorder's gain/limiter and two codecs); every detection the firmware
makes logs its probability (`hey_pip detected (p=…)`) on the serial
console, which is the ground truth to tune against — a cutoff change is
just `./install_models.sh` + a patch release, no retraining. Shipped
since v1.3.4: wake 0.45, confirm 0.10.

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

Note: pymicro-features **2.0.x** returns features pre-divided by 25.6;
validate.py compensates (`FEATURE_PRESCALE`). Without that every model
silently scores 0.0 — if a known-good model shows 0% everywhere,
suspect this first. macOS `say` voices are also a poor probe: they are
far enough out of the piper training distribution that even correct
models barely react; use box-recorded audio.

## Export to firmware

```
python tflite_to_c.py models/hey_jarvis.tflite wake 0.97 > ../../firmware/main/voice_model_wake.c
python tflite_to_c.py models/yes.tflite confirm 0.85 > ../../firmware/main/voice_model_confirm.c
```

(word slot = `wake` | `confirm`; last arg = runtime probability
cutoff 0..1.) The firmware reads only `voice_model_<slot>_data/len/
cutoff/name` — regenerating the file swaps the word with no other
code change. A zero-length confirm model keeps the VAD fallback.
