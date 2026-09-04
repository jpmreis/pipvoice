"""api: /v1 endpoints. Serves both device tokens and PWA user session
tokens (cookie or Bearer) through the same identity abstraction — a
message always flows user -> user, so the sender's client never matters."""
import asyncio
import hashlib
import logging
import os
import re
import secrets
import time
from datetime import datetime, timezone

from fastapi import (APIRouter, BackgroundTasks, Body, Form, HTTPException,
                     Request, UploadFile)
from fastapi.responses import FileResponse, JSONResponse, Response

from . import (boards, db, emails, mqtt, notify, presence, provision, push,
               themes, vmsg, voice)
from .auth import (LOCAL_AUTH, AuthDep, Identity, client_ip, create_session,
                   issue_login_code, login_blocked, login_failed,
                   login_succeeded, redeem_login_code, token_hash,
                   user_by_email, verify_password)

router = APIRouter(prefix="/v1")

log = logging.getLogger("api")

MAX_AUDIO_BYTES = 4 * 1024 * 1024
MAX_MSG_S = int(db.env("MAX_MSG_S", "90"))   # mirrors device max_message_s
RATE_MSGS = 5          # per sender->recipient pair ...
RATE_WINDOW_MIN = 5    # ... within this many minutes

# reaction keys as sent over the wire; display glyphs are a client concern
REACTIONS = {"heart", "up", "down", "haha", "bang", "quest", "joy"}

COOKIE_SECURE = (db.env("BASE_URL", "") or "").startswith("https")

# The fleet version lives in exactly ONE variable: PROJECT_VER in
# firmware/CMakeLists.txt. It is baked into the binary, lands in the
# firmware table on upload, and the active row is what every client checks
# - devices via the OTA manifest, the PWA via /v1/version and the
# X-Pip-Version header. A web-only release therefore also ships as a
# version bump + firmware upload/activate: one variable, one bump event,
# both platforms refresh together (deliberate - global versioning over
# per-half content hashes).
_gv_cache = ("", 0.0)


def global_version() -> str:
    global _gv_cache
    val, ts = _gv_cache
    if not val or time.monotonic() - ts > 5.0:   # admin activation shows <=5s late
        with db.conn() as c:
            fw = db.one(c, "SELECT version FROM firmware WHERE active=1 "
                           "ORDER BY created DESC LIMIT 1")
        val = fw["version"] if fw else "0.0.0"
        _gv_cache = (val, time.monotonic())
    return val


def version_bumped() -> None:
    """Drop the cache so a just-activated firmware shows up immediately."""
    global _gv_cache
    _gv_cache = ("", 0.0)


@router.get("/version")
def version():
    """Unauthenticated on purpose: the login screen self-updates too."""
    return {"version": global_version()}


@router.get("/webver")
def webver():
    """Deprecated in favor of /version; kept so PWAs still running an
    older shell see the value change on the next activation and reload."""
    return {"version": global_version()}


def _set_session_cookie(resp: Response, token: str) -> None:
    resp.set_cookie("pip_session", token, httponly=True, secure=COOKIE_SECURE,
                    samesite="lax", max_age=30 * 86400, path="/")


def _fmt_when(iso: str) -> str:
    try:
        dt = datetime.fromisoformat(iso)
        return dt.strftime("%a %H:%M")
    except Exception:
        return iso[:16]


def _epoch(iso: str) -> int:
    """sqlite datetime('now') strings are UTC; return unix seconds."""
    try:
        return int(datetime.fromisoformat(iso)
                   .replace(tzinfo=timezone.utc).timestamp())
    except Exception:
        return 0


# ---------- auth (phone users / PWA) ----------
def _login_response(u) -> JSONResponse:
    token = create_session(u["id"])
    t = themes.get(u["theme"]) if u["theme"] else None
    resp = JSONResponse({"token": token, "username": u["username"],
                         "display_name": u["display_name"],
                         "color": u["color"],
                         "theme": t["name"] if t else None,
                         "theme_fg": t["fg"] if t else None,
                         "theme_ver": themes.version_of(t["name"]) if t else None,
                         "managed": _managed_summary(u["id"])})
    _set_session_cookie(resp, token)
    return resp


@router.get("/auth/methods")
def auth_methods():
    """Unauthenticated: tells login screens what to offer. Email codes
    need SMTP; passwords need PIP_LOCAL_AUTH=1 (self-host); the waitlist
    link only exists on the hosted instance (db.hosted())."""
    return {"code": bool(db.env("SMTP_HOST", "")), "password": LOCAL_AUTH,
            "waitlist": db.hosted()}


