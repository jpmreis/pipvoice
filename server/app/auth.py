"""auth: email login codes, bearer tokens for devices and user sessions,
FastAPI dependencies. No external crypto deps.

Passwords are opt-in for self-hosters (PIP_LOCAL_AUTH=1): scrypt-hashed
local passwords let a family skip SMTP entirely. Off (the default),
login is email-code only and any stored hashes are blanked at boot."""
import hashlib
import secrets
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Optional

from fastapi import Depends, HTTPException, Request

from . import db

LOCAL_AUTH = db.env("LOCAL_AUTH", "") == "1"


# ---------- tokens ----------
def new_token() -> tuple[str, str]:
    raw = secrets.token_urlsafe(32)
    return raw, hashlib.sha256(raw.encode()).hexdigest()


def token_hash(raw: str) -> str:
    return hashlib.sha256(raw.encode()).hexdigest()


def create_session(user_id: int, days: int = 30) -> str:
    raw, th = new_token()
    # same format as SQLite's datetime('now') so the lexical comparison in
    # _identity_from_token is correct
    expires = (datetime.now(timezone.utc)
               + timedelta(days=days)).strftime("%Y-%m-%d %H:%M:%S")
    with db.conn() as c:
        c.execute("INSERT INTO sessions (token_hash,user_id,expires) VALUES (?,?,?)",
                  (th, user_id, expires))
    return raw


# ---------- local passwords (PIP_LOCAL_AUTH=1 only) ----------
def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    key = hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1)
    return f"scrypt${salt.hex()}${key.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        scheme, salt_hex, key_hex = stored.split("$")
        if scheme != "scrypt":
            return False
        key = hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt_hex),
                             n=2**14, r=8, p=1)
        return secrets.compare_digest(key.hex(), key_hex)
    except (ValueError, AttributeError):
        return False


# ---------- email login codes ----------
CODE_TTL_MIN = 10
CODE_MAX_ATTEMPTS = 5


def user_by_email(email: str):
    email = (email or "").strip().lower()
    if not email:
        return None
    with db.conn() as c:
        return db.one(c, "SELECT * FROM users WHERE lower(email)=?", (email,))


def issue_login_code(user_id: int) -> str:
    """Create (or replace) the user's single active code; returns plaintext
    for the email. Never log it."""
    code = f"{secrets.randbelow(10**6):06d}"
    expires = (datetime.now(timezone.utc)
               + timedelta(minutes=CODE_TTL_MIN)).strftime("%Y-%m-%d %H:%M:%S")
    with db.conn() as c:
        c.execute("""INSERT OR REPLACE INTO login_codes
                     (user_id,code_hash,expires,attempts) VALUES (?,?,?,0)""",
                  (user_id, token_hash(code), expires))
    return code


def redeem_login_code(user_id: int, code: str) -> bool:
    """Single use: the row is deleted on success and burned after
    CODE_MAX_ATTEMPTS wrong guesses."""
    with db.conn() as c:
        row = db.one(c, """SELECT * FROM login_codes WHERE user_id=?
                           AND expires > datetime('now')""", (user_id,))
        if not row or row["attempts"] >= CODE_MAX_ATTEMPTS:
            return False
        if not secrets.compare_digest(row["code_hash"],
                                      token_hash(code.strip())):
            c.execute("UPDATE login_codes SET attempts=attempts+1 "
                      "WHERE user_id=?", (user_id,))
            return False
        c.execute("DELETE FROM login_codes WHERE user_id=?", (user_id,))
    return True


# ---------- login rate limiting ----------
# in-memory is fine: single uvicorn worker, family scale
_FAIL_WINDOW = 15 * 60
_FAIL_LIMIT = 5
_failures: dict[str, list[float]] = {}


def client_ip(request: Request) -> str:
    """Only Caddy can reach the loopback bind, so X-Forwarded-For is trusted."""
    fwd = request.headers.get("x-forwarded-for", "")
    first = fwd.split(",")[0].strip()
    return first or (request.client.host if request.client else "?")


def login_blocked(key: str) -> bool:
    now = time.time()
    hits = [t for t in _failures.get(key, []) if now - t < _FAIL_WINDOW]
    _failures[key] = hits
    return len(hits) >= _FAIL_LIMIT


def login_failed(key: str) -> None:
    _failures.setdefault(key, []).append(time.time())
    if len(_failures) > 1000:            # purge stale keys, bound memory
        now = time.time()
        for k in list(_failures):
            if all(now - t >= _FAIL_WINDOW for t in _failures[k]):
                del _failures[k]


def login_succeeded(key: str) -> None:
    _failures.pop(key, None)


# ---------- identity resolution ----------
@dataclass
class Identity:
    user_id: int
    username: str
    display_name: str
    color: str
    is_admin: bool
    device_id: Optional[str]     # set when authenticated via device token


def _identity_from_token(raw: str) -> Optional[Identity]:
    th = token_hash(raw)
    with db.conn() as c:
        d = db.one(c, """SELECT d.id AS device_id, u.* FROM devices d
                         JOIN users u ON u.id=d.user_id WHERE d.token_hash=?""",
                   (th,))
        if d:
            c.execute("UPDATE devices SET last_seen=datetime('now') WHERE id=?",
                      (d["device_id"],))
            return Identity(d["id"], d["username"], d["display_name"],
                            d["color"], bool(d["is_admin"]), d["device_id"])
        s = db.one(c, """SELECT s.expires, u.* FROM sessions s
                         JOIN users u ON u.id=s.user_id
                         WHERE s.token_hash=? AND s.expires > datetime('now')""",
                   (th,))
        if s:
            # sliding expiry: any use in the second half of the window
            # renews it, so active users never see the login screen again
            if s["expires"] < (datetime.now(timezone.utc) + timedelta(days=15)
                               ).strftime("%Y-%m-%d %H:%M:%S"):
                c.execute("UPDATE sessions SET expires=datetime('now','+30 days')"
                          " WHERE token_hash=?", (th,))
            return Identity(s["id"], s["username"], s["display_name"],
                            s["color"], bool(s["is_admin"]), None)
    return None


def require_auth(request: Request) -> Identity:
    hdr = request.headers.get("Authorization", "")
    if hdr.startswith("Bearer "):
        ident = _identity_from_token(hdr[7:])
        if ident:
            return ident
    # PWA path: cookie set by /v1/auth/verify-code. Server-set cookies survive
    # Safari's 7-day script-storage purge and let <audio src> authenticate.
    sid = request.cookies.get("pip_session", "")
    if sid:
        ident = _identity_from_token(sid)
        if ident:
            return ident
    raise HTTPException(401, "invalid or missing token")


def require_admin_cookie(request: Request) -> Identity:
    sid = request.cookies.get("pip_session", "")
    ident = _identity_from_token(sid) if sid else None
    if not ident or not ident.is_admin:
        raise HTTPException(status_code=303, detail="login required",
                            headers={"Location": "/admin/login"})
    return ident


AuthDep = Depends(require_auth)
AdminDep = Depends(require_admin_cookie)
