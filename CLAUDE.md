# Pip — working notes for Claude

Family voice-message system, **in production for daily family use**. The
production server (pipvoice.com) **auto-deploys from `main`** — treat
every push to main as a production deploy of the server. One
version number for everything: `PROJECT_VER` in `firmware/CMakeLists.txt`.
Read the READMEs first (root, `firmware/`, `server/SETUP*.md`,
`docs/pwa-spec.md`); this file holds only what the docs can't show.
Deployment-specific operator notes live in `CLAUDE.local.md` (untracked).

## Toolchain

- ESP-IDF **v5.4.4** (e.g. at `~/esp/esp-idf-v5.4`); activate with
  `. ~/esp/esp-idf-v5.4/export.sh`, then `idf.py build` in `firmware/`.
- IDF 5.3 cannot resolve deps (BSP ≥2.0.1 needs esp_lcd_co5300 2.x → IDF
  ≥5.4). IDF 5.5.x has an ES8311 mic-silence regression — stay on 5.4.x.
- `firmware/dependencies.lock` is committed on purpose (reproducible builds).
- `firmware/sdkconfig` is **gitignored**; only `sdkconfig.defaults` is
  tracked. Any deliberate setting must go in sdkconfig.defaults or it
  silently vanishes on regeneration.
- opuslib encoder CTLs fail on arm64 macOS (ctypes variadic ABI) but work
  on x86_64 Linux — don't chase that locally.
- `firmware/.clangd` needs its `-isystem` toolchain path pointed at your
  own `.espressif` install (machine-specific; keep that edit local).

## Releases

- **Normal path (since CI releases landed)**: bump `PROJECT_VER` + add
  the CHANGELOG section → commit → `git tag v<version> && git push
  origin v<version>`. CI (`.github/workflows/release.yml`) verifies
  tag == PROJECT_VER, builds on IDF v5.4.4, and publishes a GitHub
  Release: `pip-<ver>.bin` + bootloader + parttable + `manifest.json`
  (ESP Web Tools-superset with sha256 per part;
  `firmware/tools/make_manifest.py`). Admin Firmware page → **Fetch
  latest release** (hash-verified, installs inactive;
  `server/app/release.py`, `PIP_RELEASE_MANIFEST` for forks) →
  **Activate**. Activation publishes a **retained** QoS1 MQTT notify:
  online boxes update in seconds, offline boxes on reconnect.
- Dev fallback: `POST /admin/firmware` (multipart: version, notes,
  file, bootloader, parttable, activate) — the admin page has no upload
  form anymore. A complete web-flash bundle needs the bootloader +
  partition-table files (the flasher falls back to the newest version
  that has them); CI bundles are always complete.
- The old hazard this replaces: a local dirty-tree build once shipped
  as a release; phantom bugs followed. CI builds from the clean tag by
  construction — don't hand-build releases anymore.

## Firmware engineering rules (learned the hard way)