@router.post("/auth/login-password")
def login_password(request: Request, email: str = Form(...),
                   password: str = Form(...)):
    """Self-host only (PIP_LOCAL_AUTH=1): local-password sign-in for
    users whose password the admin has set."""
    if not LOCAL_AUTH:
        raise HTTPException(404, "password login is not enabled")
    addr = email.strip().lower()
    rl_key = f"pw:{client_ip(request)}:{addr}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many attempts - try again later")
    u = user_by_email(addr)
    if not u or not u["password_hash"] \
            or not verify_password(password, u["password_hash"]):
        login_failed(rl_key)
        raise HTTPException(401, "wrong email or password")
    login_succeeded(rl_key)
    return _login_response(u)


@router.post("/auth/request-code")
def request_code(request: Request, email: str = Form(...)):
    """Email a 6-digit login code. The response is identical whether or not
    the address matches a user - no account enumeration."""
    rl_key = f"code-req:{client_ip(request)}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many attempts - try again later")
    login_failed(rl_key)             # every send counts toward the 5/15min cap
    u = user_by_email(email)
    if u:
        emails.send_login_code(u["id"], u["display_name"],
                               issue_login_code(u["id"]))
    return {"ok": True}


@router.post("/auth/verify-code")
def verify_code(request: Request, email: str = Form(...),
                code: str = Form(...)):
    addr = email.strip().lower()
    rl_key = f"code-ver:{client_ip(request)}:{addr}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many attempts - try again later")
    u = user_by_email(addr)
    if not u or not redeem_login_code(u["id"], code):
        login_failed(rl_key)
        raise HTTPException(401, "wrong or expired code")
    login_succeeded(rl_key)
    return _login_response(u)


# conservative shape check; the real cap is the parameterized INSERT
_EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]+\.[^@\s]{2,}$")


def _waitlist_notify_admins(addr: str) -> None:
    """Heads-up mail to every admin with an email on file. Best-effort:
    the signup row is already stored either way."""
    with db.conn() as c:
        admins = db.all_(c, """SELECT id FROM users
                               WHERE is_admin=1 AND email IS NOT NULL
                                 AND email!=''""")
    if not admins:
        log.warning("waitlist signup: no admin has an email address set, "
                    "nobody was notified")
        return
    for a in admins:
        notify.send_email(
            a["id"], "New Pip waitlist signup",
            f"Someone joined the Pip waitlist: {addr}\n\n"
            "Signups live in the waitlist table; add them as a user from\n"
            f"{notify.app_url().replace('/app/', '/admin')} when a spot "
            "opens up.\n")


@router.post("/waitlist")
def waitlist_join(request: Request, background: BackgroundTasks,
                  email: str = Form(...)):
    """Public waitlist signup (hosted instance only). Same limiter as
    login; the response is identical whether or not the address was
    already listed."""
    if not db.hosted():
        raise HTTPException(404, "waitlist is not enabled")
    rl_key = f"waitlist:{client_ip(request)}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many attempts - try again later")
    login_failed(rl_key)             # every submit counts toward the cap
    addr = email.strip().lower()
    if len(addr) > 254 or not _EMAIL_RE.fullmatch(addr):
        raise HTTPException(400, "not a valid email address")
    with db.conn() as c:
        new = c.execute("INSERT OR IGNORE INTO waitlist (email) VALUES (?)",
                        (addr,)).rowcount > 0
    if new:   # repeat submits of a listed address don't re-mail the admins
        background.add_task(_waitlist_notify_admins, addr)
    return {"ok": True}


@router.post("/auth/logout")
def logout(request: Request, ident: Identity = AuthDep):
    sid = request.cookies.get("pip_session", "")
    if sid:
        with db.conn() as c:
            c.execute("DELETE FROM sessions WHERE token_hash=?",
                      (token_hash(sid),))
    resp = JSONResponse({"ok": True})
    resp.delete_cookie("pip_session", path="/")
    return resp


@router.get("/me")
def me(ident: Identity = AuthDep):
    with db.conn() as c:
        row = db.one(c, "SELECT theme FROM users WHERE id=?", (ident.user_id,))
    t = themes.get(row["theme"]) if row and row["theme"] else None
    return {"username": ident.username, "display_name": ident.display_name,
            "color": ident.color,
            "theme": t["name"] if t else None,
            "theme_fg": t["fg"] if t else None,
            "theme_ver": themes.version_of(t["name"]) if t else None,
            "managed": _managed_summary(ident.user_id)}


# ---------- background themes ----------
@router.get("/themes")
def theme_list(ident: Identity = AuthDep):
    return [{"name": t["name"], "label": t["label"], "fg": t["fg"],
             "ver": themes.version_of(t["name"])}
            for t in themes.available()]


