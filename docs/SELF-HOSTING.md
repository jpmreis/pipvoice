# Self-hosting Pip

Run the whole Pip system — server, PWA, device fleet — on hardware you
control. Your family's voices never leave your server.

## The one hard requirement: HTTPS on a real domain

**A bare IP address will not work.** This isn't policy, it's how the
platform pieces work:

- **PWA install** (Add to Home Screen, web push) requires a secure
  context — browsers only grant it to `https://` origins.
- **The web flasher** uses Web Serial, which also requires a secure
  context.
- **The boxes validate TLS certificates** against the public CA bundle
  for both HTTPS and MQTT — a self-signed cert or plain HTTP is
  rejected by the firmware.

So you need a domain (a free subdomain of anything you own works) with
an A record pointing at your server, and a certificate. The stack below
gets the certificate for you via Let's Encrypt. If your server can't
take inbound traffic on ports 80/443 (CGNAT, home network you can't
port-forward), a **Cloudflare Tunnel** works for the web side — but
note the boxes also connect directly to port **8883** (MQTT-TLS) for
instant push; without it they still work, falling back to a 15-minute
sync poll and syncing after every user action.

Everything else is small: any €5/mo VPS or a Raspberry-Pi-class machine
at home is plenty for a family.

## Option A: Docker Compose (recommended)

Three containers: the app, Mosquitto (device push), Caddy (TLS).

```bash
git clone --recurse-submodules https://github.com/jpmreis/pipvoice.git
cd pipvoice
cp .env.example .env        # set DOMAIN and MQTT_SERVER_PASS
docker compose up -d
```

Wait a minute for Caddy to obtain the certificate (the mosquitto
container waits for it, then borrows it for MQTT-TLS and follows
renewals automatically). Then open `https://your.domain/admin`.

Data lives in named volumes (`pip-data` holds the SQLite DB, audio,
firmware, themes, and the VAPID key — that volume is the thing to back
up).

## Option B: bare VPS, no Docker

`server/SETUP.md` walks through the same stack installed directly on
Ubuntu 24.04 with one script (`server/scripts/setup.sh`). Same
requirements, same result.

## First run

1. `https://your.domain/admin` — the first visit creates **your admin
   account**. With local auth on (the compose default), you pick a
   password right there: no email round-trips, no SMTP.
2. *Users* — add a user per family member. Set each phone user's
   **local password** on the Users page (or configure `PIP_SMTP_*` and
   let them sign in with emailed codes — both work; see
   `server/env.example`).
3. *Permissions* — tick who can talk to whom.
4. *Firmware* — press **Fetch latest release**: the server pulls the
   newest published build from this repo's GitHub Releases
   (sha256-verified). **Activate** it.
5. **Flash a box** — from a desktop Chrome/Edge: PWA → Settings →
   "Set up a new Pip". The flasher writes your server's URL into the
   device; the **Server URL** field is prefilled with the site you're
   on, so flashing from your self-hosted app points the box at your
   server automatically.

## Living without SMTP

Local auth (`PIP_LOCAL_AUTH=1`) removes the only *required* use of
email: sign-in. What you give up without SMTP is optional comfort:
emailed install instructions, the "you have an unheard message" nudge
for phone users, and code-based sign-in. Web push notifications work
regardless — they don't use email.

## Firmware updates

Your server checks this repo's GitHub Releases only when you press
Fetch, verifies hashes, and installs inactive — you choose when to
activate (that's what rolls out to your boxes, over the air). Building
your own firmware instead: `firmware/README.md`, then POST the build to
`/admin/firmware`, or point `PIP_RELEASE_MANIFEST` at your own fork's
releases.

## Notes & limits

- **2.4 GHz WiFi only** — the ESP32-S3 has no 5 GHz radio.
- **One server process** by design (family scale); don't scale workers.
- **Backups**: the `pip-data` volume (or `/opt/pipvoice/data` on bare
  installs). `server/scripts/backup.sh` shows what matters: the DB,
  audio, env/secrets, `vapid_private.pem` (regenerating that key kills
  every push subscription).
- **Privacy page**: `/privacy` describes the hosted pipvoice.com
  deployment's vendors; edit `server/app/web/privacy.html` to match
  your setup.
- Coming from Pip Cloud, or moving servers? Devices must be re-flashed
  to point at the new server (Settings → "Flash an existing Pip"
  re-keys and re-flashes them; the flasher's Server URL field is where
  the new address goes).
