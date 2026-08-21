# Pip — VPS Setup Guide (Hetzner)

One small server runs everything: the API + admin site (FastAPI), the MQTT
broker (Mosquitto), and the TLS front (Caddy + Let's Encrypt).

## 1. Create the server

1. Sign up at https://console.hetzner.cloud, create a project.
2. Add your SSH public key under *Security → SSH keys*.
3. *Add server*: location near you (e.g. Falkenstein/Ashburn), image
   **Ubuntu 24.04**, type **CX22** (2 vCPU / 4GB / €4-5 mo — already
   oversized for this), select your SSH key. Create.
4. Note the public IPv4 address.

## 2. DNS

At your DNS provider, add an **A record**:
`voice.yourdomain.com → <server IPv4>` (TTL 300 is fine).
Wait until `ping voice.yourdomain.com` resolves before continuing —
Let's Encrypt needs it.

## 3. Install

```bash
ssh root@voice.yourdomain.com
# copy this server/ directory to the machine first, e.g.:
#   scp -r server root@voice.yourdomain.com:/root/
cd /root/server/scripts
./setup.sh voice.yourdomain.com you@yourdomain.com
```

The script installs packages, creates the `pipvoice` service user, sets up the
Python venv, obtains the TLS certificate (shared by HTTPS and MQTT-TLS via
a renewal hook), configures Mosquitto (local listener for the app, TLS
listener :8883 for devices, per-device credentials + ACL), enables the
firewall (22/80/443/8883), and starts everything under systemd.

## 4. First login & family setup

1. Open `https://voice.yourdomain.com/admin` → first visit asks you to
   **create the admin account** (username + email — sign-in codes go to
   that address, so set up SMTP in §5 before you sign out).
2. *Users*: create one user per family member (display name + avatar color
   are what appear on the devices; the email address is how phone users
   sign in to the PWA at `https://voice.yourdomain.com/app` — they enter
   it and receive a 6-digit code, no passwords anywhere). The **Send
   install email** button mails a user the branded setup instructions
   (public page: `/install`).
3. *Permissions*: tick who may send to whom (n-to-n matrix, symmetric —
   e.g. everyone ↔ everyone, or kids can only reach parents/grandparents).
   A device can also get a *device admin*: a phone user who manages that
   box's contact list from the PWA without needing the admin site.
4. *Devices* — the easy path needs no toolchain: any phone user opens
   the PWA on a desktop Chrome/Edge → Settings → **"Set up a new Pip"**,
   plugs in a blank board, and the browser provisions + flashes it over
   Web Serial (requires an active firmware with a complete flash bundle,
   see step 5). Manual alternative: provision in the admin (id like
   `pip-ella-01`, owner, settings PIN — the result page shows the API
   token + MQTT password **once** and a ready **NVS CSV**), then on your
   dev machine:

   ```bash
   cd firmware
   ./flash_device.sh pip-ella-01.csv /dev/ttyACM0
   ```

   That generates the NVS partition, builds, and flashes firmware +
   identity in one go. Identity and secrets live in NVS, so one firmware
   build serves all devices.
5. *Firmware*: **Fetch latest release** pulls the newest published
   build (app + bootloader + partition table, sha256-verified) from the
   project's GitHub Releases. Mark it **active** — an MQTT notify
   updates online devices within a minute; offline devices catch up via
   the daily check on reconnect. The same activation makes installed
   PWAs refresh themselves. (Developers shipping their own builds POST
   them to `/admin/firmware`; the page itself has no upload form.)

## 5. Operations

- Logs: `journalctl -u pipvoice -f`, `journalctl -u mosquitto -f`
- Backups: `crontab -e` → `15 3 * * * /opt/pipvoice/scripts/backup.sh`
  (DB snapshot + audio + config/secrets tarball incl. env, VAPID key and
  mosquitto creds; 14 days retained, on-box). Belt-and-braces: a Hetzner
  **snapshot** of the configured server (~€0.02/mo for ~1.5 GB — far
  cheaper than the 20% automated-backup option, and it survives server
  deletion). `scripts/snapshot.sh` automates it as a rolling snapshot
  (create fresh → delete older, so exactly one is kept): run it from
  launchd/cron on your own machine — daily + at login with a built-in
  7-day throttle, so a machine that was off just delays the snapshot
  instead of skipping it. Restore = create a new server from the
  snapshot, then restore the latest nightly backup.
- The `hcloud` CLI drives VPS operations (snapshots, rescue mode,
  rebuilds) from your machine: `brew install hcloud`, create an API
  token in the Hetzner Cloud Console (project → *Security → API tokens*,
  read+write), then `hcloud context create pip` and paste the token.
  Keep the token **off the server** — a compromised box must not be able
  to delete its own recovery snapshots.
- Updating the server app: keep a clone of this repo on the box and

  ```bash
  cd /root/pipvoice && git pull
  cp -r server/app /opt/pipvoice/ && chown -R pipvoice:pipvoice /opt/pipvoice/app
  systemctl restart pipvoice
  ```

  If `server/requirements.txt` changed since the last deploy (rare), also:

  ```bash
  cp server/requirements.txt /opt/pipvoice/
  sudo -u pipvoice /opt/pipvoice/venv/bin/pip install -r /opt/pipvoice/requirements.txt
  ```
- Extra environment (in `/opt/pipvoice/env`, read by the systemd unit; the
  full reference with defaults is `server/env.example`):
  `PIP_SMTP_HOST/PORT/USER/PASS/FROM` enable outgoing email — **required
  in practice**: sign-in codes, install instructions, and the notification
  fallback all ride on it (unset → email rungs are skipped, and only
  already-signed-in users can get in). Production uses Resend
  (free tier): verify the domain in their dashboard (MX + SPF TXT on
  `send.<domain>`, DKIM TXT on `resend._domainkey.<domain>`; at Porkbun
  enter only the host part — it appends the domain), then
  `PIP_SMTP_HOST=smtp.resend.com`, `PIP_SMTP_PORT=587`,
  `PIP_SMTP_USER=resend`, `PIP_SMTP_PASS=<api key>`,
  `PIP_SMTP_FROM=Pip <pip@yourdomain.com>`. Web-push VAPID keys are
  generated automatically into the data dir on first use. `ffmpeg`
  (installed by setup.sh) handles phone-audio transcoding and theme
  rendering.
- **Locked out of /admin** (SMTP down, no valid session): SSH in and
  insert a session row by hand, then set `pip_session=<token>` as a
  cookie in your browser:

  ```bash
  TOKEN=$(python3 -c "import secrets;print(secrets.token_urlsafe(32))")
  HASH=$(python3 -c "import hashlib;print(hashlib.sha256('$TOKEN'.encode()).hexdigest())")
  sqlite3 /opt/pipvoice/data/pip.db "INSERT INTO sessions VALUES ('$HASH',
    (SELECT id FROM users WHERE is_admin=1 LIMIT 1),
    datetime('now','+1 day'));"
  echo "cookie value: $TOKEN"
  ```
- Revoking a device: *Devices* → **Delete** in the admin site; its token
  stops working immediately.

## 6. Disaster recovery — rebuilding a deleted server

All state lives on the box (the backup cron writes to `/opt/pipvoice/backups`
— also on the box), so deleting the server loses the DB (users, devices,
permissions), audio, uploaded firmware, the VAPID key, and `/opt/pipvoice/env`.
To rebuild on a fresh instance:

1. Repeat §1–§3 (same domain; just update the DNS A record).
2. Recreate the repo clone: `ssh-keygen` on the new server, add the
   public key as a **read-only deploy key** in the GitHub repo settings,
   `git clone git@github.com:<you>/pipvoice.git /root/pipvoice`.
3. `/admin` first visit recreates the admin account; recreate users
   (same email addresses — there are no passwords) and the permissions
   matrix. Re-set the SMTP vars early: without them nobody can sign in.
4. Devices, two options:
   - Re-provision each box in the admin and reflash its NVS — or
   - **Zero-touch resurrection**: device tokens are stored as plain
     SHA-256 hashes, and the provisioning CSVs (kept off-box, gitignored)
     contain each box's plaintext `auth_token` and `mqtt_pass` — treat
     those CSVs as secret material and keep a copy somewhere safe; they
     are what makes this recovery possible. Insert a
     `devices` row per box with the *same* id,
     `token_hash = sha256(auth_token)` and the CSV's `mqtt_password`,
     recreate broker creds (`mosquitto_passwd -b ... <device_id> <pass>`),
     and every box reconnects by itself — the startup ACL rebuild adds
     the rest.
5. Rebuild the current firmware from the matching tag, upload + activate.
6. Web push: the VAPID key regenerates on first use; phone users log in
   again and their PWAs re-subscribe on launch. Re-set the SMTP vars in
   `/opt/pipvoice/env` if they were in use. Re-add the backup crontab entry.

## Notes on the auth model

Messages flow **user → user**. Devices authenticate with a device token
that maps to its owner. Phone users have no passwords: they enter their
email (`POST /v1/auth/request-code`), receive a 6-digit code (10-minute,
single-use, 5 attempts), and redeem it (`POST /v1/auth/verify-code`) for
a session cookie (sliding 30-day expiry) usable on the exact same
endpoints. `/admin` signs in the same way (admins only). A user is
either a box user or a phone user; the permissions matrix governs
people, not hardware.