def _theme_asset(name: str, path: str, media: str, v: str | None):
    """Theme images are immutable per version: a ?v= URL that matches the
    current content hash may be cached forever (a master change flips the
    hash, so clients switch URLs); anything else must revalidate (cheap
    304 via FileResponse's ETag) so version-less clients never go stale."""
    if not themes.get(name) or not os.path.exists(path):
        raise HTTPException(404, "unknown theme")
    resp = FileResponse(path, media_type=media)
    resp.headers["Cache-Control"] = (
        "public, max-age=31536000, immutable"
        if v and v == themes.version_of(name) else "no-cache")
    return resp


@router.post("/theme")
def theme_set(ident: Identity = AuthDep, body: dict = Body(...)):
    """Persist the PWA user's pick (devices keep theirs in NVS)."""
    name = body.get("name")
    if name is not None and not themes.get(name):
        raise HTTPException(404, "unknown theme")
    with db.conn() as c:
        c.execute("UPDATE users SET theme=? WHERE id=?", (name, ident.user_id))
    return {"ok": True}


@router.get("/themes/{name}/device.bin")
def theme_device(name: str, ident: Identity = AuthDep, v: str = None):
    return _theme_asset(name, themes.device_path(name),
                        "application/octet-stream", v)


@router.get("/themes/{name}/thumb.bin")
def theme_thumb(name: str, ident: Identity = AuthDep, v: str = None):
    return _theme_asset(name, themes.thumb_path(name),
                        "application/octet-stream", v)


@router.get("/themes/{name}/web.jpg")
def theme_web(name: str, ident: Identity = AuthDep, v: str = None):
    return _theme_asset(name, themes.web_path(name), "image/jpeg", v)


# ---------- per-device config (voice control) ----------
# The first device-scoped endpoint: everything else is user-scoped, but
# an accessibility mode belongs to one physical box, not to the user's
# account. Fetched by sync.c alongside its contacts refresh.

@router.get("/device")
def device_config(ident: Identity = AuthDep):
    if not ident.device_id:
        raise HTTPException(404, "not a device token")
    with db.conn() as c:
        row = db.one(c, "SELECT voice FROM devices WHERE id=?",
                     (ident.device_id,))
    enabled = bool(row and row["voice"])
    if enabled:
        # self-heal: render anything missing (renamed contact, new
        # phrase) off-thread; the box is notified when new clips land
        voice.ensure_user(ident.user_id)
    return {"voice": enabled,
            "prompts": voice.manifest(ident.user_id) if enabled else []}


@router.get("/voice/{key}.vmsg")
def voice_prompt(key: str, ident: Identity = AuthDep, v: str = None):
    """Spoken-prompt clip, addressed by key + content-hash version the
    /device manifest listed. Immutable per version, like theme assets."""
    if not re.fullmatch(r"[a-z0-9_.-]{1,64}", key) or \
       not re.fullmatch(r"[0-9a-f]{8}", v or ""):
        raise HTTPException(404, "no such prompt")
    path = voice.prompt_path(key, v)
    if not os.path.exists(path):
        raise HTTPException(404, "no such prompt")
    resp = FileResponse(path, media_type="application/octet-stream")
    resp.headers["Cache-Control"] = "public, max-age=31536000, immutable"
    return resp


# ---------- contacts ----------
@router.get("/contacts")
def contacts(ident: Identity = AuthDep):
    with db.conn() as c:
        rows = db.all_(c, """SELECT u.username, u.display_name, u.color
                             FROM perms p JOIN users u ON u.id=p.recipient
                             WHERE p.sender=? AND p.recipient != p.sender
                             ORDER BY u.display_name""",
                       (ident.user_id,))
    # field named device_id for firmware compatibility; value is the username
    return [{"device_id": r["username"], "name": r["display_name"],
             "color": r["color"]} for r in rows]


# ---------- device admin: manage a device's contact list ----------
# The device admin (devices.admin_id) edits which users their device can
# exchange messages with, from the PWA. Perms are symmetric, so adding or
# removing a contact always writes/deletes both directions.

def _managed_rows(c, user_id: int):
    return db.all_(c, """SELECT d.id AS device_id, d.last_seen, d.voice,
                                u.id AS uid, u.username, u.display_name
                         FROM devices d JOIN users u ON u.id=d.user_id
                         WHERE d.admin_id=? ORDER BY d.id""", (user_id,))


def _managed_summary(user_id: int):
    with db.conn() as c:
        rows = _managed_rows(c, user_id)
    return [{"device_id": r["device_id"], "username": r["username"],
             "name": r["display_name"]} for r in rows]


def _notify_contacts_changed(*user_ids: int) -> None:
    """Tell each user's boxes to refetch contacts now instead of at the
    24 h mark. NOT retained - a per-device retained message would replace
    the retained firmware notify on the same topic and break OTA-on-
    reconnect; offline boxes refresh contacts at boot anyway. No-op for
    phone users (no device rows) and on firmware <=0.1.25 (unknown event
    degrades to a plain sync)."""
    for uid in set(user_ids):
        mqtt.notify_user(uid, '{"event":"contacts"}')


