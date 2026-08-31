# Pip server image: FastAPI app + the tools it shells out to.
# ffmpeg: browser-audio transcode + theme rendering.
# mosquitto (package, daemon never started here): provides
# mosquitto_passwd, which the app uses to manage per-device broker creds.
# piper-tts (pip) + the voice model: renders the voice-control accessibility
# prompts (app/voice.py); deliberately not in requirements.txt - dev
# machines fall back to espeak-ng or run promptless.
FROM python:3.12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg mosquitto curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/pipvoice
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt piper-tts

# en_US-lessac-medium: clear neutral English; ~60 MB, fetched at build so
# the container needs no network at runtime
RUN mkdir -p voices && cd voices \
    && curl -fsSL -o en_US-lessac-medium.onnx \
       "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx" \
    && curl -fsSL -o en_US-lessac-medium.onnx.json \
       "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json"

COPY app ./app

ENV PIP_DATA=/data
ENV PIP_PIPER_VOICE=/opt/pipvoice/voices/en_US-lessac-medium.onnx
EXPOSE 8080
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
