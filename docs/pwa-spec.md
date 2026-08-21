# Pip PWA — phone-user client spec

Status: **built and live** — agreed 2026-08-11, shipped 2026-08-15, in
production at `/app` since. Kept as the design reference; the "as built"
sections at the bottom record where the implementation went beyond this
spec.

## Locked decisions

- A user is **either** a device user **or** a phone user, never both. Derived from
  data, no schema flag: user has `devices` rows → device user. Notification routing
  keys off this.
- Phone client is a **PWA** served from the Pip VPS at `/app` — no native app, no
  Apple developer account, no third-party notification service.
- Notifications: **web push** (VAPID, installed-PWA standalone mode on iOS 16.4+)
  with **email fallback ladder**. ntfy/WhatsApp/SMS/Telegram/Signal were evaluated
  and rejected (cost, fragility, or Safari-tab-only UX).
- Branding/typography identical to firmware (tokens below).
- Phone recording cap mirrors device: 90 s default (`g_cfg.max_message_s`,
  `config.c:51`), server-enforced via `PIP_MAX_MSG_S`.

## Server side

1. **Cookie sessions** — login is passwordless since 2026-08: the user enters
   their email (`/v1/auth/request-code`) and redeems the emailed 6-digit code
   (`/v1/auth/verify-code`, 10-min/single-use/5-attempt), which sets the session
   token as `pip_session` cookie (HttpOnly, Secure, SameSite=Lax);
   `require_auth` accepts cookie or Bearer. Sliding expiry: when a session
   authenticates with <15 d left, extend to 30 d. Solves: Safari 7-day
   script-storage purge (server-set cookies exempt), `<audio src>` auth,
   never-relogin.
2. **Audio in (transcode)** — `POST /v1/messages` keeps the `b"VMSG"` sniff as the
   device fast path; other content (iOS Safari MediaRecorder = AAC/MP4, Android/
   desktop = WebM/Opus) is transcoded: ffmpeg → 16 kHz mono s16 PCM → `opuslib`
   encode matching firmware encoder exactly (16 kbps, VOIP, complexity 3, 20 ms /
   320-sample frames, `opus_file.c:27-31`) → VMSG container
   (`"VMSG" | u16 ver=1 | u16 sr/100=160 | u16 frame_ms=20 | u16 duration_s`,
   frames as `u16 len | packet`). Duration computed server-side from PCM length;
   reject > `PIP_MAX_MSG_S` (default 90). Keep 4 MB byte cap.
   New deps: `ffmpeg` binary on VPS, `opuslib` in requirements.
3. **Audio out** — `GET /v1/messages/{id}/audio.wav`: opuslib-decode frames → WAV
   (16 kHz mono s16). Devices keep the raw `.vmsg` endpoint.
4. **Push plumbing** — table
   `push_subs(endpoint TEXT PRIMARY KEY, user_id INT REFERENCES users, p256dh TEXT,
   auth TEXT, created TEXT, last_ok TEXT)`;
   `POST /v1/push/subscribe` (upsert, idempotent), `POST /v1/push/unsubscribe`.
   VAPID keypair auto-generated into the data dir (`PIP_VAPID_PEM` to
   relocate it, `PIP_VAPID_SUBJECT=mailto:...`).
   New dep: `pywebpush`.
5. **Notification ladder** — extract `notify.py`; `send_message` calls it instead of
   `mqtt.notify_user` directly:
   - recipient has devices → `mqtt.notify_user()` (unchanged), stop.
   - else (phone user): web-push every `push_subs` row. `WebPushException` with
     404/410 → delete row (definitive subscription-expired signal); 429/5xx =
     transient, keep row. If 0 pushes were accepted → **immediate email**.
   - `cleanup_loop` addition: phone-user messages with `delivered=0` and no
     reminder sent, older than `PIP_REMIND_H` (default 12 h) → one reminder email;
     add `messages.reminded INTEGER DEFAULT 0`.
   - Email: stdlib `smtplib`; env `PIP_SMTP_HOST/PORT/USER/PASS/FROM`. New nullable
     `users.email` column, editable in admin. No email on file → skip that rung.
