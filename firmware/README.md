# Pip — ESP32-S3 Firmware

One codebase, three Waveshare ESP32-S3 Touch AMOLED boards. The board
is chosen at build time with `-DPIP_BOARD=<key>` (keys match the
server's `boards.py`), which selects the UI geometry block in
`main/ui/ui_geometry.h`, the BSP manifest, and `PIP_BOARD_NAME`
(printed in the boot `PIP-HW` line and checked by the web flasher).

| `PIP_BOARD` | Board | Panel | Display / touch | Mic | ESP-IDF |
|---|---|---|---|---|---|
| `amoled-1.8` (default) | ESP32-S3-Touch-AMOLED-1.8 | 368×448 rect | SH8601 (V1) / CO5300 (V2) via `esp_lcd_co5300`; FT3168 / CST820 auto-probed | ES8311 analog mic | **5.4.x** |
| `amoled-1.75b` | ESP32-S3-Touch-AMOLED-1.75-B | 466×466 round | CO5300; CST9217 | ES7210 dual digital array | **5.5.x** |
| `amoled-2.16` | ESP32-S3-Touch-AMOLED-2.16 | 480×480 square | CO5300; CST9220 | ES7210 dual digital array | **5.5.x** |

All three share the S3R8 (16 MB flash, 8 MB PSRAM), the ES8311 speaker
path, the AXP2101 PMU and the same partition table, so one NVS identity
layout and one OTA mechanism serve every model.

Why two IDF versions: the 1.75-B/2.16 BSPs have no release for IDF ≤ 5.4,
while IDF 5.5.x has an open I2S/codec regression that makes the ES8311
**analog mic** return silence (espressif/esp-idf#18621) — so the 1.8 stays
on 5.4.x. The new boards record through the ES7210 and are unaffected.

1.8 board revisions: V1 (SH8601 + FT3168, discontinued) and V2 (CO5300 +
CST820, shipping since mid-2026). BSP ≥ 2.0.1 handles both — one CO5300
driver for either panel, touch chip auto-probed at boot — so this firmware
runs on a V2 unchanged, but V2 speakers are noticeably quieter (Waveshare's
own demos default volume 90 on V2 vs 70 on V1).

## Build

Activate the matching IDF `export.sh` for the board, then:

```bash
idf.py set-target esp32s3
idf.py build                                               # amoled-1.8
idf.py -B build-amoled-1.75b -DPIP_BOARD=amoled-1.75b build
idf.py -B build-amoled-2.16  -DPIP_BOARD=amoled-2.16  build
```

Non-default boards get their own build directory and their own generated
`sdkconfig.<board>` (plus `sdkconfig.defaults.<board>` deltas where one
exists). Dependencies resolve automatically: the board's Waveshare BSP
(`waveshare/esp32_s3_touch_amoled_1_8` / `_1_75` / `_2_16`, providing
display/touch/LVGL/codec init), `joltwallet/littlefs`, and LVGL 9 come
from the ESP component registry; libopus 1.4 builds from the git
submodule at `components/opus/opus` (clone the repo with
`--recurse-submodules`).

Two generated-file gotchas: `main/idf_component.yml` is written per
board from the tracked `idf_component.<board>.yml` (the three BSPs
export identically named headers, so only one can be a dependency), and
the committed `dependencies.lock` belongs to the 1.8 manifest — building
another board rewrites it, `git restore dependencies.lock` afterwards.

## Flash a new device

The normal path needs no toolchain at all: PWA → Settings → **"Set up a
new Pip"** provisions the device and flashes a blank board from
Chrome/Edge over Web Serial. The manual path below is for development.

Provision the device in the server admin (gives you an NVS CSV), then:

```bash
./flash_device.sh pip-ella-01.csv /dev/ttyACM0
```

That generates the NVS partition (identity + secrets), builds, and flashes
firmware + identity. One firmware build serves every device of the same
model; the server records the model at provisioning and only ever serves
a box its own model's binaries (OTA and web flasher alike).

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
OTA compares it against the server's active firmware row for the box's
model. Releases are git tags `v<PROJECT_VER>`: CI builds the tag for all
three boards (1.8 on IDF v5.4.4, 1.75-B/2.16 on v5.5.5) and publishes
one GitHub Release with a flash bundle per board and a single
`manifest.json` listing a `builds[]` entry per model (see `CHANGELOG.md`
and `.github/workflows/release.yml`). Server admins pull it from the
Firmware page and activate each model independently. Local builds are
for development; upload them manually.

## Bringing up a new unit

The 1.8 has months of production use; the 1.75-B and 2.16 are
bench-verified (display, touch, mic, speaker, sync, voice control). For
a fresh unit of any model:

1. Flash Waveshare's prebuilt test firmware first to verify the unit.
2. Provision and flash from the PWA setup page (pick the model), or
   provision in the admin and flash with `flash_device.sh` — no code
   changes needed; identity (including the model) lives entirely in NVS.
3. **Cased boards (1.75-B, 2.16): check the speaker lead.** Units ship
   with the speaker unplugged inside the case — plug it into the header
   marked SPK (BAT is the battery) or playback is silent with no error.
4. If audio levels feel off (esp. on a 1.8 V2), tune MIC_GAIN_DB in
   audio.c (default 24 dB) and the speaker volume — there is no NVS knob
   for gain. The ES7210 boards average both mics and use a lower
   wake-word cutoff; both are per-board constants, not NVS.
5. Battery is optional: devices are designed to live on the charger, and
   the battery UI hides itself when no cell is detected.
6. The 2.16's Wi-Fi is ~20 dB weaker than the other two side by side
   (chip antenna shadowed by the big panel); place it near the router.

Internal RAM is tight: LVGL's heap and theme assets live in PSRAM
(`lv_mem_psram.c`), and mbedTLS buffers are PSRAM-backed via sdkconfig.
If you touch task stacks or TLS settings, watch the boot heap ledger
logged at startup.
