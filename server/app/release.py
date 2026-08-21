"""release: pull published firmware releases into this server.

The release workflow (.github/workflows/release.yml) attaches the flash
bundle and a manifest.json (ESP Web Tools-compatible superset with a
sha256 per part) to every tagged GitHub Release. install() downloads
the build for this board, verifies every hash, and stores it exactly
like a manual admin upload — as an *inactive* row: activation stays a
separate explicit admin action, because activating publishes the
retained MQTT notify that rolls the whole fleet.

PIP_RELEASE_MANIFEST points elsewhere for forks/self-hosted build
infrastructure; the default follows the upstream project's latest
release. The server fetches over TLS and verifies the manifest's
hashes, so a compromised CDN can corrupt at worst availability, not
integrity beyond what the manifest itself asserts.
"""
import hashlib
import json
import logging
import os
import re
import urllib.request
from urllib.parse import urljoin

from . import db, provision

log = logging.getLogger("release")

MANIFEST_URL = db.env(
    "RELEASE_MANIFEST",
    "https://github.com/jpmreis/pipvoice/releases/latest/download/manifest.json")

CHIP_FAMILY = "ESP32-S3"
MAX_MANIFEST = 256 * 1024
MAX_PART = 8 * 1024 * 1024      # ota_0 slot is 3 MB; leave headroom
TIMEOUT_S = 30
_VERSION_RE = r"[A-Za-z0-9._-]{1,32}"


def _get(url: str, cap: int):
    """(bytes, final URL after redirects). Size-capped."""
    req = urllib.request.Request(url, headers={"User-Agent": "pip-server"})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        data = r.read(cap + 1)
        if len(data) > cap:
            raise ValueError(f"{url}: larger than {cap} bytes")
        return data, r.geturl()


def _dest_path(version: str, kind: str) -> str:
    if kind == "app":
        return db.firmware_path(version)
    return provision.asset_path(version, kind)   # bootloader / parttable


def install() -> str:
    """Fetch the manifest and install its version. Returns a human
    message; raises on network/validation failure."""
    raw, manifest_url = _get(MANIFEST_URL, MAX_MANIFEST)
    m = json.loads(raw)
    version = str(m.get("version", ""))
    if not re.fullmatch(_VERSION_RE, version):
        raise ValueError(f"manifest has a bad version: {version!r}")

    build = next((b for b in m.get("builds", [])
                  if b.get("chipFamily") == CHIP_FAMILY), None)
    if not build:
        raise ValueError(f"manifest has no {CHIP_FAMILY} build")
    parts = build.get("parts", [])
    kinds = {p.get("kind") for p in parts}
    if not {"app", "bootloader", "parttable"} <= kinds:
        raise ValueError(f"manifest bundle incomplete: {sorted(kinds)}")

    with db.conn() as c:
        have_row = db.one(c, "SELECT version FROM firmware WHERE version=?",
                          (version,))
    have_files = all(os.path.exists(_dest_path(version, k))
                     for k in ("app", "bootloader", "parttable"))
    if have_row and have_files:
        return f"already installed: {version}"

    os.makedirs(db.FIRMWARE_DIR, exist_ok=True)
    for p in parts:
        kind = p.get("kind")
        if kind not in ("app", "bootloader", "parttable"):
            continue                     # e.g. future otadata entries
        data, _ = _get(urljoin(manifest_url, p["path"]), MAX_PART)
        got = hashlib.sha256(data).hexdigest()
        if got != p.get("sha256"):
            raise ValueError(f"{p['path']}: sha256 mismatch "
                             f"(manifest {p.get('sha256')}, got {got})")
        dest = _dest_path(version, kind)
        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dest)            # atomic: OTA never sees a torn file
        log.info("release %s: %s ok (%d bytes)", version, kind, len(data))

    with db.conn() as c:                 # inactive; admin activates when ready
        c.execute("""INSERT OR IGNORE INTO firmware (version, notes, active)
                     VALUES (?,?,0)""", (version, "GitHub release"))
    return f"installed {version} - activate it when ready"