6. **Static serving** — `/app` (single page), `/app/sw.js` (scope `/app/`),
   `/app/manifest.json`, `/app/icons/*`, `/app/fonts/*`. No Caddy changes.
7. **Admin** — user form gains optional email; a "phone user" is simply a user with
   no device provisioned; list view shows kind (device id vs "phone").

## Web app (single page, vanilla JS — match project's no-framework ethos)

- **Screens**: Login (email → code) → Home (contact grid using
  `users.color` dots + inbox with unheard badge) → Record (tap contact: round
  amber hold-to-record button mirroring device UX, live timer against 90 s cap;
  release → preview with play / re-record / send) → message rows play inline
  (`/audio.wav` in `<audio>`; POST `/ack` on first play), delete behind confirm.
- **Capture**: MediaRecorder, `audio/mp4` on iOS / `audio/webm;codecs=opus`
  elsewhere; upload blob as-is + client-measured duration (server recomputes).
- **Service worker**: `push` → show notification (payload `{sender_name, msg_id}`);
  `notificationclick` → focus or open `/app`; light network-first cache of shell,
  fonts, icons. Push is the SW's job; offline support is incidental.
- **Subscription self-heal**: every launch, `pushManager.getSubscription()`,
  re-subscribe if missing/changed, POST to server. (`pushsubscriptionchange` is
  unreliable on iOS — launch-time sync is the mechanism.)
- **Install / onboarding UI** (revised 2026-08: install moved out of the app):
  - Install instructions live on a public page, `/install` →
    `/app/install.html` (in-scope so Chrome's native `beforeinstallprompt`
    works; iOS Safari = numbered Share → "Add to Home Screen" steps with
    inline SVG glyphs, Pip-branded). The admin's "Send install email" button
    mails a branded invite linking there; the app itself never gates on
    install.
  - Standalone and `Notification.permission === "default"` → "Turn on
    notifications" screen, amber button (iOS requires user gesture);
    browser-mode users go straight to Home.
  - Settings screen: notification status + re-test, link to `/install`, logout.
- **Branding — must match firmware** (`ui_internal.h` tokens → CSS custom props):

  | token | value | use |
  |---|---|---|
  | `--bg` | `#000000` | page background (true black, matches AMOLED) |
  | `--surface` | `#16161C` | cards/rows |
  | `--surface2` | `#24242E` | toasts, inputs |
  | `--text` | `#F2F0EA` | primary text |
  | `--text-dim` | `#8B8B96` | meta text |
  | `--accent` | `#FFB300` | record/action (warm amber) |
  | `--ok` | `#3DDC84` | send/online |
  | `--danger` | `#FF5252` | delete/recording dot |

  Font: **Montserrat**, self-hosted woff2 in `/app/fonts` (no CDN). Size scale
  mirrors firmware: small 16 / body 20 / title 28 / big 40 px. Buttons: radius 16,
  height 64; round record button; toasts: surface2, radius 14, 2 px status-color
  border, top-center, tap/swipe-up dismiss (mirror `ui_toast`). Fixed dark theme
  only, `meta theme-color #000000`. App icon: amber-on-black Pip mark, Montserrat
  wordmark — generate 512/192 + 180 apple-touch + maskable variants.

## Interop with Waveshare devices

