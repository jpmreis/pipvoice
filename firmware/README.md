# Pip — ESP32-S3 Firmware

Target: Waveshare ESP32-S3-Touch-AMOLED-1.8 **V1** (SH8601 display,
FT3168 touch, ES8311 codec). ESP-IDF ≥ 5.4.

Board revisions: V1 (SH8601 + FT3168, discontinued) and V2 (CO5300 +
CST820, shipping since mid-2026). BSP ≥ 2.0.1 handles both — one CO5300
driver for either panel, touch chip auto-probed at boot — so this firmware
runs on a V2 unchanged, but V2 speakers are noticeably quieter (Waveshare's
own demos default volume 90 on V2 vs 70 on V1).

Stay on ESP-IDF 5.4.x for now: 5.5.x has an open I2S/codec regression that
makes the ES8311 mic return silence on this board
(espressif/esp-idf#18621).

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

Dependencies resolve automatically: the Waveshare BSP
(`waveshare/esp32_s3_touch_amoled_1_8`, providing display/touch/LVGL/codec
init), `joltwallet/littlefs`, and LVGL 9 come from the ESP component
registry; libopus 1.4 builds from the git submodule at
`components/opus/opus` (clone the repo with `--recurse-submodules`).

## Flash a new device

The normal path needs no toolchain at all: PWA → Settings → **"Set up a
new Pip"** provisions the device and flashes a blank board from
Chrome/Edge over Web Serial. The manual path below is for development.

Provision the device in the server admin (gives you an NVS CSV), then:

```bash
./flash_device.sh pip-ella-01.csv /dev/ttyACM0
```

That generates the NVS partition (identity + secrets), builds, and flashes
firmware + identity. One firmware build serves every device.

## Architecture

```
main/
  app_main.c        boot orchestration; UI<->module wiring (UI_LOCKED)
  config.c/.h       NVS identity/secrets/tunables; salted-SHA256 PIN
  board.c/.h        BSP wrapper, brightness, AXP2101 battery gauge
  storage.c/.h      LittleFS inbox/outbox + oldest-heard eviction
  opus_file.c/.h    .vmsg container: framed Opus, 16 kHz mono, 20 ms
  audio.c/.h        record/playback task over esp_codec_dev (ES8311)
  net_wifi.c/.h     STA manager, backoff reconnect, SNTP
  net_check.c/.h    internet-reachability probe after GOT_IP (detects
                    hotel captive portals - IP != online)
  net_http.c/.h     REST: contacts/upload/inbox/download/ack/delete,
                    reactions, theme assets
  net_mqtt.c/.h     mqtts:// notify subscription + presence publish
  sync.c/.h         outbox drainer + inbox fetcher (offline-first,
                    contacts cached to flash, theme thumbs to LittleFS)
  theme.c/.h        background themes: versioned download, PSRAM decode
  voice.c/.h        voice-control flow (accessibility): wake -> offer new
                    messages -> cycle contacts -> confirm -> record/send;
                    server-set flag, spoken prompts from /data/prompts
  voice_infer.cc/.h microWakeWord streaming inference (esp-tflite-micro +
                    micro-speech frontend), energy VAD; audio-task only
  voice_model_*.c   embedded wake/"yes" models - regenerate with
                    tools/wakeword/tflite_to_c.py, never edit
  provisioning.c/.h SoftAP + DNS hijack + captive portal (portal.html),
                    reboots after 10 idle minutes (portal activity re-arms)
  power.c/.h        inactivity dim/off, battery polling
  ota.c/.h          esp_https_ota against the server manifest - daily
                    check, kicked instantly by the MQTT firmware notify
                    and on wake-from-off
  lv_mem_psram.c    LVGL heap in PSRAM
  ui/               LVGL 9 screens: home, record, inbox, playback,
                    reactions, pinpad, settings, theme picker, wifi setup,
                    voice status, offline nag (home-only overlay after
                    60 s without WiFi)
```

Tasks: BSP LVGL task · audio (prio 6, static stack) · sync (4, core 0) ·
buttons (3) · power (2) · ota (1); net_check and the provisioning DNS
task are transient (prio 3).
UI access only via the LVGL lock; module→UI events are marshalled in
app_main.c. Messages and contacts persist in LittleFS, so the device is
fully usable offline.

## Version / releases

`PROJECT_VER` in the top-level CMakeLists is the system-wide version —
OTA compares it against the server's active firmware row. Releases are
git tags `v<PROJECT_VER>`: CI builds the tag and publishes the flash
bundle + manifest as a GitHub Release (see `CHANGELOG.md` and
`.github/workflows/release.yml`), which server admins pull from the
Firmware page. Local builds are for development; upload them manually.

## Bringing up an additional board

The firmware is proven on production V1 units; for a fresh board:

1. Flash Waveshare's prebuilt test firmware first to verify the unit.
2. Provision it in the admin and flash with `flash_device.sh` — no code
   changes needed; identity lives entirely in NVS.
3. If audio levels feel off (esp. on a V2 board), tune MIC_GAIN_DB in
   audio.c (default 24 dB) and the speaker volume — there is no NVS knob
   for gain.
4. Battery is optional: devices are designed to live on the charger, and
   the battery UI hides itself when no cell is detected.

Internal RAM is tight: LVGL's heap and theme assets live in PSRAM
(`lv_mem_psram.c`), and mbedTLS buffers are PSRAM-backed via sdkconfig.
If you touch task stacks or TLS settings, watch the boot heap ledger
logged at startup.
