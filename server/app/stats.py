"""stats: admin analytics - a 7-day detail log and forever-daily rollups.

Two tiers, chosen so the cost stays flat as the family grows:

- ``event(kind, ...)`` - something happened (a message was sent, a box
  rebooted, a push failed). One row in ``events``; kept STATS_DAYS, then
  purged. For things that happen less than about once a minute per user.
- ``count(metric, dim, value)`` - hot-path metrics (every request, every
  diag header). In memory, flushed to ``hourly`` once a minute, kept
  STATS_DAYS. Losing a minute of counters on a crash is accepted.

``rollup_once`` folds ``hourly`` into ``daily`` (one row per day x metric
x dim, kept forever). Per-user and per-device dims (PER_ENTITY) never
reach ``daily`` - they collapse to "active users/devices" counts - so the
forever table is bounded by the number of metrics, not the number of
users. Every event also counts, so a kind's daily totals come from the
same code path as its log rows.

RULES
- Never log a secret or personal datum: no login codes, tokens, emails,
  IPs, subjects, raw request paths (setup URLs carry one-shot nonces).
- ``event()`` opens its own connection unless given ``c=``; never call it
  with c=None from inside an open ``db.conn()`` block - the inner
  connection would wait on the outer write lock and the event is lost.
- The loop's work runs in a thread (asyncio.to_thread): a lock wait on
  the event loop would freeze every request.
"""
import asyncio
import logging
import os
import threading
import time
from datetime import datetime, timedelta, timezone

from . import db

log = logging.getLogger("stats")

DAYS = int(db.env("STATS_DAYS", "7"))              # events + hourly retention
# a box polls every 15 min even with MQTT up (sync.c); two misses + slack
OFFLINE_MIN = int(db.env("OFFLINE_MIN", "40"))
FLUSH_S = 60

# hourly metrics whose dim is a user or device id: summarized into a
# distinct count in daily, never copied row by row
PER_ENTITY = {"user.active": "active.users", "user.sent": None,
              "user.recv": None, "diag.rssi": "active.devices",
              "diag.heap": None}

# esp_reset_reason_t (IDF 5.4)
RESET_REASONS = {0: "unknown", 1: "poweron", 2: "ext", 3: "sw", 4: "panic",
                 5: "int_wdt", 6: "task_wdt", 7: "wdt", 8: "deepsleep",
                 9: "brownout", 10: "sdio", 11: "usb", 12: "jtag",
                 13: "efuse", 14: "pwr_glitch", 15: "cpu_lockup"}

_counters: dict = {}          # (hour, metric, dim) -> [n, total, lo, hi]
_lock = threading.Lock()


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _hour(now: datetime | None = None) -> str:
    return (now or utcnow()).strftime("%Y-%m-%d %H")


def _iso(dt: datetime) -> str:
    """The same shape SQLite's datetime('now') writes."""
    return dt.strftime("%Y-%m-%d %H:%M:%S")


# ---------- write side ----------
def count(metric: str, dim: str = "", n: int = 1,
          value: float | None = None) -> None:
    key = (_hour(), metric, str(dim))
    with _lock:
        cur = _counters.get(key)
        if cur is None:
            cur = _counters[key] = [0, 0.0, None, None]
        cur[0] += n
        if value is not None:
            cur[1] += value
            cur[2] = value if cur[2] is None else min(cur[2], value)
            cur[3] = value if cur[3] is None else max(cur[3], value)


def event(kind: str, *, dim: str = "", user_id=None, device_id=None,
          msg_id=None, value=None, detail=None, c=None) -> None:
    count(kind, dim, value=value)
    row = (kind, str(dim), user_id, device_id, msg_id, value, detail)
    sql = """INSERT INTO events (kind,dim,user_id,device_id,msg_id,value,
             detail) VALUES (?,?,?,?,?,?,?)"""
    try:
        if c is not None:
            c.execute(sql, row)
        else:
            with db.conn() as cc:
                cc.execute(sql, row)
    except Exception as e:                      # analytics never break a request
        log.warning("event %s dropped: %s", kind, e)


def flush() -> int:
    """Buffered counters -> hourly. Returns rows upserted."""
    with _lock:
        batch = dict(_counters)
        _counters.clear()
    if not batch:
        return 0
    try:
        with db.conn() as c:
            for (hour, metric, dim), (n, total, lo, hi) in batch.items():
                c.execute("""INSERT INTO hourly (hour,metric,dim,n,total,lo,hi)
                             VALUES (?,?,?,?,?,?,?)
                             ON CONFLICT(hour,metric,dim) DO UPDATE SET
                               n=n+excluded.n, total=total+excluded.total,
                               lo=CASE WHEN lo IS NULL THEN excluded.lo
                                       WHEN excluded.lo IS NULL THEN lo
                                       ELSE min(lo, excluded.lo) END,
                               hi=CASE WHEN hi IS NULL THEN excluded.hi
                                       WHEN excluded.hi IS NULL THEN hi
                                       ELSE max(hi, excluded.hi) END""",
                          (hour, metric, dim, n, total, lo, hi))
    except Exception as e:
        # put the batch back so a transient lock costs nothing
        with _lock:
            for k, v in batch.items():
                cur = _counters.get(k)
                if cur is None:
                    _counters[k] = v
                else:
                    cur[0] += v[0]
                    cur[1] += v[1]
                    if v[2] is not None:
                        cur[2] = v[2] if cur[2] is None else min(cur[2], v[2])
                    if v[3] is not None:
                        cur[3] = v[3] if cur[3] is None else max(cur[3], v[3])
        log.warning("flush failed, retrying next tick: %s", e)
        return 0
    return len(batch)


