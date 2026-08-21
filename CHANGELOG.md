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

### Changed
- Landing and privacy pages render the deployment's own domain from
  `PIP_BASE_URL` (no hardcoded hostnames in the repo).
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