def _managed_device(c, device_id: str, ident: Identity):
    dev = db.one(c, """SELECT d.user_id, u.username, u.display_name
                       FROM devices d JOIN users u ON u.id=d.user_id
                       WHERE d.id=? AND d.admin_id=?""",
                 (device_id, ident.user_id))
    if not dev:
        raise HTTPException(404, "not a device you manage")
    return dev


def _flashable_device(c, device_id: str, ident: Identity):
    """Flash paths only: the device admin, or any server admin (the admin
    Devices page links here so a box can be web-flashed without first
    reassigning its device admin). Contact editing stays device-admin-only
    - being able to reflash a box is not a licence to edit its perms."""
    if ident.is_admin:
        dev = db.one(c, """SELECT d.user_id, u.username, u.display_name
                           FROM devices d JOIN users u ON u.id=d.user_id
                           WHERE d.id=?""", (device_id,))
        if not dev:
            raise HTTPException(404, "no such device")
        return dev
    return _managed_device(c, device_id, ident)


@router.get("/managed")
def managed_list(ident: Identity = AuthDep):
    with db.conn() as c:
        out = []
        for r in _managed_rows(c, ident.user_id):
            contacts = db.all_(c, """SELECT u.username, u.display_name, u.color
                                     FROM perms p JOIN users u ON u.id=p.recipient
                                     WHERE p.sender=? ORDER BY u.display_name""",
                               (r["uid"],))
            out.append({"device_id": r["device_id"], "username": r["username"],
                        "name": r["display_name"],
                        "last_seen": r["last_seen"],   # setup page: "online?"
                        "voice": bool(r["voice"]),     # accessibility toggle
                        "contacts": [{"username": x["username"],
                                      "name": x["display_name"],
                                      "color": x["color"]} for x in contacts]})
    return out


@router.post("/managed/{device_id}/voice")
def managed_voice(device_id: str, ident: Identity = AuthDep,
                  body: dict = Body(...)):
    """Device admin's twin of the admin-page toggle: hands-free voice
    control for this box. Takes effect on the box within seconds via a
    non-retained notify; prompt clips render in the background."""
    on = bool(body.get("on"))
    with db.conn() as c:
        dev = _managed_device(c, device_id, ident)
        c.execute("UPDATE devices SET voice=? WHERE id=?",
                  (1 if on else 0, device_id))
    if on:
        voice.ensure_user(dev["user_id"])
    mqtt.notify_user(dev["user_id"], '{"event":"voice"}')
    return {"ok": True, "voice": on}


@router.post("/managed/{device_id}/contacts")
def managed_add(device_id: str, ident: Identity = AuthDep,
                body: dict = Body(...)):
    """Add a contact by exact @username (typed, never picked from a list).
    Invalid tries are rate limited so the endpoint can't be used to fish
    for usernames."""
    rl_key = f"addcontact:{ident.user_id}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many attempts - try again later")
    handle = str(body.get("username", "")).strip().lstrip("@").lower()
    with db.conn() as c:
        dev = _managed_device(c, device_id, ident)
        target = db.one(c, "SELECT id,username,display_name,color FROM users "
                           "WHERE username=?", (handle,)) if handle else None
        if not target:
            login_failed(rl_key)
            raise HTTPException(404, "no user with that @username")
        if target["id"] == dev["user_id"]:
            login_failed(rl_key)
            raise HTTPException(400, "that is this device's own @username")
        existed = db.one(c, "SELECT 1 FROM perms WHERE sender=? AND recipient=?",
                         (dev["user_id"], target["id"])) is not None
        c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)",
                  (dev["user_id"], target["id"]))
        c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)",
                  (target["id"], dev["user_id"]))
    if not existed:
        _notify_contacts_changed(dev["user_id"], target["id"])
        voice.ensure_all()      # a voice box may need the new name clip
    return {"ok": True, "existed": existed,
            "contact": {"username": target["username"],
                        "name": target["display_name"],
                        "color": target["color"]}}


@router.delete("/managed/{device_id}/contacts/{username}")
def managed_remove(device_id: str, username: str, ident: Identity = AuthDep):
    with db.conn() as c:
        dev = _managed_device(c, device_id, ident)
        target = db.one(c, "SELECT id FROM users WHERE username=?",
                        (username.lstrip("@").lower(),))
        if not target:
            raise HTTPException(404, "no such user")
        c.execute("""DELETE FROM perms WHERE (sender=? AND recipient=?)
                                          OR (sender=? AND recipient=?)""",
                  (dev["user_id"], target["id"],
                   target["id"], dev["user_id"]))
    _notify_contacts_changed(dev["user_id"], target["id"])
    return {"ok": True}


