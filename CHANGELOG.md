# Changelog

One version number covers the whole system — firmware, server, and PWA
(`PROJECT_VER` in `firmware/CMakeLists.txt`). Releases are git tags
`v<version>` on this repo; each tag is built by CI and published as a
GitHub Release with the flash bundle and an ESP Web Tools-compatible
`manifest.json`.

Format: [Keep a Changelog](https://keepachangelog.com/). The release
workflow copies a version's section into the GitHub Release notes, so
write entries for humans.

## [Unreleased]

## [0.1.31] — 2026-08-22

Device-side polish: what the box shows when the network is unhappy,
quiet hours at night, and a friendlier WiFi setup page.

### Added
- Device status bar shows WiFi *strength*: a drawn three-arc glyph lit
  0-3 by the AP's RSSI (-55/-67/-78 dBm), replacing the fixed WiFi
  symbol. The other states keep their symbols (retrying, portal
  sign-in needed, offline).
- Device "No WiFi" overlay: after 60 s offline the home screen shows a
  card offering **Set up WiFi** (PIN-gated, opens the setup QR) or
  **Not now** (snoozes 5 minutes, then returns if WiFi is still out).
  It never covers a recording, the inbox, settings or the setup screen,
  and captive portals keep their existing sign-in toast.
- Device quiet hours: between 21:00 and 08:00 local time a box leaves
  incoming messages on the server - nothing lands and nothing chimes
  until morning, when the night's messages arrive together. Sending is
  unaffected. A sleepy "Zzz" rides on the home inbox pill and a
  quiet-hours card sits at the top of the inbox.
- WiFi setup page: a **Scan again** button, and a list of saved
  networks with **Forget** (names only - passwords are never sent to
  the page).

### Changed
- Landing page: the Pip Cloud card no longer offers the waitlist signup
  (the waitlist page itself stays), the hosting cards invite readers to
  the source on GitHub, and the board price figure is gone.

### Fixed
- Background picker: the amber ring on the chosen background was hidden
  behind its own thumbnail (children draw over a parent's border), and
  the multi-second download had no feedback. The picked tile now keeps
  the ring and spins until the image lands.

## [0.1.30] — 2026-08-21

### Added
- Self-hosting support: Docker Compose stack (app + Mosquitto + Caddy)
  with automatic Let's Encrypt, `docs/SELF-HOSTING.md` guide, and a
  landing-page "Pip Cloud vs Self-host" section.
- Local passwords (`PIP_LOCAL_AUTH=1`): scrypt-hashed per-user
  passwords set in the admin; PWA and admin login offer password
  sign-in so self-hosters can skip SMTP entirely. First-run admin
  bootstrap takes a password directly.
- Web flasher: a Server URL field (prefilled with the flashing site)
  is written to the device, so boxes follow whichever server they were
  flashed from.
- GitHub Actions release builds: tagging `v<version>` builds the
  firmware, packages the flash bundle, and publishes a GitHub Release
  with a sha256-carrying manifest.
- Admin Firmware page can fetch the latest published release straight
  from GitHub (hash-verified) instead of a manual upload.
- `PIP_MQTT_PUBLIC_HOST` / `PIP_MQTT_PUBLIC_PORT` for split deployments;
  `server/env.example` documenting every setting.
- Waitlist signups email every admin who has an email address on file.
- The device-setup page has a back button, so it is no longer a dead
  end until a flash finishes.

### Changed
- Landing and privacy pages render the deployment's own domain from
  `PIP_BASE_URL` (no hardcoded hostnames in the repo).
- One hosted-vs-self-hosted check — `db.hosted()` on the server,
  `config_is_hosted()` in firmware — derived from whether the
  deployment's base URL is pipvoice.com. Self-hosted servers get no
  public waitlist (page, endpoint, or login link) and their landing
  and privacy pages speak honestly about who runs the server, instead
  of carrying the hosted instance's claims (data centre, email
  provider, backups, free service, passwordless sign-in).
- Processed public pages (landing, privacy, waitlist) moved out of the
  PWA static mount; their raw templates are no longer reachable under
  `/app/`.
- Install page and install email only promise passwordless sign-in
  where it is true.
- Open-sourced under AGPL-3.0 (closed contributions).

## [0.1.29] — 2026-08-19

The version in production when the project went open source.

- Web flashing is the provisioning path: PWA Settings → "Set up a new
  Pip" creates the device and flashes a blank board over Web Serial.
- Passwordless auth: email → 6-digit code, PWA and admin alike.
- Privacy: messages delete from the server on delivery; undelivered
  messages age out after 30 days.
- Reactions, presence, per-user background themes, OTA via retained
  MQTT notify, offline-first sync.
