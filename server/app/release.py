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

from . import boards, db, provision

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


def _dest_path(version: str, kind: str, board: str) -> str:
    if kind == "app":
        return db.firmware_path(version, board)
    return provision.asset_path(version, kind, board)  # bootloader/parttable


def _install_build(version: str, board: str, build: dict) -> bool:
    """Download + verify one board's parts; True when work was done."""
    parts = build.get("parts", [])
    kinds = {p.get("kind") for p in parts}
    if not {"app", "bootloader", "parttable"} <= kinds:
        raise ValueError(f"{board}: manifest bundle incomplete: "
                         f"{sorted(kinds)}")

    with db.conn() as c:
        have_row = db.one(c, """SELECT version FROM firmware
                                WHERE version=? AND board=?""",
                          (version, board))
    have_files = all(os.path.exists(_dest_path(version, k, board))
                     for k in ("app", "bootloader", "parttable"))
    if have_row and have_files:
        return False

    for p in parts:
        kind = p.get("kind")
        if kind not in ("app", "bootloader", "parttable"):
            continue                     # e.g. future otadata entries
        # resolve against the *requested* URL, not the redirect target:
        # GitHub hands out release assets via a signed CDN, so a sibling
        # path next to the final URL lacks that asset's token (HTTP 618
        # jwt-not-provided). Re-walking the redirect per part gets each
        # asset its own token; a "latest" release that moves mid-install
        # is caught by the sha256 checks below.
        data, _ = _get(urljoin(MANIFEST_URL, p["path"]), MAX_PART)
        got = hashlib.sha256(data).hexdigest()
        if got != p.get("sha256"):
            raise ValueError(f"{p['path']}: sha256 mismatch "
                             f"(manifest {p.get('sha256')}, got {got})")
        dest = _dest_path(version, kind, board)
        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dest)            # atomic: OTA never sees a torn file
        log.info("release %s/%s: %s ok (%d bytes)",
                 version, board, kind, len(data))

    with db.conn() as c:                 # inactive; admin activates when ready
        c.execute("""INSERT OR IGNORE INTO firmware
                     (version, notes, active, board) VALUES (?,?,0,?)""",
                  (version, "GitHub release", board))
    return True


def install() -> str:
    """Fetch the manifest and install its version - every board build it
    carries. Returns a human message; raises on network/validation
    failure. An unknown board name is a hard error (a silent first-match
    here once meant a wrong-model flash was possible); a pre-multi-SKU
    manifest (no board field) is the 1.8's."""
    raw, _ = _get(MANIFEST_URL, MAX_MANIFEST)
    m = json.loads(raw)
    version = str(m.get("version", ""))
    if not re.fullmatch(_VERSION_RE, version):
        raise ValueError(f"manifest has a bad version: {version!r}")

    builds = {}                          # board key -> build entry
    for b in m.get("builds", []):
        if b.get("chipFamily") != CHIP_FAMILY:
            continue
        key = boards.by_manifest_name(b.get("board", ""))
        if key is None:
            raise ValueError(f"manifest names an unknown board: "
                             f"{b.get('board')!r}")
        if key in builds:
            raise ValueError(f"manifest lists two builds for {key}")
        builds[key] = b
    if not builds:
        raise ValueError(f"manifest has no {CHIP_FAMILY} builds")

    os.makedirs(db.FIRMWARE_DIR, exist_ok=True)
    installed = [k for k, b in sorted(builds.items())
                 if _install_build(version, k, b)]
    if not installed:
        return f"already installed: {version} ({', '.join(sorted(builds))})"
    return (f"installed {version} for {', '.join(installed)} - "
            "activate per model when ready")