- **Internal RAM is the scarce resource.** UI/theme data goes to PSRAM
  (LVGL heap is PSRAM via `lv_mem_psram.c`); task stacks and WiFi DMA must
  stay internal — keep new task stacks static if internal, and never leave
  an `xTaskCreate` return unchecked (a failed audio/ota task once died
  silently). `app_main` prints a per-phase boot heap ledger; healthy
  baseline ≈ 90 K internal free at "boot complete" (31.7 K largest block).
  Compare after any change adding tasks/buffers/UI.
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` (PSRAM) is required — the IDF
  default put every TLS connection's ~20-40 KB in internal RAM and HTTPS
  handshakes failed next to MQTT (`PK verify 0x4290` = alloc failure).
- **One TLS connection at a time**: any new TLS work must run inside the
  sync task (theme downloads learned this — a parallel TLS task starved
  mbedtls even after the PSRAM fix).
- Audio: chimes are pre-rendered into PSRAM before the codec opens (live
  synthesis raced the 20 ms deadline under WiFi bursts → garble). Chime
  synth exists twice — `audio.c chime_note` and `app.js pipStrike` — keep
  them in sync. Mic chain: 24 dB analog + ×3 digital, limiter knee
  3:1@22000; if recordings are too quiet, bump `REC_DIGITAL_GAIN`, never
  analog (36 dB clipped the ADC on close speech). Clipping diagnostic:
  decode VMSGs server-side, histogram amplitudes — pile-up near ±31 k =
  ADC clipping.
- Buttons: BOOT (top) = record-to-recent; AXP2101 power key (bottom) =
  inbox; key events are the **lower** nibble of INTSTS2.
- Voice control (accessibility): wake/"yes" models are embedded C arrays
  — regenerate via `tools/wakeword/tflite_to_c.py`, never hand-edit; the
  wake-word listener lives INSIDE the audio task (one ES8311, one I2S
  duplex pair — mic and speaker must never be open together, and a
  second TLS/task would break the established rules). Prompt keys
  (`ask_play`, `ask_confirm`, `cancelled`, `ask_send-<username>`) are
  firmware API shared with server voice.py — rename in both or clips
  stop resolving. Wake model is a hey_jarvis stand-in until "hey pip"
  is trained (tools/wakeword/README.md).
- Timezone comes from `ip-api.com/json` over plain http after WiFi connect
  (worldtimeapi silently ignores http). POSIX TZ sign is inverted and the
  zone name needs ≥3 chars ("LOC-1:00" works, "LT-1" silently keeps UTC).
- Closing the USB-JTAG serial port can hard-reset the board (a killed
  capture script = surprise reboot = surprise OTA check). An unread USB
  console can also block panic dumps — always capture during soaks.

## Protocol / data invariants

- Permissions are **symmetric**: both direction rows stored; `db.init()`
  re-symmetrizes idempotently at boot. Never write one-way rows.
- Reaction keys are ASCII (`heart up down haha bang quest joy`); glyphs
  are per-client UI maps. The `reactions` table has **no FK to messages on
  purpose** — reactions outlive message deletion (unseen rows never
  expire; seen rows purge after 30 d).
- Device `.meta` files are 8 lines (line 6 sender_id, 7 UTC ts, 8 own
  reaction).
- MQTT notify topic is shared: the firmware-update notify is **retained**
  (offline boxes OTA on reconnect); the `{"event":"contacts"}` and
  `{"event":"voice"}` notifies **must stay non-retained** or they replace
  the retained firmware notify and break OTA-on-reconnect.
- **A notify is a promise the message is fetchable now.** Nothing
  announces before the bytes the client will ask for exist: the audio is
  written before the row is inserted, the phone's `.m4a` is rendered
  before the push, and `POST /v1/messages` hands the notify to a
  background task so a single-worker server isn't blocked serving the
  recipient it just woke. Adding a notify path means keeping that order.
- The new-message notify to a **box** carries `msg_id/from/from_name/
  color/ts/dur` so `sync.c` can fetch the audio without listing the
  inbox first (one TLS handshake instead of two — that is most of
  delivery latency on weak wifi). The firmware reads it into a fixed
  384 B buffer; keep the payload well under that. Truncation isn't
  fatal — the parse fails and sync falls back to `GET /v1/inbox` — but
  it costs the fast path. Old firmware ignores the extra keys.
- A box chimes when the message is **on its flash**, not when the ack
  lands (`deliver_one` in sync.c). Don't put network calls back in front
  of `s_ev.new_message`.
- Message audio exists twice on the server: `<id>.vmsg` (what boxes
  download, authoritative) and a derived `<id>.m4a` (what browsers
  play, ~8x smaller than the WAV path it replaced). They live and die
  together — `db.drop_audio()` is the only correct way to delete either,
  since a stray `.m4a` is a privacy leak, not a cache hit. `audio.wav`
  stays served for phones running an older cached `app.js`.
- A user is either a device user or a phone user, never both — derived
  from `devices` rows, no schema flag.
- Auth modes: `PIP_LOCAL_AUTH=1` (self-host) enables scrypt local
  passwords and stops the boot-time blanking of `password_hash`;
  without it login is email-code only and hashes are blanked at boot.
  The production deployment stays passwordless — never set it there.
- **Boxes mirror the server inbox** (`sync.c` reconcile drops any local
  message missing from `/v1/inbox`), so privacy delete-on-delivery
  (cleanup.py) removes only the *audio* for device recipients and keeps
  the row until the 30 d age-out; phone-user rows are deleted whole,
  ~24 h (`PIP_DELIVERED_GRACE_H`) after the first ack — the grace lets a
  second PWA on the same account fetch it. Never delete a
  device-recipient row early or the box's copy vanishes on next sync.

## NVS / provisioning gotchas

- **Web flashing is the normal provisioning path** (since 0.1.29): PWA
  Settings → "Set up a new Pip" → `/app/setup.html` (Chrome/Edge
  desktop, Web Serial, vendored esptool-js). Creates the device user +
  device (creator becomes device admin + first contact), generates the
  NVS bin server-side (`provision.py`, esp-idf-nvs-partition-gen — new
  deps need the pip-install step in SETUP.md §5), flashes
  bootloader/parttable/otadata/NVS/app. Re-key
  (`/v1/setup/{id}/rekey`) rotates the token — the box is offline until
  reflashed. Hard-won details: esptool-js `after("hard_reset")` is a
  no-op on USB-Serial-JTAG (setup.js pulses RTS itself), and any chip
  reset re-enumerates the in-chip USB device (verify must reattach, not
  reuse the port).
- Rewriting the NVS partition **erases the `wifi_nets` blob** of learned
  networks. Firmware ≥0.1.29 auto-opens the WiFi-setup QR at zero
  networks, so a wiped box self-recovers via phone; no WiFi creds are
  baked anywhere. On firmware <0.1.29 the box just sits on "connecting".
  `wifi_cred_t` = 33+65 B packed, most-recent-first, entry 0 active.
- Box rename recipe that worked: UPDATE `devices.id`, provision new /
  revoke old broker creds, regen CSV (new device_id/name/mqtt_user,
  token+PIN unchanged), `nvs_partition_gen` 0x6000, `esptool write_flash
  0x9000`. The identity stamp (`/data/.owner`) wipes local cache by
  design; the box re-syncs.
- `idf.py flash` does **not** touch the NVS partition — identity survives
  firmware reflashes.
- Box greeting names live in NVS `device_name`; DB renames don't reach
  them (needs reprovision or a future config push).
- Wiping a device "from scratch" must also purge server-side state
  (`messages` rows + audio files), or old messages re-deliver as a chime
  storm.

## Local bench testing

- Local server: see `server/SETUP-LOCAL-MAC.md`. Devices skip MQTT when
  `mqtt_host` is empty in NVS (strip the CSV line); they sync on a 15-min
  poll or after any user action. The LAN IP is baked into NVS — Mac IP
  change ⇒ re-provision.
- A "virtual peer" is just curl with a provisioned device's bearer token;
  an echo bot (bounce messages back) covers solo receive-path testing.
- Device→server presence trust (broker ACL) is only testable with a real
  broker + valid TLS; locally simulate with
  `mosquitto_pub -t 'presence/pip-x-01' ...`.
- ESP32-S3 is 2.4 GHz-only: iPhone hotspots need "Maximize Compatibility"
  and stop beaconing when idle (keep the hotspot screen open).

## Backlog / accepted gaps

Someday (agreed, not planned): firmware signing; config-push (would fix
domain migration and NVS `device_name` renames without reflash); off-box
backups; a mosquitto `.path` unit to replace the sudo reload; SQLite
session-row cleanup (now also stale `login_codes` rows); vestigial strstr
branch in `net_http.c`; admin edit-user form (email is editable per-row
since the passwordless change; display name/color still SQL-only).

Accepted: admin-bootstrap-on-empty-DB race on reinstalls; in-memory rate
limiter + presence assume a single uvicorn worker (by design, see
server/README.md); hard-walled captive portals fail open to "online".
