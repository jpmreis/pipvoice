"""provision: shared device+user provisioning and web-flash support.

Used by the /admin device form and the PWA "setup device" flow. Creating
a device mints its bearer token, broker credentials and the NVS config
the box boots from. For the browser flasher the freshly generated NVS
partition image is handed out through a short-lived in-memory stash: the
raw device token is never persisted (only its hash is in the DB), so the
image exists nowhere else. In-memory is fine - single uvicorn worker by
design (see rate limiter / presence).
"""
import hashlib
import os
import re
import secrets
import tempfile
import threading
import time
from types import SimpleNamespace

from fastapi import HTTPException

from . import boards, db, mqtt
from .auth import new_token

# 16 avatar colors that read well on the black theme (admin imports this)
PALETTE = ["FFB300", "FF8A65", "F06292", "BA68C8",
           "9575CD", "7986CB", "4FC3F7", "4DD0E1",
           "4DB6AC", "3DDC84", "AED581", "DCE775",
           "FFD54F", "FFA726", "A1887F", "90A4AE"]

NVS_SIZE = 0x6000          # partitions.csv: nvs @ 0x9000, 0x6000 long


# ---------- NVS image ----------
_BASE_URL_RE = re.compile(r"https?://[A-Za-z0-9._~:/\-\[\]]{1,110}")


def build_nvs_csv(device_id: str, device_name: str, token_raw: str,
                  mqtt_pass: str, pin: str, base_url: str = "",
                  board: str = boards.DEFAULT_BOARD) -> str:
    """CSV for nvs_partition_gen (namespace pipcfg). No wifi keys: a box
    with zero networks auto-opens its WiFi setup QR (firmware >=0.1.28).
    base_url overrides PIP_BASE_URL as the URL written to the device —
    the flasher offers it so a box can be pointed at the address it will
    actually reach the server on (self-host LAN bench, domain alias)."""
    base = (base_url or "").strip().rstrip("/")
    if base and not _BASE_URL_RE.fullmatch(base):
        raise HTTPException(400, "server URL must be http(s):// and free "
                                 "of unusual characters")
    base = base or db.env("BASE_URL", "")
    if not base:
        raise HTTPException(500, "PIP_BASE_URL is not set - cannot build the "
                                 "device NVS CSV")
    # broker defaults to the web host; PIP_MQTT_PUBLIC_HOST/PORT override
    # for split deployments
    host = (db.env("MQTT_PUBLIC_HOST", "")
            or base.split("//")[-1].split("/")[0].split(":")[0])
    mqtt_port = int(db.env("MQTT_PUBLIC_PORT", "8883"))
    pin_salt = secrets.token_hex(8)
    pin_hash = hashlib.sha256((pin_salt + pin).encode()).hexdigest()
    safe_name = re.sub(r"[,\r\n]", " ", device_name).strip()[:23]
    return "\n".join([
        "key,type,encoding,value",
        "pipcfg,namespace,,",
        f"device_id,data,string,{device_id}",
        f"device_name,data,string,{safe_name}",
        f"server_base,data,string,{base}/v1",
        f"auth_token,data,string,{token_raw}",
        f"mqtt_host,data,string,{host}",
        f"mqtt_port,data,u32,{mqtt_port}",
        f"mqtt_user,data,string,{device_id}",
        f"mqtt_pass,data,string,{mqtt_pass}",
        f"pin_salt,data,string,{pin_salt}",
        f"pin_hash,data,string,{pin_hash}",
        f"board,data,string,{board}",
        "",
    ])


def build_nvs_bin(csv_text: str) -> bytes:
    """Run esp-idf-nvs-partition-gen over the CSV. It only speaks files,
    so use a throwaway dir under DATA_DIR (the systemd sandbox allows
    /opt/pip/data and private /tmp; tempfile picks the latter locally)."""
    from esp_idf_nvs_partition_gen import nvs_partition_gen
    with tempfile.TemporaryDirectory(dir=db.DATA_DIR) as d:
        csv_path = os.path.join(d, "nvs.csv")
        out_path = os.path.join(d, "nvs.bin")
        with open(csv_path, "w") as f:
            f.write(csv_text)
        nvs_partition_gen.generate(SimpleNamespace(
            input=csv_path, output=out_path, size=hex(NVS_SIZE),
            outdir=d, version=2))
        with open(out_path, "rb") as f:
            return f.read()


# ---------- NVS stash (nonce -> freshly built image) ----------
_STASH_TTL = 30 * 60
_stash: dict[str, tuple[str, bytes, float]] = {}
_stash_lock = threading.Lock()


def stash_put(device_id: str, blob: bytes) -> str:
    nonce = secrets.token_urlsafe(24)
    now = time.time()
    with _stash_lock:
        for k in [k for k, v in _stash.items()
                  if v[2] < now or v[0] == device_id]:
            del _stash[k]            # expire, and one live image per device
        _stash[nonce] = (device_id, blob, now + _STASH_TTL)
    return nonce


def stash_get(nonce: str):
    """Returns (device_id, bytes) or None. Multiple reads within the TTL
    are allowed - flashing retries re-fetch the same image."""
    with _stash_lock:
        v = _stash.get(nonce)
        if not v or v[2] < time.time():
            _stash.pop(nonce, None)
            return None
        return v[0], v[1]


