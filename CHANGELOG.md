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

## [1.0.0] — 2026-08-23

The first full version. Nothing new is switched on here — Pip has been
carrying the family's messages for months, and this release is the two
fixes the wandering-dot sleep screen needed to feel finished.

### Changed
- The sleeping dot now moves every 10 seconds instead of every minute,
  so a passing glance is much more likely to catch it mid-hop. A full
  sweep of the 48-cell grid takes 8 minutes rather than 48; the burn-in
  budget is unchanged, since what each pixel takes is set by the grid,
  not by how often the dot moves.

### Fixed
- Touching a sleeping box to wake it blanked the screen for the length
  of the hello-splash: the dot vanished the instant you touched it and
  the letters fell into an empty panel, which is exactly the seam the
  sleep screen was built to avoid. The dot now stays where the night
  left it and glides into the wordmark while the letters fall, as
  intended. (The travelling dot was being drawn inside the "Pip"
  wordmark's own box, so anywhere else on the panel it was clipped away
  and simply not drawn.)
- Waking mid-bounce started the glide from slightly the wrong place —
  up to 16 px of jump at the moment of the touch.

## [0.1.36] — 2026-08-23

A firmware release: boxes get a new sleep state. Phones and the server
are unchanged apart from three new background themes.

### Added
- A box that falls asleep with unheard messages no longer goes dark: the
  amber dot from the hello-splash — the full stop of "Pip." — stays on
  the black screen at about a third of full brightness, out wandering
  for the night. It moves to a new spot on a 48-cell grid every minute
  and greets each one with a little squash-and-stretch bounce, so "you
  have mail" catches a passing eye across a dark room while no pixel
  holds the dot for more than ~2% of the ambient hours (a 14 px dot
  lights well under 0.1% of the panel — and amber was already the
  safest colour to hold here: its blue channel is dark, and blue
  emitters age fastest). With nothing unheard the screen goes fully off
  exactly as before, and quiet hours keep it dark too — a message left
  unheard from before bedtime shouldn't glow all night.

  Touching the screen blanks nothing: the "Pip" letters fall in as they
  always have while the dot glides home from wherever the night left it
  and lands as the wordmark's full stop, brightness easing up
  alongside. The buttons still go straight to recording or the inbox.

  A message arriving overnight gets the chime and the dot, but no toast
  — the dot is the notification, and a lit card at 3 a.m. is not.
- Three new background themes to pick from in Settings: Benfica,
  Portugal and Namibia.

### Fixed
- Waking a box while the "No WiFi" card was up could flash the card over
  the hello-splash for a frame before it went away.

## [0.1.35] — 2026-08-23

A PWA release. The firmware is identical to 0.1.34 apart from its
version stamp — boxes will update, but nothing about them changes.
Activating it is what the phones are here for: an installed PWA only
hard-refreshes when the active version moves, so this is how the badge
below reaches phones that are still running older app code.

### Added
- The installed PWA now carries an unread count on its home-screen icon.
  The push carries the number so the badge is right even when the app has
  never been opened; playing, deleting or signing out updates it.
  Requires an installed app and notification permission — on iOS the same
  pair web push already needs — and is silently absent everywhere else.

### Fixed
- Deleting an unheard message left the inbox tab's count one too high
  until the next refresh.

## [0.1.34] — 2026-08-23

Notifications and messages used to race, and on weak wifi the
notification kept winning: a banner or a chime, then seconds of nothing
before the message was actually there. This release makes a notify mean
"the message is fetchable now", and takes the waiting out of what
happens after one.

### Changed
- Voice messages are served to phones as AAC in MP4 instead of
  uncompressed WAV — about 8x smaller (a 20 s message goes from ~640 KB
  to ~80 KB). Rendered once when the message is sent and cached beside
  the recording; older messages and older clients still get the WAV.
- The PWA's service worker downloads a message's audio as soon as its
  push arrives, so it is already on the phone by the time the
  notification is tapped, and playback starts from local storage
  instead of opening a connection on a phone that has just woken up.
- An open PWA now shows a new message the moment its push lands,
  instead of on its next 60 s poll.
- Boxes get the sender, colour, timestamp and duration inside the MQTT
  notify, so they fetch the audio directly instead of asking for the
  inbox listing first. That is one fewer TLS handshake — four round
  trips — before the chime.
- A box chimes as soon as a message is on its flash. It used to wait
  for the server to be told the message had been delivered, which on a
  weak link was another handshake of silence after the message had
  already arrived.
- New mail jumps the queue on a box: an arriving message is fetched
  before the contact refresh, outbox upload, or theme download that
  would otherwise hold the device's single TLS slot.

### Fixed
- Sending a message no longer stalls the whole server while the
  recipient is notified. Encoding the audio and delivering web push
  (up to 10 s per subscription) ran on the event loop, and there is one
  worker — so the recipient's phone, woken by that very push, was
  queueing behind it.
- A voice message whose download was cut short is no longer kept as if
  it were complete. Boxes mirror the server inbox and never re-fetch
  what they already have, so a truncated recording stayed truncated;
  partial downloads are now discarded and retried.

## [0.1.33] — 2026-08-22

### Fixed
- Quiet hours could deliver (and chime) the moment a box was powered on
  at night. A box that has just booted has no clock until SNTP answers,
  and "I don't know what time it is" was being read as daytime, so the
  first sync ran before the hold could apply. Incoming mail now waits
  for the clock - bounded, so a network that blocks NTP still gets its
  messages a couple of minutes after boot.
- Boot no longer flashes the home screen before the greeting: the
  splash is put on the panel synchronously and the backlight comes up
  on the animation itself.
- The battery poll and button task now start after the greeting rather
  than during it: both talk to the power chip over I2C and the first
  read fires immediately, which is work a 2 s animation should not have
  to share a system with.

## [0.1.32] — 2026-08-22

Fixes from testing 0.1.31 on a box.

### Fixed
- The boot greeting no longer stutters. The splash used to animate on
  top of the heaviest part of boot - the cached background being read
  out of LittleFS and applied under the LVGL lock, and WiFi bring-up.
  Boot now keeps the panel dark while that work happens, plays the
  greeting on a quiet system, and starts the network afterwards
  (online a couple of seconds later, which nothing is waiting on).
- The "No WiFi" card no longer appears in the first three minutes after
  a boot: a box coming back from an OTA can spend a while
  re-associating, and the card popping up over a box that just updated
  itself read as the update having broken something. The status-bar
  retry glyph still shows what's happening.

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
