"""themes: background images for device + PWA.

Masters ship with the repo (app/themes/<name>.jpeg, device-aspect ~4:5).
render_all() caches derived variants under DATA_DIR/themes at startup:
  <name>-device.bin  368x448 raw RGB565 little-endian - the panel's native
                     format, so the firmware loads bytes straight into PSRAM
                     with no image decoder
  <name>-thumb.bin   108x132 raw RGB565 little-endian picker thumbnail
                     (3-column grid on the 368px panel, same aspect)
  <name>-web.jpg     1080px-wide JPEG for the PWA (cover-cropped client-side)

fg is the color for text drawn directly on the background; text inside its
own surface (avatars, buttons, message rows) keeps the normal palette.

Each theme has a version: sha256 of its master, 8 hex chars. Clients cache
thumbs/backgrounds keyed by it and skip the download when it is unchanged
(PWA: immutable-cache ?v= URLs; device: LittleFS files named <name>-<ver>).
"""
import hashlib
import logging
import os
import subprocess

from . import db

log = logging.getLogger("themes")

SRC_DIR = os.path.join(os.path.dirname(__file__), "themes")
OUT_DIR = os.path.join(db.DATA_DIR, "themes")

DEVICE_W, DEVICE_H = 368, 448
THUMB_W, THUMB_H = 108, 132

THEMES = [
    {"name": "cloud",    "label": "Cloud",    "fg": "white"},
    {"name": "dark",     "label": "Dark",     "fg": "white"},
    {"name": "garden",   "label": "Garden",   "fg": "black"},
    {"name": "olivia",   "label": "Olivia",   "fg": "black"},
    {"name": "pink",     "label": "Pink",     "fg": "black"},
    {"name": "sea",      "label": "Sea",      "fg": "white"},
    {"name": "benfica",  "label": "Benfica",  "fg": "white"},
    {"name": "portugal", "label": "Portugal", "fg": "black"},
    {"name": "namibia",  "label": "Namibia",  "fg": "white"},
]


def get(name):
    return next((t for t in THEMES if t["name"] == name), None)


def device_path(name: str) -> str:
    return os.path.join(OUT_DIR, f"{name}-device.bin")


def thumb_path(name: str) -> str:
    return os.path.join(OUT_DIR, f"{name}-thumb.bin")


def web_path(name: str) -> str:
    return os.path.join(OUT_DIR, f"{name}-web.jpg")


_ver_cache = {}   # name -> (master mtime, ver)


def version_of(name: str) -> str:
    """8-hex content hash of the theme's master image."""
    src = os.path.join(SRC_DIR, name + ".jpeg")
    try:
        mt = os.path.getmtime(src)
    except OSError:
        return "0"
    hit = _ver_cache.get(name)
    if hit and hit[0] == mt:
        return hit[1]
    with open(src, "rb") as f:
        ver = hashlib.sha256(f.read()).hexdigest()[:8]
    _ver_cache[name] = (mt, ver)
    return ver


def _ffmpeg(src: str, dst: str, *args: str) -> None:
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src,
                    *args, dst], check=True)


def render_all() -> None:
    """(Re)render any variant that is missing or older than its master."""
    os.makedirs(OUT_DIR, exist_ok=True)
    for t in THEMES:
        src = os.path.join(SRC_DIR, t["name"] + ".jpeg")
        if not os.path.exists(src):
            log.warning("theme %s: master missing (%s)", t["name"], src)
            continue
        stale = lambda p: (not os.path.exists(p)
                           or os.path.getmtime(p) < os.path.getmtime(src))
        try:
            if stale(device_path(t["name"])):
                _ffmpeg(src, device_path(t["name"]),
                        "-vf",
                        f"scale={DEVICE_W}:{DEVICE_H}:"
                        "force_original_aspect_ratio=increase:flags=lanczos,"
                        f"crop={DEVICE_W}:{DEVICE_H}",
                        "-f", "rawvideo", "-pix_fmt", "rgb565le")
            if stale(thumb_path(t["name"])):
                _ffmpeg(src, thumb_path(t["name"]),
                        "-vf",
                        f"scale={THUMB_W}:{THUMB_H}:"
                        "force_original_aspect_ratio=increase:flags=lanczos,"
                        f"crop={THUMB_W}:{THUMB_H}",
                        "-f", "rawvideo", "-pix_fmt", "rgb565le")
            if stale(web_path(t["name"])):
                _ffmpeg(src, web_path(t["name"]),
                        "-vf", "scale=1080:-2:flags=lanczos", "-q:v", "4")
            log.info("theme %s ready", t["name"])
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            log.error("theme %s render failed: %s", t["name"], e)


def available():
    """Themes whose rendered variants exist (i.e. safe to offer clients)."""
    return [t for t in THEMES
            if os.path.exists(device_path(t["name"]))
            and os.path.exists(thumb_path(t["name"]))
            and os.path.exists(web_path(t["name"]))]