# ---------- device diagnostics ----------
def parse_diag(header: str) -> dict:
    """'v=1.3.8;b=amoled-1.8;r=-61;h=91234;m=31700;u=3600;rr=1' -> dict.
    Every key optional; anything malformed is ignored, never fatal."""
    out = {}
    for part in (header or "").split(";"):
        k, _, v = part.strip().partition("=")
        k, v = k.strip(), v.strip()
        if not k or not v:
            continue
        if k in ("v", "b"):
            if len(v) <= 32:
                out[k] = v
        elif k in ("r", "h", "m", "u", "rr"):
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def device_diag(c, device_id: str, board: str, header: str,
                prev) -> None:
    """Fold a box's X-Pip-Diag header into its devices row (the caller's
    connection - this runs inside the auth path's write) and log what
    changed. ``prev`` is the device row before this request."""
    d = parse_diag(header)
    if not d:
        return
    now = utcnow()
    sets, args = [], []
    if "v" in d:
        sets.append("fw_version=?")
        args.append(d["v"])
        if d["v"] != prev["fw_version"]:
            event("device.fw", dim=board, device_id=device_id,
                  detail=f"{prev['fw_version'] or '?'} -> {d['v']}", c=c)
    if "r" in d and d["r"] != 0:                 # 0 = "don't know" (net_wifi.c)
        sets.append("rssi=?")
        args.append(d["r"])
        count("diag.rssi", device_id, value=d["r"])
    if "h" in d:
        sets.append("heap=?")
        args.append(d["h"])
        count("diag.heap", device_id, value=d["h"])
    if "m" in d:
        sets.append("heap_max=?")
        args.append(d["m"])
    if "rr" in d:
        sets.append("reset=?")
        args.append(RESET_REASONS.get(d["rr"], str(d["rr"])))
    if "u" in d:
        boot_at = now - timedelta(seconds=max(d["u"], 0))
        sets.append("boot_at=?")
        args.append(_iso(boot_at))
        prev_boot = prev["boot_at"]
        if prev_boot:
            try:
                old = datetime.fromisoformat(prev_boot).replace(
                    tzinfo=timezone.utc)
                # a reboot moves the boot instant forward; small drift
                # between uptime and wall clock is not a reboot
                if (boot_at - old).total_seconds() > 120:
                    event("device.boot", dim=board, device_id=device_id,
                          detail=RESET_REASONS.get(d.get("rr", -1),
                                                   prev["reset"] or "?"),
                          c=c)
            except ValueError:
                pass
    if sets:
        c.execute(f"UPDATE devices SET {', '.join(sets)} WHERE id=?",
                  args + [device_id])


def presence_sweep() -> int:
    """Online/offline transitions from last_seen gaps. Returns changes."""
    changes = []
    with db.conn() as c:
        rows = db.all_(c, """SELECT id, board, online, last_seen,
                                    (julianday('now') - julianday(last_seen))
                                      * 1440 AS quiet_min
                             FROM devices""")
        for r in rows:
            seen = r["last_seen"] is not None
            up = seen and r["quiet_min"] < OFFLINE_MIN
            if up != bool(r["online"]):
                c.execute("UPDATE devices SET online=? WHERE id=?",
                          (1 if up else 0, r["id"]))
                changes.append((r["id"], r["board"], up,
                                r["quiet_min"] if seen else None))
    for dev, board, up, quiet in changes:
        event("device.online" if up else "device.offline", dim=board,
              device_id=dev, value=round(quiet, 1) if quiet else None)
    return len(changes)


