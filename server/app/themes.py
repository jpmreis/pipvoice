"""themes: background images, rendered once per client kind.

Masters ship with the repo (app/themes/<name>.jpeg, device-aspect ~4:5).
render_all() caches derived variants under DATA_DIR/themes at startup -
every theme exists in FOUR versions, one per board plus the PWA:
  <name>-device.bin           368x448  raw RGB565 LE   (amoled-1.8; the
                              legacy filename, so old firmware URLs and
                              already-rendered caches keep working)
  <name>-thumb.bin            108x132  raw RGB565 LE   (amoled-1.8 picker)
  <name>-amoled-1.75b-device.bin  466x466  centre-crop for the round panel
  <name>-amoled-1.75b-thumb.bin   100x100  (circular tiles in the picker)
  <name>-amoled-2.16-device.bin   480x480
  <name>-amoled-2.16-thumb.bin    106x106
  <name>-web.jpg              1080px-wide JPEG for the PWA (cover-cropped
                              client-side)

The raw .bin renditions are the panels' native format, so the firmware
loads bytes straight into PSRAM with no image decoder; a device asks for
its own rendition with ?board= (api.py) and its byte-count guard rejects
a wrong-size file. The round/square renditions are centre-crops of the
same 4:5 masters for now - format-aware masters (round: centre-weighted,
nothing in the corners) are a planned regeneration pass.

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

from . import boards, db

log = logging.getLogger("themes")

SRC_DIR = os.path.join(os.path.dirname(__file__), "themes")
OUT_DIR = os.path.join(db.DATA_DIR, "themes")

# per-board picker thumbnail sizes - MUST match the firmware's
# ui_geometry.h GEO_THUMB_W/H for that board (theme.c and the picker
# reject/mis-render anything else)
THUMBS = {
    "amoled-1.8":   (108, 132),
    "amoled-1.75b": (100, 100),
    "amoled-2.16":  (106, 106),
}

THEMES = [
    {"name": "cloud",    "label": "Cloud",    "fg": "white"},
    {"name": "dark",     "label": "Dark",     "fg": "white"},
    {"name": "garden",   "label": "Garden",   "fg": "black"},
    {"name": "olivia",   "label": "Olivia",   "fg": "black"},
    {"name": "pink",     "label": "Pink",     "fg": "black"},
    {"name": "sea",      "label": "Sea",      "fg": "white"},
    {"name": "benfica",  "label": "Benfica",  "fg": "black"},
    {"name": "portugal", "label": "Portugal", "fg": "black"},
    {"name": "namibia",  "label": "Namibia",  "fg": "white"},
    {"name": "mario",    "label": "Mario",    "fg": "black"},
]


def get(name):
    return next((t for t in THEMES if t["name"] == name), None)


def _suffix(board: str) -> str:
    """Default board keeps the legacy asset names (old firmware URLs)."""
    return "" if board == boards.DEFAULT_BOARD else f"-{board}"


def device_path(name: str, board: str = boards.DEFAULT_BOARD) -> str:
    return os.path.join(OUT_DIR, f"{name}{_suffix(board)}-device.bin")


def thumb_path(name: str, board: str = boards.DEFAULT_BOARD) -> str:
    return os.path.join(OUT_DIR, f"{name}{_suffix(board)}-thumb.bin")


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


def _raw565(src: str, dst: str, w: int, h: int) -> None:
    """Cover-scale + centre-crop to w x h, raw RGB565 little-endian."""
    _ffmpeg(src, dst,
            "-vf",
            f"scale={w}:{h}:"
            "force_original_aspect_ratio=increase:flags=lanczos,"
            f"crop={w}:{h}",
            "-f", "rawvideo", "-pix_fmt", "rgb565le")


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
            for board, spec in boards.BOARDS.items():
                scr = spec["screen"]
                if stale(device_path(t["name"], board)):
                    _raw565(src, device_path(t["name"], board),
                            scr["w"], scr["h"])
                tw, th = THUMBS[board]
                if stale(thumb_path(t["name"], board)):
                    _raw565(src, thumb_path(t["name"], board), tw, th)
            if stale(web_path(t["name"])):
                _ffmpeg(src, web_path(t["name"]),
                        "-vf", "scale=1080:-2:flags=lanczos", "-q:v", "4")
            log.info("theme %s ready", t["name"])
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            log.error("theme %s render failed: %s", t["name"], e)


def available():
    """Themes whose rendered variants exist (i.e. safe to offer clients).
    Gated on the default board's set + web: all renditions come from the
    same master in the same pass, so one failing means the pass failed;
    a per-board asset that is somehow missing 404s at its endpoint."""
    return [t for t in THEMES
            if os.path.exists(device_path(t["name"]))
            and os.path.exists(thumb_path(t["name"]))
            and os.path.exists(web_path(t["name"]))]
