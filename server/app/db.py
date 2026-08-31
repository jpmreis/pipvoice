"""db: SQLite schema + tiny helpers. Single-writer family scale."""
import os
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
    mqtt_password TEXT NOT NULL,
    created TEXT NOT NULL DEFAULT (datetime('now')),
    last_seen TEXT,
    voice INTEGER NOT NULL DEFAULT 0    -- accessibility voice control
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
CREATE TABLE IF NOT EXISTS firmware (
    version TEXT PRIMARY KEY,
    notes TEXT DEFAULT '',
    active INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS reactions ( -- one per message, latest wins;
    msg_id TEXT PRIMARY KEY,           -- no FK to messages: a reaction must
    reactor INTEGER NOT NULL REFERENCES users(id),  -- survive message deletion
    target INTEGER NOT NULL REFERENCES users(id),   -- the original sender
    reaction TEXT NOT NULL,
    created TEXT NOT NULL DEFAULT (datetime('now')),
    seen INTEGER NOT NULL DEFAULT 0
);
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
]


def init():
    os.makedirs(AUDIO_DIR, exist_ok=True)
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    with conn() as c:
        c.executescript(SCHEMA)
        for m in MIGRATIONS:
            try:
                c.execute(m)
            except sqlite3.OperationalError:
                pass                     # column already exists
        # without local auth, login is email code only: blank any stored
        # hash so old secrets don't linger in the file. Self-hosters keep
        # theirs (PIP_LOCAL_AUTH=1, see auth.py).
        if os.environ.get("PIP_LOCAL_AUTH") != "1":
            c.execute("UPDATE users SET password_hash='' "
                      "WHERE password_hash!=''")
        # permissions are symmetric (a pair either talks or it doesn't);
        # both directions are stored so send/contacts queries stay simple.
        # Idempotent, runs every boot: also migrates pre-symmetric rows.
        c.execute("""INSERT OR IGNORE INTO perms (sender, recipient)
                     SELECT recipient, sender FROM perms""")
        # rows acked before the delivered_at column existed: stamp them now
        # so delete-on-delivery picks them up after the normal grace window
        c.execute("""UPDATE messages SET delivered_at=datetime('now')
                     WHERE delivered=1 AND delivered_at IS NULL""")
    # not world-readable: the db holds token/login-code hashes
    try:
        os.chmod(DATA_DIR, 0o750)
        os.chmod(DB_PATH, 0o600)
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


def firmware_path(version: str) -> str:
    return os.path.join(FIRMWARE_DIR, f"{version}.bin")
