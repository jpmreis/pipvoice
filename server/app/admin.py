"""admin: server-rendered management UI at /admin.
Users, devices (provision + NVS CSV), n-to-n permissions matrix, OTA."""
import os
import re
import secrets

from fastapi import APIRouter, Form, HTTPException, Request, UploadFile
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates

from urllib.parse import quote

from . import api, boards, db, emails, mqtt, provision, release, voice
from .auth import (LOCAL_AUTH, AdminDep, Identity, client_ip, create_session,
                   hash_password, issue_login_code, login_blocked,
                   login_failed, login_succeeded, redeem_login_code,
                   user_by_email, verify_password)
from .provision import PALETTE   # avatar colors; also used by PWA setup

router = APIRouter(prefix="/admin")
templates = Jinja2Templates(
    directory=os.path.join(os.path.dirname(__file__), "templates"))
# self-host local-password mode: every template can branch on it
templates.env.globals["local_auth"] = LOCAL_AUTH


def _page(request: Request, name: str, **ctx):
    return templates.TemplateResponse(request, name, ctx)


# ---------- login / bootstrap ----------
def _signed_in(user_id: int) -> RedirectResponse:
    resp = RedirectResponse("/admin", status_code=303)
    resp.set_cookie("pip_session", create_session(user_id), httponly=True,
                    samesite="strict", max_age=30 * 86400)
    return resp


@router.get("/login", response_class=HTMLResponse)
def login_page(request: Request):
    with db.conn() as c:
        empty = db.one(c, "SELECT COUNT(*) AS n FROM users")["n"] == 0
    return _page(request, "login.html", bootstrap=empty, stage="email",
                 email="", error=None)


@router.post("/login")
def login_post(request: Request, action: str = Form(...),
               email: str = Form(""), code: str = Form(""),
               username: str = Form(""), display_name: str = Form(""),
               password: str = Form("")):
    rl_key = f"admin-code:{client_ip(request)}"
    if login_blocked(rl_key):
        return _page(request, "login.html", bootstrap=False, stage="email",
                     email="",
                     error="Too many attempts - try again in 15 minutes")
    email = email.strip().lower()
    with db.conn() as c:
        empty = db.one(c, "SELECT COUNT(*) AS n FROM users")["n"] == 0

    if action == "bootstrap":        # first run: create the admin account
        if not empty:
            return RedirectResponse("/admin/login", status_code=303)
        username = username.strip().lower()
        pw_hash = ""
        if LOCAL_AUTH and password:  # self-host: admin can skip SMTP entirely
            if len(password) < 8:
                return _page(request, "login.html", bootstrap=True,
                             stage="email", email="",
                             error="Password needs at least 8 characters")
            pw_hash = hash_password(password)
        with db.conn() as c:
            c.execute("""INSERT INTO users
                         (username,display_name,color,password_hash,email,
                          is_admin) VALUES (?,?,?,?,?,1)""",
                      (username, display_name.strip() or username.title(),
                       "FFB300", pw_hash, email or None))
            u = db.one(c, "SELECT * FROM users WHERE username=?", (username,))
        return _signed_in(u["id"])

    if action == "password":         # self-host local-password sign-in
        if not LOCAL_AUTH:
            return RedirectResponse("/admin/login", status_code=303)
        u = user_by_email(email)
        if not u or not u["is_admin"] or not u["password_hash"] \
                or not verify_password(password, u["password_hash"]):
            login_failed(rl_key)
            return _page(request, "login.html", bootstrap=False,
                         stage="email", email=email,
                         error="Wrong email or password")
        login_succeeded(rl_key)
        return _signed_in(u["id"])

    if action == "request":
        login_failed(rl_key)         # every code email counts toward the cap
        u = user_by_email(email)
        if u and u["is_admin"]:
            emails.send_login_code(u["id"], u["display_name"],
                                   issue_login_code(u["id"]))
        # identical response whether or not the address matched an admin
        return _page(request, "login.html", bootstrap=False, stage="code",
                     email=email, error=None)

    u = user_by_email(email)         # action == "verify"
    if not u or not u["is_admin"] or not redeem_login_code(u["id"], code):
        login_failed(rl_key)
        return _page(request, "login.html", bootstrap=False, stage="code",
                     email=email, error="Wrong or expired code")
    login_succeeded(rl_key)
    return _signed_in(u["id"])


# ---------- dashboard ----------
def _size(n: int) -> str:
    """Bytes at whatever unit reads best - a family server can sit at a few
    hundred KB for weeks, and "0.0 MB" says nothing."""
    for unit, step in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= step:
            return f"{n / step:.1f} {unit}"
    return f"{n} B"