# ---------- device setup (PWA "set up a new Pip" + web flasher) ----------
# A PWA user creates a device user + device (becoming its device admin)
# and flashes the box from the browser over Web Serial. The manifest
# mirrors firmware/partitions.csv; the chip gates mirror
# sdkconfig.defaults (ESP32-S3, 8 MB octal PSRAM, 16 MB flash).

def _no_device_tokens(ident: Identity) -> None:
    """Boxes must not be able to mint or re-key devices."""
    if ident.device_id is not None:
        raise HTTPException(403, "not available to devices")


def _flash_manifest(device_id: str, nonce: str,
                    board: str = boards.DEFAULT_BOARD) -> dict:
    b = boards.BOARDS[board]
    with db.conn() as c:
        fw = db.one(c, "SELECT version FROM firmware WHERE active=1 "
                       "AND board=? ORDER BY created DESC LIMIT 1", (board,))
    out = {"device_id": device_id, "version": fw["version"] if fw else None,
           "board": board, "board_name": b["full_name"],
           "board_label": b["label"],
           "chip": {"family": "ESP32-S3", "psramCap": 1, "minFlashMB": 16},
           "flash": {"mode": "dio", "freqMHz": 80, "sizeMB": 16},
           "problem": None, "parts": []}
    assets_v = provision.flash_assets_version()
    if not fw:
        out["problem"] = (f"no firmware for the {b['label']} model yet"
                          if board != boards.DEFAULT_BOARD
                          else "no active firmware on the server")
    elif not assets_v:
        out["problem"] = ("the server has no bootloader/partition-table "
                          "files yet - upload them on the admin Firmware "
                          "page")
    else:
        out["parts"] = [
            {"name": "bootloader", "offset": 0x0,
             "url": f"/v1/setup/asset/{assets_v}/bootloader.bin"},
            {"name": "partition-table", "offset": 0x8000,
             "url": f"/v1/setup/asset/{assets_v}/parttable.bin"},
            {"name": "nvs", "offset": 0x9000,
             "url": f"/v1/setup/nvs/{nonce}.bin"},
            {"name": "otadata", "offset": 0xF000,
             "url": "/v1/setup/asset/-/otadata.bin"},
            {"name": "app", "offset": 0x20000,
             "url": f"/v1/firmware/{fw['version']}.bin"},
        ]
    return out


@router.get("/setup/boards")
def setup_boards(ident: Identity = AuthDep):
    """The model catalog for the setup page's picker. A model is
    flashable only once the server holds an active firmware build for
    it - the picker greys out the rest."""
    _no_device_tokens(ident)
    with db.conn() as c:
        have = {r["board"] for r in db.all_(
            c, "SELECT DISTINCT board FROM firmware WHERE active=1")}
    return [{"board": key, "label": b["label"], "name": b["full_name"],
             "blurb": b["blurb"], "screen": b["screen"],
             "img": f"boards/{key}.jpg", "img_back": f"boards/{key}-back.jpg",
             "available": key in have}
            for key, b in boards.BOARDS.items()]


@router.post("/setup/device")
def setup_device(ident: Identity = AuthDep, body: dict = Body(...)):
    """Create a device user + device; the caller becomes device admin and
    first contact. Returns the flash manifest for the new box. The
    chosen board model is recorded on the device row and in the NVS
    image - OTA serves only that model's builds from then on."""
    _no_device_tokens(ident)
    rl_key = f"setup:{ident.user_id}"
    if login_blocked(rl_key):
        raise HTTPException(429, "too many devices created - try again later")
    login_failed(rl_key)     # every attempt counts toward the cap
    board = str(body.get("board", "") or boards.DEFAULT_BOARD)
    created = provision.create_device_user(
        ident.user_id, str(body.get("name", "")),
        str(body.get("username", "")), str(body.get("pin", "")),
        str(body.get("server_url", "")), board)
    _notify_contacts_changed(ident.user_id, created["user_id"])
    return {"device_id": created["device_id"],
            "username": created["username"],
            "manifest": _flash_manifest(created["device_id"],
                                        created["nvs_nonce"], board)}


@router.post("/setup/{device_id}/rekey")
def setup_rekey(device_id: str, ident: Identity = AuthDep,
                body: dict = Body(...)):
    """Fresh token + NVS image for a device the caller manages (resume an
    interrupted setup, or deliberately re-flash). Invalidates the box's
    current credentials - it must be (re)flashed afterwards."""
    _no_device_tokens(ident)
    with db.conn() as c:
        dev = _flashable_device(c, device_id, ident)
        board = db.one(c, "SELECT board FROM devices WHERE id=?",
                       (device_id,))["board"]
    nonce = provision.rekey_device(device_id, dev["display_name"],
                                   str(body.get("pin", "")),
                                   str(body.get("server_url", "")))
    # name: a deep-linked flash (?flash=<id>) may target a device outside
    # the caller's /managed list, so the flasher can't look it up there
    return {"device_id": device_id, "name": dev["display_name"],
            "manifest": _flash_manifest(device_id, nonce, board)}


