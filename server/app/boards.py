"""boards: the hardware models (SKUs) Pip runs on.

The single server-side source of truth for board identity. The short
key is what travels everywhere: the NVS `board` key baked at provision
time, the `devices.board` and `firmware.board` DB columns, the flash
manifest, and the firmware's compiled-in PIP_BOARD_NAME (printed in the
boot PIP-HW line, checked by the web flasher). The long manifest name
is what CI's make_manifest.py writes in a release's builds[] entries.

Adding a board here does NOT make it flashable - the setup page marks a
model available only once an active firmware build exists for it (see
api.setup_boards). That is what makes multi-SKU OTA safe: the server
never offers or serves a binary for a model other than the one recorded
on the device row at first provisioning.
"""

DEFAULT_BOARD = "amoled-1.8"

BOARDS = {
    "amoled-1.8": {
        "label": "1.8″ classic",
        "full_name": "Waveshare ESP32-S3-Touch-AMOLED-1.8",
        "manifest_board": "waveshare-esp32-s3-touch-amoled-1.8",
        "screen": {"w": 368, "h": 448, "shape": "rect"},
        "blurb": "Rectangular 368×448 · the original Pip",
    },
    "amoled-1.75b": {
        "label": "1.75″ round",
        "full_name": "Waveshare ESP32-S3-Touch-AMOLED-1.75-B",
        "manifest_board": "waveshare-esp32-s3-touch-amoled-1.75-b",
        "screen": {"w": 466, "h": 466, "shape": "round"},
        "blurb": "Round 466×466 · cased, side buttons",
    },
    "amoled-2.16": {
        "label": "2.16″ square",
        "full_name": "Waveshare ESP32-S3-Touch-AMOLED-2.16",
        "manifest_board": "waveshare-esp32-s3-touch-amoled-2.16",
        "screen": {"w": 480, "h": 480, "shape": "rect"},
        "blurb": "Square 480×480 · the big one",
    },
}


def valid(key: str) -> bool:
    return key in BOARDS


def by_manifest_name(name: str):
    """Board key for a release manifest's builds[].board value, or None.
    A missing/empty name means a pre-multi-SKU manifest: the 1.8."""
    if not name:
        return DEFAULT_BOARD
    for key, b in BOARDS.items():
        if b["manifest_board"] == name:
            return key
    return None
