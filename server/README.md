# Pip — Server

FastAPI + SQLite + Mosquitto behind Caddy. One process serves three
clients: device firmware (bearer tokens), the phone PWA (cookie
sessions), and the human admin (server-rendered pages). Setup guides:
`SETUP.md` (VPS), `SETUP-LOCAL-MAC.md` (local dev).

## Architecture

```
app/
  main.py       FastAPI wiring: /v1 API, /admin UI, /app PWA statics;
                startup = theme render, cleanup loop, presence
                subscriber, broker-ACL refresh
  db.py         SQLite schema + helpers; PIP_* env config; symmetric
                permissions re-asserted at every boot
  auth.py       email login codes, device bearer tokens, user session
                tokens (cookie or Bearer), rate-limited login
  api.py        /v1: contacts, messages (send/inbox/audio/ack/delete),
                reactions, presence, themes, push subs, device-admin
                contact management, firmware manifest, /version
  admin.py      /admin: users, devices (provision -> NVS CSV),
                permissions matrix, firmware fetch/upload/activate
  provision.py  NVS CSV/bin generation (device identity + secrets),
                web-flash bundle assets, one-shot flash-image stash
  release.py    "Fetch latest release": pull the published GitHub
                Release bundle, sha256-verified, installed inactive
  emails.py     branded transactional email (login codes, install
                invites, reminders) over PIP_SMTP_*
  notify.py     fan-out on new message: device user -> MQTT; phone
                user -> web push, else immediate email
  mqtt.py       publish notifies; manage mosquitto passwd/ACL
                (degrades to no-op on dev machines without a broker)
  push.py       pywebpush + auto-generated VAPID keypair; 404/410
                prunes the subscription
  presence.py   in-memory "X is recording to you" (MQTT from devices,
                POST /v1/presence from the PWA; 20 s TTL)
  cleanup.py    delete-on-delivery (audio goes ~24 h after the first
                ack; phone-user rows too, device-user rows stay for the
                box and age out at 30 d), undelivered give-up at 30 d,
                orphaned-audio sweep, reminder emails
  themes.py     ffmpeg-rendered background variants (device .bin RGB565,
                thumb .bin, web .jpg), content-hash versioned
  voice.py      spoken prompts for device voice control (accessibility):
                piper/espeak TTS -> .vmsg clips, content-hash versioned
                like themes; per-device flag lives on devices.voice
  vmsg.py       .vmsg container (mirrors firmware opus_file.c): browser
                audio -> ffmpeg -> opuslib -> VMSG; VMSG -> WAV out
  templates/    admin pages (Jinja2)
  themes/       theme master images (committed, ~17 MB)
  web/          the PWA: single-page vanilla JS (app.js), service
                worker (push + shell cache), manifest, icons, fonts
```

## Data directory (`PIP_DATA`, prod `/opt/pipvoice/data`)

`pip.db` (SQLite) · `audio/` (.vmsg blobs) · `firmware/` (fetched or
uploaded app .bins + web-flash bundle files) · `themes/` (rendered
variants) · `prompts/` (voice-control TTS clips) · `vapid_private.pem`
(auto-generated; regenerating invalidates every push subscription).

## Design constraints worth knowing

- **Single uvicorn worker, family scale**: presence and the login rate
  limiter are in-memory; the DB is single-writer. Don't add workers.
- A user is either a device user or a phone user (derived from
  `devices` rows) — that decides the notify path.
- One global version: the active `firmware` row is what `/v1/version`
  and the `X-Pip-Version` header report; activation MQTT-notifies boxes
  (retained) and makes PWAs self-refresh.
- Analytics (`stats.py`) has two tiers so cost stays flat as the family
  grows: `stats.event()` for things that happen (rows in `events`, kept
  `PIP_STATS_DAYS`), `stats.count()` for hot paths (in memory, flushed to
  `hourly` every minute, same retention), and an hourly rollup into
  `daily` (kept forever, bounded per day - per-user/per-device dims never
  reach it). Never log a secret or personal datum (codes, tokens,
  emails, IPs, raw paths); never call `event()` without `c=` from inside
  an open `db.conn()` block. Boxes report vitals in an `X-Pip-Diag`
  header on every request (`stats.parse_diag` is the contract).
- Permissions are symmetric pairs; never write one-way rows.