@router.get("/setup/{device_id}/online")
def setup_online(device_id: str, ident: Identity = AuthDep):
    """Post-flash poll: last_seen newer than the flash start means the box
    booted, onboarded and authenticated. Same audience as the flash paths
    (an admin-flashed device may not be in the caller's /managed list)."""
    _no_device_tokens(ident)
    with db.conn() as c:
        _flashable_device(c, device_id, ident)
        dev = db.one(c, "SELECT last_seen FROM devices WHERE id=?",
                     (device_id,))
    return {"last_seen": dev["last_seen"]}


@router.get("/setup/asset/{version}/{name}.bin")
def setup_asset(version: str, name: str, ident: Identity = AuthDep):
    if name == "otadata":
        # 0x2000 of 0xFF = erased otadata: the bootloader boots ota_0
        return Response(b"\xff" * 0x2000,
                        media_type="application/octet-stream")
    if name not in provision.ASSET_KINDS or \
            not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", version):
        raise HTTPException(404, "no such asset")
    path = provision.asset_path(version, name)
    if not os.path.exists(path):
        raise HTTPException(404, "no such asset")
    return FileResponse(path, media_type="application/octet-stream")


@router.get("/setup/nvs/{nonce}.bin")
def setup_nvs(nonce: str, ident: Identity = AuthDep):
    _no_device_tokens(ident)
    got = provision.stash_get(nonce)
    if not got:
        raise HTTPException(410, "flash image expired - re-key the device")
    device_id, blob = got
    with db.conn() as c:
        _flashable_device(c, device_id, ident)  # device admin or server admin
    return Response(blob, media_type="application/octet-stream")


# ---------- send ----------
def _write_audio(msg_id: str, data: bytes) -> None:
    with open(db.audio_path(msg_id), "wb") as f:
        f.write(data)


@router.post("/messages")
async def send_message(bg: BackgroundTasks,
                       ident: Identity = AuthDep,
                       recipient_id: str = Form(...),
                       duration: int = Form(0),
                       audio: UploadFile = None):
    if audio is None:
        raise HTTPException(400, "audio file required")
    with db.conn() as c:
        rcpt = db.one(c, "SELECT id FROM users WHERE username=?", (recipient_id,))
        if not rcpt:
            raise HTTPException(404, "unknown recipient")
        allowed = db.one(c, "SELECT 1 FROM perms WHERE sender=? AND recipient=?",
                         (ident.user_id, rcpt["id"]))
        if not allowed:
            raise HTTPException(403, "not permitted to message this user")
        # checked before the audio is even read; a 429'd device upload stays
        # in its outbox and retries after the window, so nothing is lost
        recent = db.one(c, f"""SELECT COUNT(*) n FROM messages
                               WHERE sender=? AND recipient=?
                                 AND created > datetime('now','-{RATE_WINDOW_MIN} minutes')""",
                        (ident.user_id, rcpt["id"]))["n"]
        if recent >= RATE_MSGS:
            raise HTTPException(
                429, f"max {RATE_MSGS} messages per contact "
                     f"per {RATE_WINDOW_MIN} minutes")

    data = await audio.read()
    if len(data) > MAX_AUDIO_BYTES:
        raise HTTPException(413, "audio too large")
    if not data.startswith(b"VMSG"):
        # browser upload (AAC/MP4, WebM/Opus, ...): transcode to the
        # firmware's container; duration is recomputed server-side.
        # Off-thread: this forks ffmpeg and then Opus-encodes the whole
        # recording, and there is exactly one uvicorn worker - doing it on
        # the event loop stalled every other request, including the
        # recipient's inbox fetch that our own push had just triggered.
        try:
            data, duration = await asyncio.to_thread(
                vmsg.transcode_to_vmsg, data, MAX_MSG_S)
        except ValueError as e:
            raise HTTPException(400, str(e))
    else:
        # box recording: level-normalize at ingest (decode -> gain/limit ->
        # re-encode; see vmsg.normalize_pcm). Same single-worker reasoning
        # as above. Never fatal - on any failure the original bytes stand.
        data = await asyncio.to_thread(vmsg.normalize_vmsg, data)

    # ms-timestamp prefix + random suffix: still 32 hex chars, but ids sort
    # chronologically - device inbox ordering relies on this (storage.c)
    msg_id = f"{int(time.time() * 1000):016x}{secrets.token_hex(8)}"
    await asyncio.to_thread(_write_audio, msg_id, data)
    with db.conn() as c:
        c.execute("""INSERT INTO messages (id,sender,recipient,duration)
                     VALUES (?,?,?,?)""",
                  (msg_id, ident.user_id, rcpt["id"], duration))
    # After the response, in a worker thread: notifying means a webpush per
    # subscription at up to 10 s each, and the sender has no reason to wait
    # on it. The audio and the row are already in place, so the promise a
    # notify makes still holds the moment it fires.
    bg.add_task(notify.message_created, rcpt["id"], msg_id,
                ident.display_name)
    return {"id": msg_id}


