#!/usr/bin/env python3
"""Bench a microWakeWord streaming model the way the firmware runs it.

Usage: validate.py <model.tflite> <positives_dir> [negatives_dir]

Streams every 16 kHz mono wav through the micro-speech feature frontend
(pymicro-features - identical settings to the firmware's
esp-micro-speech-features) and the streaming model, sweeping detection
cutoffs, so the firmware threshold can be picked from data:

  positives: fraction of files with >=1 detection (want > 0.90)
  negatives: false accepts per hour of audio    (want < 0.5 for wake)

deps: pip install pymicro-features ai-edge-litert numpy
"""
import sys
import wave
from pathlib import Path

import numpy as np
from pymicro_features import MicroFrontend

# pymicro-features 2.0.x returns features already divided by 25.6 (the
# classic micro_speech float scaling); the firmware frontend hands us the
# raw uint16 values. Undo it or every model silently scores 0.0.
FEATURE_PRESCALE = 25.6

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # older envs
    from tflite_runtime.interpreter import Interpreter

SLIDING_WINDOW = 5          # firmware: mean of the last 5 probabilities
REFRACTORY_S = 2.0          # firmware: suppression after a detection
CUTOFFS = [0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 0.97, 0.99]


def wav_samples(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1, \
            f"{path}: need 16 kHz mono (got {w.getframerate()} Hz " \
            f"x{w.getnchannels()})"
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)


class Stream:
    """One streaming pass; mirrors firmware voice_infer.cc."""

    def __init__(self, model: str):
        self.itp = Interpreter(model_path=model)
        self.itp.allocate_tensors()
        self.itp.reset_all_variables()
        self.inp = self.itp.get_input_details()[0]
        self.out = self.itp.get_output_details()[0]
        self.stride = self.inp["shape"][1]      # feature frames per invoke
        self.frontend = MicroFrontend()
        self.probs: list[float] = []

    def run(self, pcm: np.ndarray) -> list[tuple[float, float]]:
        """Returns [(t_seconds, windowed_probability)] per invoke."""
        self.itp.reset_all_variables()
        self.frontend = MicroFrontend()
        self.probs.clear()
        out, buf, t = [], [], 0.0
        audio = pcm.tobytes()
        i = 0
        while i + 320 <= len(audio):          # 160 samples = 10 ms hops
            frame = self.frontend.process_samples(audio[i:i + 320])
            i += frame.samples_read * 2
            t += frame.samples_read / 16000.0
            if not frame.features:
                continue
            # firmware scaling: int8 = feature * 256/666 - 128
            feats = (np.array(frame.features, dtype=np.float32)
                     * FEATURE_PRESCALE * 256.0 / 666.0 - 128.0)
            buf.append(np.clip(feats, -128, 127).astype(np.int8))
            if len(buf) < self.stride:
                continue
            x = np.stack(buf)[None, ...]
            buf.clear()
            self.itp.set_tensor(self.inp["index"], x)
            self.itp.invoke()
            p = float(self.itp.get_tensor(self.out["index"])[0][0]) / 255.0
            self.probs.append(p)
            window = self.probs[-SLIDING_WINDOW:]
            out.append((t, sum(window) / len(window)))
        return out


def detections(trace: list[tuple[float, float]], cutoff: float) -> int:
    n, mute_until = 0, -1.0
    for t, p in trace:
        if p > cutoff and t >= mute_until:
            n += 1
            mute_until = t + REFRACTORY_S
    return n


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    model, pos_dir = sys.argv[1], Path(sys.argv[2])
    neg_dir = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    s = Stream(model)
    print(f"model {model}: stride={s.stride} frames/invoke")

    pos = sorted(pos_dir.glob("**/*.wav"))
    pos_traces = [s.run(wav_samples(p)) for p in pos]
    neg_traces, neg_hours = [], 0.0
    if neg_dir:
        for p in sorted(neg_dir.glob("**/*.wav")):
            samples = wav_samples(p)
            neg_hours += len(samples) / 16000.0 / 3600.0
            neg_traces.append(s.run(samples))

    print(f"\n{'cutoff':>7} {'detect%':>8} {'FA/h':>8}   "
          f"({len(pos)} positives, {neg_hours:.2f} h negatives)")
    for c in CUTOFFS:
        hit = sum(1 for tr in pos_traces if detections(tr, c) > 0)
        fa = sum(detections(tr, c) for tr in neg_traces)
        rate = f"{fa / neg_hours:8.2f}" if neg_hours else "     n/a"
        print(f"{c:>7} {100.0 * hit / max(len(pos), 1):>7.1f}% {rate}")


if __name__ == "__main__":
    main()