def _age(hours: float) -> str:
    """Compact age for the waiting list: minutes, hours, then days."""
    if hours < 1:
        return f"{max(int(hours * 60), 1)} min"
    if hours < 48:
        return f"{hours:.0f} h"
    return f"{hours / 24:.0f} d"


def _waiting(c) -> list[dict]:
    """Who is still owed messages, one row per recipient.

    Delivery is acked per message per *recipient*, not per client: the box
    acks after writing to its flash, a PWA after fetching. So a box maps to
    exactly one row here, while a phone user's row covers every PWA they
    have installed - the schema cannot say which install is behind, only
    how many are subscribed to push (0 is the interesting case: nothing on
    that account can be woken)."""
    # aggregate before joining devices: a second box on one account would
    # otherwise fan the message rows out and double every count
    rows = db.all_(c, """SELECT w.uid, u.display_name, d.id AS box,
                                d.last_seen, w.n, w.oldest_h
                         FROM (SELECT recipient AS uid, COUNT(*) AS n,
                                      (julianday('now') -
                                       julianday(MIN(created))) * 24
                                        AS oldest_h
                               FROM messages WHERE delivered = 0
                               GROUP BY recipient) w
                         JOIN users u ON u.id = w.uid
                         LEFT JOIN devices d ON d.user_id = w.uid
                         ORDER BY w.n DESC, w.oldest_h DESC""")
    installs = {r["user_id"]: r["n"] for r in
                db.all_(c, "SELECT user_id, COUNT(*) n FROM push_subs "
                           "GROUP BY user_id")}
    out = []
    for r in rows:
        subs = installs.get(r["uid"], 0)
        out.append({
            "target": r["box"] or r["display_name"],
            "owner": r["display_name"] if r["box"] else "",
            "kind": "box" if r["box"] else
                    (f"PWA, {subs} install{'' if subs == 1 else 's'}"
                     if subs else "PWA, no install subscribed"),
            "quiet": not r["box"] and not subs,
            "last_seen": r["last_seen"] if r["box"] else None,
            "n": r["n"],
            "oldest": _age(r["oldest_h"] or 0),
        })
    return out


@router.get("", response_class=HTMLResponse)
def dashboard(request: Request, ident: Identity = AdminDep):
    stored, stored_bytes = db.audio_usage()
    with db.conn() as c:
        stats = {
            "users": db.one(c, "SELECT COUNT(*) n FROM users")["n"],
            "devices": db.one(c, "SELECT COUNT(*) n FROM devices")["n"],
            "messages": db.one(c, "SELECT COUNT(*) n FROM messages")["n"],
            "pending": db.one(c, "SELECT COUNT(*) n FROM messages "
                                 "WHERE delivered=0")["n"],
            "stored": stored,
            "stored_size": _size(stored_bytes),
        }
        waiting = _waiting(c)
        devices = db.all_(c, """SELECT d.id, d.last_seen, u.display_name
                                FROM devices d JOIN users u ON u.id=d.user_id
                                ORDER BY d.id""")
    return _page(request, "dashboard.html", stats=stats, devices=devices,
                 waiting=waiting, me=ident)


# ---------- users ----------
@router.get("/users", response_class=HTMLResponse)
def users_page(request: Request, ident: Identity = AdminDep):
    with db.conn() as c:
        users = db.all_(c, "SELECT * FROM users ORDER BY username")
        device_users = {r["username"] for r in db.all_(
            c, "SELECT DISTINCT u.username FROM devices d "
               "JOIN users u ON u.id=d.user_id")}
        used = {u["color"] for u in users}
    # prefer colors nobody has yet for the random preselect
    free = [c for c in PALETTE if c not in used] or PALETTE
    return _page(request, "users.html", users=users,
                 device_users=device_users, palette=PALETTE,
                 random_color=secrets.choice(free), me=ident,
                 sent=request.query_params.get("sent"))


@router.post("/users")
def users_create(ident: Identity = AdminDep, username: str = Form(...),
                 display_name: str = Form(...), color: str = Form("4FC3F7"),
                 email: str = Form(""), is_admin: str = Form(None)):
    with db.conn() as c:
        c.execute("""INSERT INTO users
                     (username,display_name,color,password_hash,email,is_admin)
                     VALUES (?,?,?,'',?,?)""",
                  (username.strip().lower(), display_name,
                   color.lstrip("#").upper(),
                   email.strip().lower() or None, 1 if is_admin else 0))
    return RedirectResponse("/admin/users", status_code=303)