# ---------- inbox ----------
@router.get("/inbox")
def inbox(ident: Identity = AuthDep):
    with db.conn() as c:
        rows = db.all_(c, """SELECT m.id, m.duration, m.created, m.delivered,
                                    u.username, u.display_name, u.color,
                                    r.reaction
                             FROM messages m JOIN users u ON u.id=m.sender
                             LEFT JOIN reactions r ON r.msg_id=m.id
                             WHERE m.recipient=? ORDER BY m.created DESC""",
                       (ident.user_id,))
    return [{"id": r["id"], "sender_id": r["username"],
             "sender_name": r["display_name"], "sender_color": r["color"],
             "when": _fmt_when(r["created"]), "ts": _epoch(r["created"]),
             "duration": r["duration"], "delivered": bool(r["delivered"]),
             "reaction": r["reaction"] or ""}
            for r in rows]


def _owned_message(ident: Identity, msg_id: str):
    with db.conn() as c:
        m = db.one(c, "SELECT * FROM messages WHERE id=? AND recipient=?",
                   (msg_id, ident.user_id))
    if not m:
        raise HTTPException(404, "no such message")
    return m


@router.get("/messages/{msg_id}/audio")
def download(msg_id: str, ident: Identity = AuthDep):
    _owned_message(ident, msg_id)
    path = db.audio_path(msg_id)
    if not os.path.exists(path):
        raise HTTPException(410, "audio expired")
    return FileResponse(path, media_type="application/octet-stream")


@router.get("/messages/{msg_id}/audio.m4a")
def download_m4a(msg_id: str, ident: Identity = AuthDep):
    """Browser playback. Normally rendered at send time (notify.py) and
    served straight off disk; rendered here on demand for messages that
    predate the .m4a cache, or whose render failed."""
    _owned_message(ident, msg_id)
    path = vmsg.ensure_playback(msg_id)
    if not path:
        raise HTTPException(410, "audio expired")
    # immutable by id: a message's audio never changes under the same URL,
    # so the service worker and the HTTP cache can both hold it forever
    return FileResponse(path, media_type="audio/mp4", headers={
        "Cache-Control": "private, max-age=31536000, immutable"})


@router.get("/messages/{msg_id}/audio.wav")
def download_wav(msg_id: str, ident: Identity = AuthDep):
    """Browser playback for clients still running an older cached app.js:
    the .vmsg decoded to WAV. New clients use audio.m4a."""
    _owned_message(ident, msg_id)
    path = db.audio_path(msg_id)
    if not os.path.exists(path):
        raise HTTPException(410, "audio expired")
    with open(path, "rb") as f:
        try:
            wav = vmsg.vmsg_to_wav(f.read())
        except ValueError as e:
            raise HTTPException(500, str(e))
    return Response(wav, media_type="audio/wav")


# ---------- web push (phone users) ----------
@router.get("/push/key")
def push_key(ident: Identity = AuthDep):
    return {"key": push.public_key()}


@router.post("/push/subscribe")
def push_subscribe(ident: Identity = AuthDep, sub: dict = Body(...)):
    if not sub.get("endpoint") or "keys" not in sub:
        raise HTTPException(400, "not a push subscription")
    push.save_subscription(ident.user_id, sub)
    return {"ok": True}


@router.post("/push/unsubscribe")
def push_unsubscribe(ident: Identity = AuthDep, sub: dict = Body(...)):
    if sub.get("endpoint"):
        push.drop_subscription(sub["endpoint"])
    return {"ok": True}


@router.post("/push/test")
def push_test(ident: Identity = AuthDep):
    """Self-test from the PWA settings screen."""
    n = push.send_to_user(ident.user_id, {
        "title": "Test", "body": "Notifications are working!", "msg_id": ""})
    return {"accepted": n}


@router.post("/messages/{msg_id}/ack")
def ack(msg_id: str, ident: Identity = AuthDep):
    _owned_message(ident, msg_id)
    with db.conn() as c:
        # delivered_at stays at the FIRST ack: it starts the grace window
        # after which cleanup deletes the audio (and, for phone users, the
        # message) from the server
        c.execute("""UPDATE messages SET delivered=1,
                       delivered_at=COALESCE(delivered_at, datetime('now'))
                     WHERE id=?""", (msg_id,))
    return {"ok": True}


