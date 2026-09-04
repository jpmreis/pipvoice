"""admin: server-rendered management UI at /admin.
Users, devices (provision + NVS CSV), n-to-n permissions matrix, OTA."""
import os
import re
import secrets
from datetime import datetime, timedelta

from fastapi import APIRouter, Form, HTTPException, Query, Request, UploadFile
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.templating import Jinja2Templates

from urllib.parse import quote

from . import api, boards, db, emails, mqtt, provision, release, stats, voice
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
        stats.event("login.blocked", dim="admin")
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
            stats.event("login.fail", dim="admin",
                        user_id=u["id"] if u else None)
            return _page(request, "login.html", bootstrap=False,
                         stage="email", email=email,
                         error="Wrong email or password")
        login_succeeded(rl_key)
        stats.event("login.ok", dim="admin", user_id=u["id"],
                    detail="password")
        return _signed_in(u["id"])

    if action == "request":
        login_failed(rl_key)         # every code email counts toward the cap
        u = user_by_email(email)
        stats.event("login.code", dim="admin",
                    user_id=u["id"] if u and u["is_admin"] else None)
        if u and u["is_admin"]:
            emails.send_login_code(u["id"], u["display_name"],
                                   issue_login_code(u["id"]))
        # identical response whether or not the address matched an admin
        return _page(request, "login.html", bootstrap=False, stage="code",
                     email=email, error=None)

    u = user_by_email(email)         # action == "verify"
    if not u or not u["is_admin"] or not redeem_login_code(u["id"], code):
        login_failed(rl_key)
        stats.event("login.fail", dim="admin", user_id=u["id"] if u else None)
        return _page(request, "login.html", bootstrap=False, stage="code",
                     email=email, error="Wrong or expired code")
    login_succeeded(rl_key)
    stats.event("login.ok", dim="admin", user_id=u["id"], detail="code")
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


# ---------- analytics ----------
RANGES = {"7d": 7, "30d": 30, "90d": 90, "all": None}
# event-log filter buttons: label -> kind prefix
KIND_FILTERS = (("all", ""), ("messages", "msg."), ("reactions", "reaction"),
                ("logins", "login."), ("push", "push."), ("email", "email."),
                ("devices", "device."), ("ota", "ota."),
                ("http", "http."), ("server", "server."))
LOG_PAGE = 200


def _by_day(rows) -> dict:
    """series rows -> {day: {dim: [n, total, lo, hi]}} (hourly buckets fold
    into their day, so every range charts one bar per day)."""
    out: dict = {}
    for r in rows:
        day = r["b"][:10]
        cur = out.setdefault(day, {}).setdefault(r["dim"], [0, 0.0, None, None])
        cur[0] += r["n"]
        cur[1] += r["total"] or 0
        if r["lo"] is not None:
            cur[2] = r["lo"] if cur[2] is None else min(cur[2], r["lo"])
        if r["hi"] is not None:
            cur[3] = r["hi"] if cur[3] is None else max(cur[3], r["hi"])
    return out


def _chart(days: list, by_day: dict, dims: list, mode: str = "n") -> dict:
    """Chart payload for the template's JS: one series per dim.
    mode n = counts, avg = total/n, hi = max."""
    series = []
    for d in dims:
        vals = []
        for day in days:
            cur = by_day.get(day, {}).get(d)
            if not cur or not cur[0]:
                vals.append(0)
            elif mode == "avg":
                vals.append(round(cur[1] / cur[0], 1))
            elif mode == "hi":
                vals.append(round(cur[3] or 0, 1))
            else:
                vals.append(cur[0])
        series.append({"name": d or "total", "values": vals})
    return {"labels": [d[5:] for d in days], "series": series}


def _n(tot: dict, metric: str, dim=None) -> int:
    """Sum of n over a metric (or one dim) from stats.totals()."""
    dims = tot.get(metric, {})
    if dim is not None:
        return int(dims.get(dim, {}).get("n", 0) or 0)
    return int(sum((v["n"] or 0) for v in dims.values()))