- **Zero firmware changes.** Phone→box rides the exact device path (insert + MQTT
  notify; box can't distinguish senders). Box→phone: `mqtt.notify_user` no-ops
  (no devices rows) and the push/email ladder fires.
- Contacts already interop: `/v1/contacts` aliases username as `device_id`
  (`api.py:66-68`); web client sends the same field to `recipient_id`.
- Either/or user model kills the shared-inbox reconcile concern: each inbox has
  one client class. Device `sync.c` behavior untouched.
- **Fidelity gate**: transcoded VMSG must decode on-box (16 kHz mono Opus, 20 ms).
  Bench test = phone → real box, listen; box → phone, check push + WAV playback.

## Build order

1. Cookie auth + sliding sessions.
2. Transcode in/out + VMSG packer; pytest against a firmware-recorded `.vmsg`.
3. Push: table, endpoints, VAPID, `notify.py` ladder with 410-pruning + email.
4. PWA shell: page, tokens/fonts, SW, manifest, icons.
5. Onboarding/install UI + subscription self-heal + settings.
6. Reminder net in `cleanup_loop`; extend `smoke_test.py`
   (login → send AAC blob → inbox → WAV → ack → delete; push-sub CRUD; 410 prune
   via fake endpoint).
7. Bench test with both boxes.

## Open items

- ~~Generate app icon assets~~ done — shipped in `app/web/icons/`.
- ~~Vendor Montserrat woff2~~ done — 400/600/700 in `app/web/fonts/`.
- ~~SMTP creds in production~~ done — the email fallback rung is live
  (`PIP_SMTP_*`; the ladder skips it while unset).

## Global versioning (as built, 2026-08-16)

One version covers the whole fleet, stored in exactly **one variable**:
`PROJECT_VER` in `firmware/CMakeLists.txt`. It is baked into the firmware
binary, lands in the `firmware` DB table on upload, and the **active row** is
what every client checks. There are no per-half hashes: a web-only release
still ships as a version bump + firmware upload/activate — one variable, one
bump event, both platforms refresh together.

- `GET /v1/version` (unauthenticated) returns `{version}`; every `/v1/*`
  response also carries the string in an `X-Pip-Version` header.
- **Devices** keep their existing flow: daily manifest check + `ota_kick()` on
  wake-from-off/interaction, and the MQTT notify on activation — which now
  includes the version: `{"event":"firmware","version":"0.1.22"}` (older
  firmware ignores the extra field).
- **PWA** baselines the version at boot and hard-refreshes (SW update + cache
  purge + reload) when it changes, detected via: visibilitychange (the wake-up
  path), a 60 s foreground poll, or the header on any API interaction. The
  header is the browser-side stand-in for MQTT: browsers can't hold an MQTT
  subscription in the background (iOS freezes background tabs), so a push
  channel would add nothing over the visibility check — a deliberate choice.
  The refresh defers while the record screen is open so it never discards a
  take. Settings shows the current string. `/v1/webver` is deprecated (now an
  alias for the same single version) but kept so older shells still see the
  value change on the next activation and reload into the new scheme.

## Theme thumbnails + asset versioning (as built, 2026-08-16)

Both pickers show thumbnails, and both clients cache theme images keyed by a
per-theme version — the 8-hex sha256 of the master image, exposed as `ver` in
`GET /v1/themes` (and `theme_ver` in `/v1/me` + login). Server pre-renders
`<name>-thumb.bin` (108×132 raw RGB565LE, the device grid size) next to the
existing device/web variants; served at `/v1/themes/{name}/thumb.bin`.

- **Caching rule** (all three theme assets): a request with `?v=` matching the
  current hash gets `Cache-Control: immutable` — safe because changing a
  master flips the hash, so clients switch URLs. Any other request gets
  `no-cache` (cheap ETag 304 revalidation), so version-less clients never go
  stale. This is what stops the PWA re-downloading thumbs on every settings
  visit.
- **Device** (fw 0.1.22): background picker is a standalone screen entered
  from Settings → Background — a 3-column thumbnail grid ("None" first, amber
  ring on the active pick), mirroring the PWA's. The sync task (single-TLS
  rule) downloads thumbs to LittleFS `/data/thumbs/<name>-<ver>.bin` — the
  version-in-filename makes staleness a `stat()`, no conditional HTTP — and
  prunes files that stopped matching the server list. The picker loads each
  thumb once into a kept PSRAM slot (~28.5 KB per theme, never internal RAM);
  until a thumb lands the tile falls back to the theme's label.
