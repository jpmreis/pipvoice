"""db: SQLite schema + tiny helpers. Single-writer family scale."""
import os
import re
import sqlite3
from contextlib import contextmanager


def env(name: str, default=None):
    """All config comes from PIP_* environment variables (see env.example)."""
    return os.environ.get("PIP_" + name, default)


# The hosted instance's domain. THE single hosted-vs-self-hosted check:
# a deployment is "hosted" iff PIP_BASE_URL points at this domain.
# Everything hosted-only (waitlist signup, landing/privacy page copy)
# branches on hosted() — never on a second flag. The firmware twin is
# config_is_hosted() in firmware/main/config.c.
HOSTED_DOMAIN = "pipvoice.com"


def hosted() -> bool:
    host = (env("BASE_URL", "") or "") \
        .split("//")[-1].split("/")[0].split(":")[0].lower()
    return host == HOSTED_DOMAIN or host.endswith("." + HOSTED_DOMAIN)


DATA_DIR = env("DATA", "/opt/pipvoice/data")
DB_PATH = os.path.join(DATA_DIR, "pip.db")
AUDIO_DIR = os.path.join(DATA_DIR, "audio")
FIRMWARE_DIR = os.path.join(DATA_DIR, "firmware")

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    username TEXT UNIQUE NOT NULL,
    display_name TEXT NOT NULL,
    color TEXT NOT NULL DEFAULT '4FC3F7',
    password_hash TEXT NOT NULL,
    is_admin INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,                -- e.g. pip-ella-01
    user_id INTEGER NOT NULL REFERENCES users(id),
    token_hash TEXT NOT NULL,
    mqtt_password TEXT NOT NULL,        -- always '' since 1.3.12 (never stored)
    created TEXT NOT NULL DEFAULT (datetime('now')),
    last_seen TEXT,
    voice INTEGER NOT NULL DEFAULT 0,   -- accessibility voice control
    board TEXT NOT NULL DEFAULT 'amoled-1.8'  -- hardware model (boards.py)
);
CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    expires TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS login_codes (   -- one active email code per user
    user_id INTEGER PRIMARY KEY REFERENCES users(id),
    code_hash TEXT NOT NULL,
    expires TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS perms (      -- sender may message recipient
    sender INTEGER NOT NULL REFERENCES users(id),
    recipient INTEGER NOT NULL REFERENCES users(id),
    PRIMARY KEY (sender, recipient)
);
CREATE TABLE IF NOT EXISTS messages (
    id TEXT PRIMARY KEY,
    sender INTEGER NOT NULL REFERENCES users(id),
    recipient INTEGER NOT NULL REFERENCES users(id),
    duration INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL DEFAULT (datetime('now')),
    delivered INTEGER NOT NULL DEFAULT 0,
    delivered_at TEXT
);
CREATE TABLE IF NOT EXISTS firmware (   -- one row per (version, board):
    version TEXT NOT NULL,               -- one PROJECT_VER tag ships one
    notes TEXT DEFAULT '',               -- build per hardware model
    active INTEGER NOT NULL DEFAULT 0,   -- active is per board
    created TEXT NOT NULL DEFAULT (datetime('now')),
    board TEXT NOT NULL DEFAULT 'amoled-1.8',  -- hardware model (boards.py)
    PRIMARY KEY (version, board)
);
CREATE TABLE IF NOT EXISTS reactions ( -- one per message, latest wins;
    msg_id TEXT PRIMARY KEY,           -- no FK to messages: a reaction must
    reactor INTEGER NOT NULL REFERENCES users(id),  -- survive message deletion
    target INTEGER NOT NULL REFERENCES users(id),   -- the original sender
    reaction TEXT NOT NULL,
    created TEXT NOT NULL DEFAULT (datetime('now')),
    seen INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS groups (    -- per-box broadcast lists, set up
    id TEXT PRIMARY KEY,               -- by the box's device admin.
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    name TEXT NOT NULL,                -- 'g-' + 8 hex: can never look like
    color TEXT NOT NULL DEFAULT 'FFB300',  -- a username ([a-z0-9_])
    created TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS group_members ( -- the box user is always a row;
    group_id TEXT NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    PRIMARY KEY (group_id, user_id)    -- others: admin or a box contact
);                                     -- (prune_group_members)
CREATE INDEX IF NOT EXISTS group_members_user ON group_members(user_id);
CREATE TABLE IF NOT EXISTS push_subs (  -- phone users' web push endpoints
    endpoint TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    p256dh TEXT NOT NULL,
    auth TEXT NOT NULL,
    created TEXT NOT NULL DEFAULT (datetime('now')),
    last_ok TEXT
);
CREATE TABLE IF NOT EXISTS waitlist (   -- public signups; read by hand
    email TEXT PRIMARY KEY,
    created TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS events (     -- analytics detail log (stats.py):
    id INTEGER PRIMARY KEY,             -- things that happened, kept
    ts TEXT NOT NULL DEFAULT (datetime('now')),  -- STATS_DAYS then purged
    kind TEXT NOT NULL,                 -- 'msg.sent', 'device.boot', ...
    dim TEXT NOT NULL DEFAULT '',       -- small enum facet (via, board, ...)
    user_id INTEGER,                    -- actor, when there is one
    device_id TEXT,
    msg_id TEXT,
    value REAL,                         -- kind-specific number (latency s)
    detail TEXT                         -- short human text; never a secret
);
CREATE INDEX IF NOT EXISTS events_ts ON events(ts);
CREATE TABLE IF NOT EXISTS hourly (     -- analytics counters, flushed from
    hour TEXT NOT NULL,                 -- memory every minute; STATS_DAYS
    metric TEXT NOT NULL,               -- 'YYYY-MM-DD HH' UTC
    dim TEXT NOT NULL DEFAULT '',
    n INTEGER NOT NULL DEFAULT 0,
    total REAL NOT NULL DEFAULT 0,      -- sum of values (avg = total/n)
    lo REAL, hi REAL,
    PRIMARY KEY (hour, metric, dim)
);
CREATE TABLE IF NOT EXISTS daily (      -- analytics rollup, kept forever:
    day TEXT NOT NULL,                  -- bounded rows/day (no per-user or
    metric TEXT NOT NULL,               -- per-device dims land here)
    dim TEXT NOT NULL DEFAULT '',
    n INTEGER NOT NULL DEFAULT 0,
    total REAL NOT NULL DEFAULT 0,
    lo REAL, hi REAL,
    PRIMARY KEY (day, metric, dim)
);
"""

# additive columns for pre-existing databases; "duplicate column" is fine
MIGRATIONS = [
    "ALTER TABLE users ADD COLUMN email TEXT",
    "ALTER TABLE messages ADD COLUMN reminded INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE users ADD COLUMN theme TEXT",   # background theme (PWA users)
    # the user who manages this device's contact list from the PWA
    "ALTER TABLE devices ADD COLUMN admin_id INTEGER REFERENCES users(id)",
    # when the first client acked; drives delete-on-delivery (cleanup.py)
    "ALTER TABLE messages ADD COLUMN delivered_at TEXT",
    # accessibility: hands-free voice control on this box (admin toggle)
    "ALTER TABLE devices ADD COLUMN voice INTEGER NOT NULL DEFAULT 0",
    # hardware model (boards.py key); every pre-existing box is a 1.8.
    # Recorded at provisioning and never changed by rekey: OTA and the
    # flash manifest only ever serve this model's artifacts.
    "ALTER TABLE devices ADD COLUMN board TEXT NOT NULL DEFAULT 'amoled-1.8'",
    "ALTER TABLE firmware ADD COLUMN board TEXT NOT NULL DEFAULT 'amoled-1.8'",
    # device diagnostics, from the X-Pip-Diag header boxes send on every
    # request (stats.device_diag): what it runs, how it is doing
    "ALTER TABLE devices ADD COLUMN fw_version TEXT",
    "ALTER TABLE devices ADD COLUMN rssi INTEGER",
    "ALTER TABLE devices ADD COLUMN heap INTEGER",      # free internal RAM
    "ALTER TABLE devices ADD COLUMN heap_max INTEGER",  # largest free block
    "ALTER TABLE devices ADD COLUMN boot_at TEXT",      # now - uptime
    "ALTER TABLE devices ADD COLUMN reset TEXT",        # last reset reason
    # the server's current belief, so online/offline transitions can be
    # logged as events (stats.presence_sweep)
    "ALTER TABLE devices ADD COLUMN online INTEGER NOT NULL DEFAULT 0",
    # the Wi-Fi network a box is on (X-Pip-Diag 's='). Shown only to the
    # box's device admin via /v1/managed; a personal datum, so it lives on
    # this row and nowhere else (never in events/hourly, never rendered to
    # the server admin)
    "ALTER TABLE devices ADD COLUMN ssid TEXT",
    # group fan-out: one ordinary row (and audio file) per recipient.
    # group_id tags the copy for presentation - no FK on purpose, rows
    # outlive the group. origin = id of the first copy of one send, so the
    # per-(sender, group) send cap counts sends, not copies.
    "ALTER TABLE messages ADD COLUMN group_id TEXT",
    "ALTER TABLE messages ADD COLUMN origin TEXT",
    "CREATE INDEX IF NOT EXISTS messages_group "
    "ON messages(sender, group_id, created)",
]

# ---------- groups ----------
GROUP_ID_RE = re.compile(r"g-[0-9a-f]{8}")
BOX_NAME_BYTES = 23        # firmware UI_NAME_LEN - 1: what a box can show


def is_group_id(s: str) -> bool:
    return bool(s) and GROUP_ID_RE.fullmatch(s) is not None


def clip_utf8(s: str, n: int) -> str:
    """Longest prefix of s that fits n bytes without splitting a codepoint
    (the firmware's strlcpy would)."""
    b = s.encode("utf-8")
    return s if len(b) <= n else b[:n].decode("utf-8", "ignore")


def group_label(sender_name: str, group_name: str) -> str:
    """'Sender · Group' as the box's sender_name, within its 23 B budget.
    The group name wins: it is what the tile and the reply are about."""
    sep = " · "                         # 4 bytes
    budget = BOX_NAME_BYTES - len(sep.encode()) - len(group_name.encode())
    if budget >= 3:
        return clip_utf8(sender_name, budget) + sep + group_name
    return clip_utf8(group_name, BOX_NAME_BYTES)


def prune_group_members(c) -> int:
    """Membership invariant: a member is the box itself, its device admin,
    or a current contact of the box (perms box -> user). Runs after
    anything that can shrink the perms matrix or change the admin, and at
    boot. Idempotent; returns rows removed."""
    cur = c.execute("""DELETE FROM group_members WHERE rowid IN (
        SELECT gm.rowid FROM group_members gm
          JOIN groups g ON g.id = gm.group_id
          JOIN devices d ON d.id = g.device_id
         WHERE gm.user_id != d.user_id
           AND gm.user_id IS NOT d.admin_id
           AND NOT EXISTS (SELECT 1 FROM perms p
                           WHERE p.sender = d.user_id
                             AND p.recipient = gm.user_id))""")
    return cur.rowcount


def init():
    os.makedirs(AUDIO_DIR, exist_ok=True)
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    with conn() as c:
        # WAL: the analytics page reads a week of counters while requests
        # keep writing; in rollback-journal mode every such read would
        # block writers. Persistent, so once is enough; the -wal/-shm
        # siblings appear next to the db (backup.sh's .backup copes).
        c.execute("PRAGMA journal_mode=WAL")
        c.executescript(SCHEMA)
        for m in MIGRATIONS:
            try:
                c.execute(m)
            except sqlite3.OperationalError:
                pass                     # column already exists
        # one-off table rebuild (SQLite can't ALTER a primary key): the
        # firmware PK grew from (version) to (version, board) for
        # multi-SKU releases. Detected by the stored CREATE statement;
        # idempotent - rebuilt tables carry the composite PK verbatim.
        fw_sql = one(c, """SELECT sql FROM sqlite_master
                           WHERE type='table' AND name='firmware'""")
        if fw_sql and "PRIMARY KEY (version, board)" not in fw_sql["sql"]:
            c.executescript("""
                ALTER TABLE firmware RENAME TO firmware_old;
                CREATE TABLE firmware (
                    version TEXT NOT NULL,
                    notes TEXT DEFAULT '',
                    active INTEGER NOT NULL DEFAULT 0,
                    created TEXT NOT NULL DEFAULT (datetime('now')),
                    board TEXT NOT NULL DEFAULT 'amoled-1.8',
                    PRIMARY KEY (version, board)
                );
                INSERT INTO firmware (version, notes, active, created, board)
                    SELECT version, notes, active, created, board
                    FROM firmware_old;
                DROP TABLE firmware_old;
            """)
        # without local auth, login is email code only: blank any stored
        # hash so old secrets don't linger in the file. Self-hosters keep
        # theirs (PIP_LOCAL_AUTH=1, see auth.py).
        if os.environ.get("PIP_LOCAL_AUTH") != "1":
            c.execute("UPDATE users SET password_hash='' "
                      "WHERE password_hash!=''")
        # broker passwords are no longer kept (a rekey mints a new one);
        # blank whatever pre-1.3.12 provisioning left behind
        c.execute("UPDATE devices SET mqtt_password='' WHERE mqtt_password!=''")
        # permissions are symmetric (a pair either talks or it doesn't);
        # both directions are stored so send/contacts queries stay simple.
        # Idempotent, runs every boot: also migrates pre-symmetric rows.
        c.execute("""INSERT OR IGNORE INTO perms (sender, recipient)
                     SELECT recipient, sender FROM perms""")
        prune_group_members(c)
        # rows acked before the delivered_at column existed: stamp them now
        # so delete-on-delivery picks them up after the normal grace window
        c.execute("""UPDATE messages SET delivered_at=datetime('now')
                     WHERE delivered=1 AND delivered_at IS NULL""")
    # not world-readable: the db holds token/login-code hashes
    for path in (DB_PATH, DB_PATH + "-wal", DB_PATH + "-shm"):
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
    try:
        os.chmod(DATA_DIR, 0o750)
    except OSError:
        pass


@contextmanager
def conn():
    c = sqlite3.connect(DB_PATH)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA foreign_keys=ON")
    try:
        yield c
        c.commit()
    finally:
        c.close()


def one(c, sql, args=()):
    return c.execute(sql, args).fetchone()


def all_(c, sql, args=()):
    return c.execute(sql, args).fetchall()


def audio_path(msg_id: str) -> str:
    return os.path.join(AUDIO_DIR, f"{msg_id}.vmsg")


def playback_path(msg_id: str) -> str:
    """Browser-playable rendering of the same message (vmsg.ensure_playback
    creates it). Derived, never authoritative: deleting it costs a re-render,
    and it goes wherever the .vmsg goes."""
    return os.path.join(AUDIO_DIR, f"{msg_id}.m4a")


# every on-disk rendering of one message: the .vmsg the boxes download and
# the .m4a the PWA plays. They live and die together — the .m4a is derived,
# so leaving one behind is a privacy leak, not a cache hit.
AUDIO_EXTS = (".vmsg", ".m4a")


def drop_audio(msg_id: str) -> bool:
    """Remove every rendering of a message. True if anything was there."""
    gone = False
    for path in (audio_path(msg_id), playback_path(msg_id)):
        try:
            os.remove(path)
            gone = True
        except FileNotFoundError:
            pass
    return gone


def audio_usage() -> tuple[int, int]:
    """(messages with audio still on disk, bytes used by all renderings).

    The messages table is not the answer: a device recipient's row outlives
    its audio by up to RETENTION_DAYS (cleanup.py keeps the row so the box's
    inbox mirror survives), so rows overcount what is actually stored here.
    One .vmsg per message is the real count; the derived .m4a only adds to
    the size."""
    n = size = 0
    try:
        entries = os.scandir(AUDIO_DIR)
    except FileNotFoundError:
        return 0, 0
    with entries as it:
        for e in it:
            if not e.name.endswith(AUDIO_EXTS) or not e.is_file():
                continue
            if e.name.endswith(".vmsg"):
                n += 1
            try:
                size += e.stat().st_size
            except OSError:      # swept between scandir and stat
                pass
    return n, size


def firmware_path(version: str, board: str = "amoled-1.8") -> str:
    """The default board keeps the bare legacy filename: old OTA URLs and
    already-stored files stay valid."""
    if board == "amoled-1.8":
        return os.path.join(FIRMWARE_DIR, f"{version}.bin")
    return os.path.join(FIRMWARE_DIR, f"{version}.{board}.bin")