@router.post("/users/{user_id}/email")
def users_set_email(user_id: int, ident: Identity = AdminDep,
                    email: str = Form("")):
    with db.conn() as c:
        c.execute("UPDATE users SET email=? WHERE id=?",
                  (email.strip().lower() or None, user_id))
    return RedirectResponse("/admin/users", status_code=303)


@router.post("/users/{user_id}/password")
def users_set_password(user_id: int, ident: Identity = AdminDep,
                       password: str = Form("")):
    """Self-host only: set (or clear, with an empty field) a user's local
    password so the family signs in without email codes."""
    if not LOCAL_AUTH:
        raise HTTPException(404, "local passwords are not enabled")
    if password and len(password) < 8:
        raise HTTPException(400, "password needs at least 8 characters")
    with db.conn() as c:
        c.execute("UPDATE users SET password_hash=? WHERE id=?",
                  (hash_password(password) if password else "", user_id))
    return RedirectResponse("/admin/users", status_code=303)


@router.post("/users/{user_id}/send-install")
def users_send_install(user_id: int, ident: Identity = AdminDep):
    """The install-instructions email, on demand. Best-effort like all
    mail; the ?sent flag just feeds a little confirmation note."""
    ok = emails.send_install(user_id)
    return RedirectResponse("/admin/users?sent=" + ("1" if ok else "0"),
                            status_code=303)


# ---------- permissions matrix (symmetric: one checkbox per pair) ----------
@router.get("/perms", response_class=HTMLResponse)
def perms_page(request: Request, ident: Identity = AdminDep):
    with db.conn() as c:
        users = [dict(u) for u in db.all_(
            c, "SELECT id,display_name,color FROM users ORDER BY id")]
        pairs = sorted({(min(p["sender"], p["recipient"]),
                         max(p["sender"], p["recipient"])) for p in
                        db.all_(c, "SELECT sender,recipient FROM perms")})
    return _page(request, "perms.html", users=users,
                 pairs=[list(p) for p in pairs], me=ident)


@router.post("/perms")
async def perms_save(request: Request, ident: Identity = AdminDep):
    form = await request.form()
    with db.conn() as c:
        c.execute("DELETE FROM perms")
        for key in form.keys():
            if key.startswith("p_"):
                s, r = key[2:].split("_")
                if s != r:               # a checked pair talks both ways
                    c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)",
                              (int(s), int(r)))
                    c.execute("INSERT OR IGNORE INTO perms VALUES (?,?)",
                              (int(r), int(s)))
    # boxes refetch contacts right away; NOT retained (a retained message
    # here would replace the retained firmware notify on the same topic)
    mqtt.notify_all('{"event":"contacts"}', retain=False)
    voice.ensure_all()   # voice boxes may need clips for new contacts
    return RedirectResponse("/admin/perms", status_code=303)


# ---------- devices ----------
@router.get("/devices", response_class=HTMLResponse)
def devices_page(request: Request, ident: Identity = AdminDep):
    with db.conn() as c:
        devices = db.all_(c, """SELECT d.*, u.display_name FROM devices d
                                JOIN users u ON u.id=d.user_id ORDER BY d.id""")
        users = db.all_(c, "SELECT id,display_name FROM users ORDER BY id")
    return _page(request, "devices.html", devices=devices, users=users,
                 me=ident)


@router.post("/devices/{device_id}/admin")
def devices_set_admin(device_id: str, ident: Identity = AdminDep,
                      admin_id: str = Form("")):
    """Assign (or clear) the device admin - the user who manages this
    device's contact list from the PWA."""
    with db.conn() as c:
        c.execute("UPDATE devices SET admin_id=? WHERE id=?",
                  (int(admin_id) if admin_id else None, device_id))
    return RedirectResponse("/admin/devices", status_code=303)


@router.post("/devices/{device_id}/voice")
def devices_voice(device_id: str, ident: Identity = AdminDep,
                  on: str = Form("")):
    """Accessibility: hands-free voice control ("Hey Pip") on this box.
    The box learns within seconds (non-retained notify -> config fetch);
    spoken prompts render in the background on first enable."""
    with db.conn() as c:
        dev = db.one(c, "SELECT user_id FROM devices WHERE id=?", (device_id,))
        if not dev:
            raise HTTPException(404, "no such device")
        c.execute("UPDATE devices SET voice=? WHERE id=?",
                  (1 if on else 0, device_id))
    if on:
        voice.ensure_user(dev["user_id"])
    mqtt.notify_user(dev["user_id"], '{"event":"voice"}')
    return RedirectResponse("/admin/devices", status_code=303)