# ---------- device + user creation ----------
def insert_device(c, device_id: str, user_id: int, admin_id=None,
                  board: str = boards.DEFAULT_BOARD) -> tuple[str, str]:
    """Insert the device row; returns (token_raw, mqtt_pass). Caller owns
    the transaction and calls mqtt.provision_device once it commits."""
    token_raw, token_h = new_token()
    mqtt_pass = secrets.token_urlsafe(12)
    c.execute("""INSERT INTO devices (id,user_id,token_hash,mqtt_password,
                 admin_id,board) VALUES (?,?,?,?,?,?)""",
              (device_id, user_id, token_h, mqtt_pass, admin_id, board))
    return token_raw, mqtt_pass


def derive_device_id(c, username: str) -> str:
    base = "pip-" + username.replace("_", "-")
    base = re.sub(r"-+", "-", base)[:28].rstrip("-")
    device_id = base
    n = 1
    while db.one(c, "SELECT 1 FROM devices WHERE id=?", (device_id,)):
        n += 1
        device_id = f"{base}-{n}"
    return device_id


def create_device_user(creator_id: int, name: str, username: str,
                       pin: str, base_url: str = "",
                       board: str = boards.DEFAULT_BOARD) -> dict:
    """PWA setup flow: new device user + device, creator becomes device
    admin and first contact (symmetric perms). Returns ids + the NVS
    stash nonce for the browser flasher. The board (hardware model) is
    recorded on the device row and baked into the NVS image - it is the
    fact that keeps per-model OTA from ever cross-flashing a box."""
    name = name.strip()
    username = username.strip().lstrip("@").lower()
    pin = pin.strip()
    if not (1 <= len(name) <= 23):
        raise HTTPException(400, "name must be 1-23 characters")
    if not re.fullmatch(r"[a-z0-9_]{2,24}", username):
        raise HTTPException(400, "username must be 2-24 characters: "
                                 "a-z, 0-9, _")
    if not re.fullmatch(r"\d{4}", pin):
        raise HTTPException(400, "Settings PIN must be 4 digits")
    if not boards.valid(board):
        raise HTTPException(400, "unknown board model")

    with db.conn() as c:
        if db.one(c, "SELECT 1 FROM users WHERE username=?", (username,)):
            raise HTTPException(409, "that @username is taken")
        used = {u["color"] for u in db.all_(c, "SELECT color FROM users")}
        free = [x for x in PALETTE if x not in used] or PALETTE
        c.execute("""INSERT INTO users
                     (username,display_name,color,password_hash,email,is_admin)
                     VALUES (?,?,?,'',NULL,0)""",
                  (username, name, secrets.choice(free)))
        uid = db.one(c, "SELECT id FROM users WHERE username=?",
                     (username,))["id"]
        device_id = derive_device_id(c, username)
        token_raw, mqtt_pass = insert_device(c, device_id, uid,
                                             admin_id=creator_id, board=board)
        # the creator is the box's first contact, both directions
        c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)", (uid, creator_id))
        c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)", (creator_id, uid))

    mqtt.provision_device(device_id, mqtt_pass)
    blob = build_nvs_bin(build_nvs_csv(device_id, name, token_raw,
                                       mqtt_pass, pin, base_url, board))
    return {"device_id": device_id, "user_id": uid, "username": username,
            "nvs_nonce": stash_put(device_id, blob)}


def rekey_device(device_id: str, device_name: str, pin: str,
                 base_url: str = "") -> str:
    """Mint a fresh token for an existing device and stash a new NVS
    image; the old token stops working. mqtt_pass is kept (stored in the
    DB). Returns the stash nonce."""
    pin = pin.strip()
    if not re.fullmatch(r"\d{4}", pin):
        raise HTTPException(400, "Settings PIN must be 4 digits")
    token_raw, token_h = new_token()
    with db.conn() as c:
        dev = db.one(c, "SELECT mqtt_password, board FROM devices WHERE id=?",
                     (device_id,))
        if not dev:
            raise HTTPException(404, "no such device")
        c.execute("UPDATE devices SET token_hash=? WHERE id=?",
                  (token_h, device_id))
        mqtt_pass = dev["mqtt_password"]
    # broker creds unchanged but re-assert them (repairs a lost passwd file)
    mqtt.provision_device(device_id, mqtt_pass)
    # board comes from the row, never the caller: the hardware model of an
    # existing box is a fact, not a choice - a rekey must not change it
    blob = build_nvs_bin(build_nvs_csv(device_id, device_name, token_raw,
                                       mqtt_pass, pin, base_url,
                                       dev["board"]))
    return stash_put(device_id, blob)


# ---------- flash bundle (bootloader / partition table / otadata) ----------
ASSET_KINDS = ("bootloader", "parttable")


def asset_path(version: str, kind: str,
               board: str = "amoled-1.8") -> str:
    """Default board keeps the bare legacy names (pre-multi-SKU files)."""
    if board == "amoled-1.8":
        return os.path.join(db.FIRMWARE_DIR, f"{version}.{kind}.bin")
    return os.path.join(db.FIRMWARE_DIR, f"{version}.{board}.{kind}.bin")


def flash_assets_version(board: str = "amoled-1.8"):
    """Version whose bootloader+partition-table files exist on disk for
    this board: the active firmware's if complete, else the newest
    complete set (they change ~never; an app-only upload must not break
    flashing). Returns None when no complete set exists."""
    with db.conn() as c:
        rows = db.all_(c, """SELECT version FROM firmware WHERE board=?
                             ORDER BY active DESC, created DESC""", (board,))
    for r in rows:
        if all(os.path.exists(asset_path(r["version"], k, board))
               for k in ASSET_KINDS):
            return r["version"]
    return None
