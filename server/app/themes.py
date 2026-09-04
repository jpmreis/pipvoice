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
a wrong-size file.

A theme may ship per-format masters next to the base 4:5 one, and each
rendition is cut from the closest master (falling back to the base):
  <name>-round.jpeg    1:1, all content inside the inscribed circle,
                       black corners (amoled-1.75b)
  <name>-square.jpeg   1:1 (amoled-2.16)
  <name>-phone.jpeg    9:16 portrait (PWA web.jpg)
The base <name>.jpeg stays the 1.8's source and the universal fallback;
its hash keys the 1.8 fleet's caches, so never regenerate it.

fg is the color for text drawn directly on the background; text inside its
own surface (avatars, buttons, message rows) keeps the normal palette.

Each theme has a version per FORMAT: sha256 of the master that format is
cut from, 8 hex chars. Clients cache thumbs/backgrounds keyed by it and
skip the download when it is unchanged (PWA: immutable-cache ?v= URLs;
device: LittleFS files named <name>-<ver>). api.py picks the format from
the calling client (device row's board, or the PWA), so adding/updating
a -round/-square/-phone master cache-busts exactly the clients that
consume it while the 1.8's base-master version stays untouched.
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

# which per-format master feeds each board's renditions (None = the base
# 4:5 master; also the fallback when the format master doesn't exist)
FORMATS = {
    "amoled-1.8":   None,
    "amoled-1.75b": "round",
    "amoled-2.16":  "square",
}
WEB_FORMAT = "phone"

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


def board_format(board: str):
    """Master format key for a board ('round'/'square'/None)."""
    return FORMATS.get(board)


def master_path(name: str, fmt: str | None = None) -> str:
    """Source master for a format, falling back to the base 4:5 one."""
    if fmt:
        p = os.path.join(SRC_DIR, f"{name}-{fmt}.jpeg")
        if os.path.exists(p):
            return p
    return os.path.join(SRC_DIR, name + ".jpeg")


def _suffix(board: str) -> str:
    """Default board keeps the legacy asset names (old firmware URLs)."""
    return "" if board == boards.DEFAULT_BOARD else f"-{board}"


def device_path(name: str, board: str = boards.DEFAULT_BOARD) -> str:
    return os.path.join(OUT_DIR, f"{name}{_suffix(board)}-device.bin")


def thumb_path(name: str, board: str = boards.DEFAULT_BOARD) -> str:
    return os.path.join(OUT_DIR, f"{name}{_suffix(board)}-thumb.bin")


def web_path(name: str) -> str:
    return os.path.join(OUT_DIR, f"{name}-web.jpg")


_ver_cache = {}   # (name, fmt) -> (master path, mtime, ver)


def version_of(name: str, fmt: str | None = None) -> str:
    """8-hex content hash of the master a format is cut from. A format
    without its own master inherits the base master's hash, so fallback
    renditions share the base version."""
    src = master_path(name, fmt)
    try:
        mt = os.path.getmtime(src)
    except OSError:
        return "0"
    hit = _ver_cache.get((name, fmt))
    if hit and hit[0] == src and hit[1] == mt:
        return hit[2]
    with open(src, "rb") as f:
        ver = hashlib.sha256(f.read()).hexdigest()[:8]
    _ver_cache[(name, fmt)] = (src, mt, ver)
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
    stale = lambda p, src: (not os.path.exists(p)
                            or os.path.getmtime(p) < os.path.getmtime(src))
    for t in THEMES:
        if not os.path.exists(master_path(t["name"])):
            log.warning("theme %s: master missing (%s)", t["name"],
                        master_path(t["name"]))
            continue
        try:
            for board, spec in boards.BOARDS.items():
                src = master_path(t["name"], FORMATS[board])
                scr = spec["screen"]
                if stale(device_path(t["name"], board), src):
                    _raw565(src, device_path(t["name"], board),
                            scr["w"], scr["h"])
                tw, th = THUMBS[board]
                if stale(thumb_path(t["name"], board), src):
                    _raw565(src, thumb_path(t["name"], board), tw, th)
            src = master_path(t["name"], WEB_FORMAT)
            if stale(web_path(t["name"]), src):
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