@router.post("/devices")
def devices_create(request: Request, ident: Identity = AdminDep,
                   device_id: str = Form(...), user_id: int = Form(...),
                   pin: str = Form("1234")):
    device_id = device_id.strip().lower()
    if not re.fullmatch(r"[a-z0-9-]{1,31}", device_id):
        raise HTTPException(400, "device id must be [a-z0-9-], max 31 chars")
    with db.conn() as c:
        u = db.one(c, "SELECT * FROM users WHERE id=?", (user_id,))
        if not u:
            raise HTTPException(404, "no such user")
        token_raw, mqtt_pass = provision.insert_device(c, device_id, user_id)
    mqtt.provision_device(device_id, mqtt_pass)

    nvs_csv = provision.build_nvs_csv(device_id, u["display_name"],
                                      token_raw, mqtt_pass, pin)
    return _page(request, "device_new.html", device_id=device_id,
                 token=token_raw, mqtt_pass=mqtt_pass, nvs_csv=nvs_csv,
                 me=ident)


@router.post("/devices/{device_id}/delete")
def devices_delete(device_id: str, ident: Identity = AdminDep):
    with db.conn() as c:
        c.execute("DELETE FROM devices WHERE id=?", (device_id,))
    mqtt.revoke_device(device_id)
    return RedirectResponse("/admin/devices", status_code=303)


# ---------- firmware / OTA ----------
@router.get("/firmware", response_class=HTMLResponse)
def firmware_page(request: Request, ident: Identity = AdminDep,
                  msg: str = None):
    with db.conn() as c:
        rows = db.all_(c, "SELECT * FROM firmware ORDER BY created DESC")
    bundles = {f"{r['version']}|{r['board']}": all(
        os.path.exists(provision.asset_path(r["version"], k, r["board"]))
        for k in provision.ASSET_KINDS) for r in rows}
    return _page(request, "firmware.html", firmwares=rows, bundles=bundles,
                 msg=msg, release_url=release.MANIFEST_URL, me=ident)


@router.post("/firmware/fetch")
def firmware_fetch(ident: Identity = AdminDep):
    """Pull the latest published release (hash-verified) as an inactive
    row; activation stays the explicit fleet-rollout action."""
    try:
        msg = release.install()
    except Exception as e:      # network/manifest problems land in the UI
        msg = f"fetch failed: {e}"
    return RedirectResponse(f"/admin/firmware?msg={quote(msg)}",
                            status_code=303)


@router.post("/firmware")
async def firmware_upload(ident: Identity = AdminDep,
                          version: str = Form(...), notes: str = Form(""),
                          activate: str = Form(None), file: UploadFile = None,
                          bootloader: UploadFile = None,
                          parttable: UploadFile = None,
                          board: str = Form(boards.DEFAULT_BOARD)):
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", version) or ".." in version:
        raise HTTPException(400, "version must be [A-Za-z0-9._-], max 32 chars")
    if not boards.valid(board):
        raise HTTPException(400, "unknown board model")
    data = await file.read()
    with open(db.firmware_path(version, board), "wb") as f:
        f.write(data)
    # optional web-flash bundle parts (build/bootloader/bootloader.bin,
    # build/partition_table/partition-table.bin)
    for kind, up in (("bootloader", bootloader), ("parttable", parttable)):
        if up and up.filename:
            blob = await up.read()
            with open(provision.asset_path(version, kind, board), "wb") as f:
                f.write(blob)
    with db.conn() as c:
        if activate:
            # active is per board: each model has its own rollout
            c.execute("UPDATE firmware SET active=0 WHERE board=?", (board,))
        c.execute("""INSERT OR REPLACE INTO firmware (version,notes,active,
                     board) VALUES (?,?,?,?)""",
                  (version, notes, 1 if activate else 0, board))
    if activate:
        api.version_bumped()
        mqtt.notify_all('{"event":"firmware","version":"%s"}' % version)
    return RedirectResponse("/admin/firmware", status_code=303)


@router.post("/firmware/{board}/{version}/activate")
def firmware_activate(board: str, version: str, ident: Identity = AdminDep):
    with db.conn() as c:
        fw = db.one(c, """SELECT board FROM firmware
                          WHERE version=? AND board=?""", (version, board))
        if not fw:
            raise HTTPException(404, "no such firmware")
        # active is per board: activating a build only rolls out its model
        c.execute("UPDATE firmware SET active=0 WHERE board=?", (board,))
        c.execute("""UPDATE firmware SET active=1
                     WHERE version=? AND board=?""", (version, board))
    api.version_bumped()
    mqtt.notify_all('{"event":"firmware","version":"%s"}' % version)
    return RedirectResponse("/admin/firmware", status_code=303)
