#!/bin/sh
# Embed freshly trained models into the firmware and rebuild.
# Usage: ./install_models.sh [wake_cutoff] [confirm_cutoff]
# Expects models/hey_pip.tflite and models/yes.tflite (from the Colab
# notebook's pip_voice_models.zip). Cutoffs default to 0.97 / 0.85 -
# pick better ones from validate.py's sweep if you have bench audio.
set -e
cd "$(dirname "$0")"

WAKE_CUTOFF="${1:-0.97}"
CONFIRM_CUTOFF="${2:-0.85}"

test -f models/hey_pip.tflite || {
    echo "models/hey_pip.tflite missing - unzip pip_voice_models.zip into models/"; exit 1; }

python3 tflite_to_c.py models/hey_pip.tflite wake "$WAKE_CUTOFF" \
    > ../../firmware/main/voice_model_wake.c
echo "wake    <- hey_pip.tflite (cutoff $WAKE_CUTOFF)"

if [ -f models/yes.tflite ]; then
    python3 tflite_to_c.py models/yes.tflite confirm "$CONFIRM_CUTOFF" \
        > ../../firmware/main/voice_model_confirm.c
    echo "confirm <- yes.tflite (cutoff $CONFIRM_CUTOFF)"
else
    echo "confirm: no models/yes.tflite - keeping the VAD fallback"
fi

echo
echo "Rebuilding firmware..."
cd ../../firmware
idf.py build | tail -3
echo
echo "Next: flash a bench box (idf.py flash) and try the wake word;"
echo "when happy: bump PROJECT_VER + CHANGELOG, commit, tag v<version>."