@router.get("/analytics", response_class=HTMLResponse)
def analytics_page(request: Request, ident: Identity = AdminDep,
                   rng: str = Query("7d", alias="range"), kind: str = "",
                   user: str = "", device: str = "", before: str = ""):
    """User trends, server health, device health (stats.py). The 7-day
    view reads the hourly counters; longer ranges read the daily rollup.
    Per-user and per-device detail is always the last 7 days - that is
    the only place it exists."""
    if rng not in RANGES:
        rng = "7d"
    ndays = RANGES[rng]
    now = stats.utcnow()
    table = "hourly" if rng == "7d" else "daily"
    if ndays:
        start = now - timedelta(days=ndays - 1)
        since = start.strftime("%Y-%m-%d %H" if table == "hourly"
                               else "%Y-%m-%d")
        days = [(start + timedelta(days=i)).strftime("%Y-%m-%d")
                for i in range(ndays)]
    else:
        since = "0000"
        days = None
    week = (now - timedelta(days=6)).strftime("%Y-%m-%d %H")
    with db.conn() as c:
        tot = stats.totals(c, table, since)
        wk = tot if table == "hourly" else stats.totals(c, "hourly", week)
        if days is None:                     # "all": every day on record
            days = [r["d"] for r in db.all_(
                c, "SELECT DISTINCT day AS d FROM daily ORDER BY day")] or \
                [now.strftime("%Y-%m-%d")]
        sent = _by_day(stats.series(c, table, "msg.sent", since))
        lat = _by_day(stats.series(c, table, "msg.delivered", since))
        req = _by_day(stats.series(c, table, "req", since))
        errs = _by_day(stats.series(c, table, "req.status", since))
        storage = _by_day(stats.series(c, "daily", "db.bytes",
                                       since if table == "daily" else
                                       since[:10]))
        audio = _by_day(stats.series(c, "daily", "audio.bytes",
                                     since if table == "daily" else
                                     since[:10]))
        # active users: distinct ids this week, or the daily count's
        # average when the range no longer has per-user data
        if table == "hourly":
            active_users = len(tot.get("user.active", {}))
            active_devices = len(tot.get("diag.rssi", {}))
        else:
            au = [r["n"] for r in stats.series(c, "daily", "active.users",
                                               since)]
            ad = [r["n"] for r in stats.series(c, "daily", "active.devices",
                                               since)]
            active_users = round(sum(au) / len(au), 1) if au else 0
            active_devices = round(sum(ad) / len(ad), 1) if ad else 0
        groups = sorted(tot.get("req", {}).items(),
                        key=lambda kv: -(kv[1]["n"] or 0))
        latency_rows = [{"group": g, "n": v["n"],
                         "avg": round((v["total"] or 0) / v["n"]) if v["n"] else 0,
                         "hi": round(v["hi"] or 0)} for g, v in groups]
        # per-user table: last 7 days of hourly, capped
        names = {r["id"]: r for r in db.all_(
            c, "SELECT id, username, display_name, color FROM users")}
        per_user = []
        for uid_s, v in wk.get("user.active", {}).items():
            uid = int(uid_s)
            u = names.get(uid)
            if not u:
                continue
            per_user.append({
                "name": u["display_name"], "color": u["color"],
                "active_h": v["n"],
                "sent": int(wk.get("user.sent", {}).get(uid_s, {}).get("n", 0) or 0),
                "recv": int(wk.get("user.recv", {}).get(uid_s, {}).get("n", 0) or 0),
                "id": uid})
        per_user.sort(key=lambda r: (-r["sent"], -r["active_h"]))
        per_user = per_user[:50]
        # devices: current row + 7-day diag aggregates + 7-day event counts
        active_fw = {r["board"]: r["version"] for r in db.all_(
            c, "SELECT board, version FROM firmware WHERE active=1")}
        dev_events = {}
        for r in db.all_(c, """SELECT device_id, kind, COUNT(*) n FROM events
                               WHERE device_id IS NOT NULL
                               GROUP BY device_id, kind"""):
            dev_events.setdefault(r["device_id"], {})[r["kind"]] = r["n"]
        devices = []
        for d in db.all_(c, """SELECT d.*, u.display_name FROM devices d
                               JOIN users u ON u.id=d.user_id ORDER BY d.id"""):
            rs = wk.get("diag.rssi", {}).get(d["id"])
            hp = wk.get("diag.heap", {}).get(d["id"])
            ev = dev_events.get(d["id"], {})
            uptime = None
            if d["boot_at"]:
                try:
                    uptime = (now.replace(tzinfo=None)
                              - datetime.fromisoformat(d["boot_at"])
                              ).total_seconds() / 3600
                except ValueError:
                    pass
            devices.append({
                "id": d["id"], "owner": d["display_name"],
                "board": boards.BOARDS.get(d["board"], {}).get("label",
                                                               d["board"]),
                "online": bool(d["online"]), "last_seen": d["last_seen"],
                "fw": d["fw_version"],
                "fw_stale": bool(d["fw_version"] and active_fw.get(d["board"])
                                 and d["fw_version"] != active_fw[d["board"]]),
                "rssi": d["rssi"],
                "rssi_avg": round(rs["total"] / rs["n"]) if rs and rs["n"] else None,
                "rssi_lo": rs["lo"] if rs else None,
                "heap": d["heap"], "heap_max": d["heap_max"],
                "heap_lo": hp["lo"] if hp else None,
                "uptime": _age(uptime) if uptime is not None else None,
                "reset": d["reset"],
                "boots": ev.get("device.boot", 0),
                "offline": ev.get("device.offline", 0),
                "ota": ev.get("ota.download", 0),
            })
        # per-device drill-down sparklines (7 days, hourly)
        spark = None
        if device:
            spark = {
                "rssi": [[r["b"][5:], round(r["total"] / r["n"], 1)]
                         for r in stats.series(c, "hourly", "diag.rssi", week,
                                               device) if r["n"]],
                "heap": [[r["b"][5:], round(r["total"] / r["n"] / 1024, 1)]
                         for r in stats.series(c, "hourly", "diag.heap", week,
                                               device) if r["n"]],
            }
        # event log
        where, args = [], []
        if kind:
            where.append("e.kind LIKE ?")
            args.append(kind + "%")
        if user:
            where.append("e.user_id=?")
            args.append(int(user) if user.isdigit() else -1)
        if device:
            where.append("e.device_id=?")
            args.append(device)
        if before.isdigit():
            where.append("e.id < ?")
            args.append(int(before))
        sql = """SELECT e.*, u.display_name FROM events e
                 LEFT JOIN users u ON u.id=e.user_id"""
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += f" ORDER BY e.id DESC LIMIT {LOG_PAGE + 1}"
        rows = db.all_(c, sql, args)
        more = len(rows) > LOG_PAGE
        log_rows = [dict(r) for r in rows[:LOG_PAGE]]
        for r in log_rows:                    # 7.0 -> 7 in the log column
            v = r["value"]
            if isinstance(v, float) and v.is_integer():
                r["value"] = int(v)
        undelivered = db.one(c, "SELECT COUNT(*) n FROM messages "
                                "WHERE delivered=0")["n"]

    delivered = tot.get("msg.delivered", {})
    lat_summary = []
    for via in ("box", "phone"):
        v = delivered.get(via)
        if v and v["n"]:
            lat_summary.append({"via": via, "n": v["n"],
                                "avg": round(v["total"] / v["n"]),
                                "hi": round(v["hi"] or 0)})
    tiles = {
        "sent": _n(tot, "msg.sent"), "delivered": _n(tot, "msg.delivered"),
        "reactions": _n(tot, "reaction"), "logins": _n(tot, "login.ok"),
        "active_users": active_users, "active_devices": active_devices,
        "requests": _n(tot, "req"), "errors_5xx": _n(tot, "req.status", "5xx"),
        "errors_4xx": _n(tot, "req.status", "4xx"),
        "restarts": _n(tot, "server.start"),
        "ratelimit": _n(tot, "http.ratelimit"),
        "push_ok": _n(tot, "push.ok"), "push_fail": _n(tot, "push.fail"),
        "push_pruned": _n(tot, "push.pruned"),
        "email_ok": _n(tot, "email.ok"), "email_fail": _n(tot, "email.fail"),
        "reminders": _n(tot, "reminder"), "undelivered": undelivered,
        "expired": _n(tot, "msg.expired"),
    }
    req_dims = [g for g, _ in groups][:8]
    charts = {
        "sent": _chart(days, sent, ["box", "phone"]),
        "latency": _chart(days, lat, ["box", "phone"], mode="avg"),
        "req": _chart(days, req, req_dims),
        "errors": _chart(days, errs, ["4xx", "5xx"]),
        "storage": {"labels": [d[5:] for d in days],
                    "series": [
                        {"name": "db MB", "values": [
                            round((storage.get(d, {}).get("", [0])[0] or 0)
                                  / 1048576, 2) for d in days]},
                        {"name": "audio MB", "values": [
                            round((audio.get(d, {}).get("", [0])[0] or 0)
                                  / 1048576, 2) for d in days]}]},
    }
    return _page(request, "analytics.html", me=ident, range=rng,
                 ranges=list(RANGES), tiles=tiles, charts=charts,
                 latency=latency_rows, lat_summary=lat_summary,
                 per_user=per_user, devices=devices, spark=spark,
                 log=log_rows, more=more, kind=kind, user=user,
                 device=device, kind_filters=KIND_FILTERS,
                 last_id=log_rows[-1]["id"] if log_rows else 0,
                 stats_days=stats.DAYS, offline_min=stats.OFFLINE_MIN,
                 now=now.strftime("%Y-%m-%d %H:%M"))


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
    # One table row per version; one status/activate column per model.
    board_cols = [(k, b["label"]) for k, b in boards.BOARDS.items()]
    board_cols += [(k, k) for k in                # rows from retired models
                   sorted({r["board"] for r in rows} - set(boards.BOARDS))]
    releases, by_ver = [], {}
    for r in rows:
        rel = by_ver.get(r["version"])
        if rel is None:
            rel = by_ver[r["version"]] = {"version": r["version"],
                                          "notes": r["notes"],
                                          "created": r["created"],
                                          "boards": {}}
            releases.append(rel)
        if r["notes"] and not rel["notes"]:
            rel["notes"] = r["notes"]
        rel["boards"][r["board"]] = {"active": r["active"], "bundle": all(
            os.path.exists(provision.asset_path(r["version"], k, r["board"]))
            for k in provision.ASSET_KINDS)}
    return _page(request, "firmware.html", releases=releases,
                 board_cols=board_cols,
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
        stats.event("firmware.activate", dim=board, user_id=ident.user_id,
                    detail=version)
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
    stats.event("firmware.activate", dim=board, user_id=ident.user_id,
                detail=version)
    mqtt.notify_all('{"event":"firmware","version":"%s"}' % version)
    return RedirectResponse("/admin/firmware", status_code=303)