# ---------- rollup + retention ----------
def rollup_once(now: datetime | None = None) -> dict:
    """hourly -> daily for every day still in hourly (idempotent, so a
    server that was down across a day boundary catches up), today's
    snapshots, then purge detail older than DAYS."""
    now = now or utcnow()
    today = now.strftime("%Y-%m-%d")
    stored, stored_bytes = db.audio_usage()
    db_bytes = 0
    for suffix in ("", "-wal"):
        try:
            db_bytes += os.path.getsize(db.DB_PATH + suffix)
        except OSError:
            pass
    rolled = 0
    with db.conn() as c:
        days = [r["d"] for r in db.all_(
            c, "SELECT DISTINCT substr(hour,1,10) AS d FROM hourly")]
        for day in days:
            rows = db.all_(c, """SELECT metric, dim, SUM(n) n, SUM(total) total,
                                        MIN(lo) lo, MAX(hi) hi
                                 FROM hourly WHERE substr(hour,1,10)=?
                                 GROUP BY metric, dim""", (day,))
            distinct: dict = {}
            for r in rows:
                if r["metric"] in PER_ENTITY:
                    agg = PER_ENTITY[r["metric"]]
                    if agg:
                        distinct[agg] = distinct.get(agg, 0) + 1
                    continue
                c.execute("""INSERT OR REPLACE INTO daily
                             (day,metric,dim,n,total,lo,hi)
                             VALUES (?,?,?,?,?,?,?)""",
                          (day, r["metric"], r["dim"], r["n"], r["total"],
                           r["lo"], r["hi"]))
                rolled += 1
            for metric, n in distinct.items():
                c.execute("""INSERT OR REPLACE INTO daily (day,metric,n)
                             VALUES (?,?,?)""", (day, metric, n))
                rolled += 1
        # point-in-time snapshots, today only (yesterday's stays final)
        snaps = {
            "users": db.one(c, "SELECT COUNT(*) n FROM users")["n"],
            "devices": db.one(c, "SELECT COUNT(*) n FROM devices")["n"],
            "push_subs": db.one(c, "SELECT COUNT(*) n FROM push_subs")["n"],
            "undelivered": db.one(c, "SELECT COUNT(*) n FROM messages "
                                     "WHERE delivered=0")["n"],
            "audio.files": stored, "audio.bytes": stored_bytes,
            "db.bytes": db_bytes,
        }
        for metric, n in snaps.items():
            c.execute("""INSERT OR REPLACE INTO daily (day,metric,n)
                         VALUES (?,?,?)""", (today, metric, n))
        cutoff = _iso(now - timedelta(days=DAYS))
        purged = c.execute("DELETE FROM events WHERE ts < ?",
                           (cutoff,)).rowcount
        purged += c.execute("DELETE FROM hourly WHERE hour < ?",
                            (cutoff[:13],)).rowcount
    return {"rolled": rolled, "purged": purged}


async def stats_loop():
    last_hour = _hour()
    while True:
        await asyncio.sleep(FLUSH_S)
        try:
            await asyncio.to_thread(flush)
            await asyncio.to_thread(presence_sweep)
            if _hour() != last_hour:
                last_hour = _hour()
                r = await asyncio.to_thread(rollup_once)
                if r["purged"]:
                    log.info("rolled up %d rows, purged %d", r["rolled"],
                             r["purged"])
        except Exception as e:
            log.warning("stats tick failed: %s", e)


# ---------- read side (admin page) ----------
ROUTE_GROUPS = (
    ("/v1/inbox", "inbox"), ("/v1/messages/", "messages"),
    ("/v1/messages", "messages"), ("/v1/auth/", "auth"),
    ("/v1/contacts", "contacts"), ("/v1/themes", "themes"),
    ("/v1/theme", "themes"), ("/v1/firmware", "firmware"),
    ("/v1/push/", "push"), ("/v1/version", "version"),
    ("/v1/webver", "version"), ("/v1/setup", "setup"),
    ("/v1/managed", "managed"), ("/v1/reactions", "reactions"),
    ("/v1/presence", "presence"), ("/v1/voice/", "voice"),
    ("/v1/device", "device"), ("/v1/me", "me"), ("/v1/", "other"),
    ("/admin", "admin"), ("/app", "app"),
)


def route_group(path: str) -> str:
    if path.startswith("/v1/messages/") and "/audio" in path:
        return "audio"
    for prefix, group in ROUTE_GROUPS:
        if path.startswith(prefix):
            return group
    return "site"


def series(c, table: str, metric: str, since: str, dim=None) -> list:
    """[(bucket, dim, n, total, lo, hi)] for one metric, oldest first."""
    col = "hour" if table == "hourly" else "day"
    sql = f"""SELECT {col} AS b, dim, n, total, lo, hi FROM {table}
              WHERE metric=? AND {col} >= ?"""
    args = [metric, since]
    if dim is not None:
        sql += " AND dim=?"
        args.append(dim)
    return db.all_(c, sql + f" ORDER BY {col}", args)


def totals(c, table: str, since: str) -> dict:
    """{metric: {dim: {n, total, lo, hi}}} over the whole range."""
    col = "hour" if table == "hourly" else "day"
    out: dict = {}
    for r in db.all_(c, f"""SELECT metric, dim, SUM(n) n, SUM(total) total,
                                   MIN(lo) lo, MAX(hi) hi
                            FROM {table} WHERE {col} >= ?
                            GROUP BY metric, dim""", (since,)):
        out.setdefault(r["metric"], {})[r["dim"]] = dict(r)
    return out
