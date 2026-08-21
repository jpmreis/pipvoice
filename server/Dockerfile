# Pip server image: FastAPI app + the tools it shells out to.
# ffmpeg: browser-audio transcode + theme rendering.
# mosquitto (package, daemon never started here): provides
# mosquitto_passwd, which the app uses to manage per-device broker creds.
FROM python:3.12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg mosquitto \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/pipvoice
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

ENV PIP_DATA=/data
EXPOSE 8080
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