# ---------- reactions ----------
@router.post("/messages/{msg_id}/reaction")
def react(msg_id: str, ident: Identity = AuthDep, body: dict = Body(...)):
    """Recipient reacts to a message; latest wins, "" clears."""
    key = body.get("reaction", "")
    if key and key not in REACTIONS:
        raise HTTPException(400, "unknown reaction")
    m = _owned_message(ident, msg_id)
    with db.conn() as c:
        if not key:
            c.execute("DELETE FROM reactions WHERE msg_id=?", (msg_id,))
            return {"ok": True}
        c.execute("""INSERT INTO reactions (msg_id,reactor,target,reaction)
                     VALUES (?,?,?,?)
                     ON CONFLICT(msg_id) DO UPDATE SET
                       reaction=excluded.reaction,
                       created=datetime('now'), seen=0""",
                  (msg_id, ident.user_id, m["sender"], key))
    notify.reaction_created(m["sender"], ident.display_name,
                            ident.username, key, msg_id)
    return {"ok": True}


@router.get("/reactions")
def reactions(ident: Identity = AuthDep):
    """Unseen reactions to messages the caller sent (for badges/toasts)."""
    with db.conn() as c:
        rows = db.all_(c, """SELECT r.msg_id, r.reaction, r.created,
                                    u.username, u.display_name
                             FROM reactions r JOIN users u ON u.id=r.reactor
                             WHERE r.target=? AND r.seen=0
                             ORDER BY r.created DESC""",
                       (ident.user_id,))
    return [{"msg_id": r["msg_id"], "from": r["username"],
             "from_name": r["display_name"], "reaction": r["reaction"],
             "ts": _epoch(r["created"])} for r in rows]


@router.post("/reactions/seen")
def reactions_seen(ident: Identity = AuthDep, body: dict = Body(...)):
    """Mark reactions seen, per reactor username or all at once."""
    with db.conn() as c:
        if body.get("all"):
            c.execute("UPDATE reactions SET seen=1 WHERE target=?",
                      (ident.user_id,))
        elif body.get("from"):
            c.execute("""UPDATE reactions SET seen=1 WHERE target=? AND
                         reactor=(SELECT id FROM users WHERE username=?)""",
                      (ident.user_id, body["from"]))
    return {"ok": True}


@router.delete("/messages/{msg_id}")
def delete(msg_id: str, ident: Identity = AuthDep):
    _owned_message(ident, msg_id)
    with db.conn() as c:
        c.execute("DELETE FROM messages WHERE id=?", (msg_id,))
    db.drop_audio(msg_id)
    return {"ok": True}


# ---------- recording presence ----------
@router.post("/presence")
def presence_set(ident: Identity = AuthDep, body: dict = Body(...)):
    """PWA path; devices publish presence/<id> over MQTT instead."""
    state = body.get("state", "")
    if state not in ("start", "stop"):
        raise HTTPException(400, "state must be start or stop")
    if not presence.set_recording(ident.user_id, body.get("to", ""), state):
        raise HTTPException(403, "not permitted")
    return {"ok": True}


@router.get("/presence")
def presence_get(ident: Identity = AuthDep):
    return presence.get_for(ident.user_id)


# ---------- firmware / OTA ----------
@router.get("/firmware")
def firmware_manifest(ident: Identity = AuthDep):
    """Per-model: a box is only ever offered builds for the board recorded
    on its device row at provisioning (ota.c updates on ANY version
    difference, so serving a foreign model's build would flash it). The
    retained firmware notify still fans out to the whole fleet - that is
    harmless, boxes just re-check here."""
    board = boards.DEFAULT_BOARD
    with db.conn() as c:
        if ident.device_id:
            row = db.one(c, "SELECT board FROM devices WHERE id=?",
                         (ident.device_id,))
            if row:
                board = row["board"]
        fw = db.one(c, "SELECT version FROM firmware WHERE active=1 "
                       "AND board=? ORDER BY created DESC LIMIT 1", (board,))
    base = (db.env("BASE_URL", "") or "").rstrip("/")
    if not fw or not base:   # no active build, or no absolute URL to offer
        return JSONResponse({"version": None})
    return {"version": fw["version"],
            "url": f"{base}/v1/firmware/{fw['version']}.bin"}


@router.get("/firmware/{version}.bin")
def firmware_download(version: str, ident: Identity = AuthDep):
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", version):
        raise HTTPException(400, "bad version")
    path = db.firmware_path(version)
    if not os.path.exists(path):
        raise HTTPException(404, "no such firmware")
    return FileResponse(path, media_type="application/octet-stream")
