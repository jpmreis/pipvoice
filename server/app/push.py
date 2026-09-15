"""push: web push to phone users' installed PWAs (pywebpush + VAPID).

The VAPID keypair is generated on first use into DATA_DIR (survives deploys,
zero manual provisioning). A 404/410 from a push endpoint is the definitive
"subscription expired" signal — the row is pruned so the caller can fall back
to email. Transient failures (429/5xx/network) keep the row.
"""
import json
import logging
from urllib.parse import urlsplit

from . import db, stats

log = logging.getLogger("push")

# push services want a reachable contact for the sender; set
# PIP_VAPID_SUBJECT (mailto:you@your.domain) — defaults to pip@<base host>
VAPID_SUBJECT = db.env("VAPID_SUBJECT", "") or ("mailto:pip@" + (
    db.env("BASE_URL", "").split("//")[-1].split("/")[0].split(":")[0]
    or "localhost"))


def _pem_path() -> str:
    return db.env("VAPID_PEM",
                  __import__("os").path.join(db.DATA_DIR, "vapid_private.pem"))


_pub_cache = None
_priv_cache = None


def _vapid():
    from py_vapid import Vapid
    path = _pem_path()
    import os
    if not os.path.exists(path):
        v = Vapid()
        v.generate_keys()
        v.save_key(path)
        os.chmod(path, 0o600)
        log.info("generated VAPID keypair at %s", path)
    return Vapid.from_file(path)


def _private_key() -> str:
    """Raw base64url private key: the one form every pywebpush version
    accepts (2.x no longer auto-detects PEM file paths)."""
    global _priv_cache
    if _priv_cache is None:
        from py_vapid import b64urlencode
        raw = _vapid().private_key.private_numbers() \
            .private_value.to_bytes(32, "big")
        _priv_cache = b64urlencode(raw)
    return _priv_cache


def public_key() -> str:
    """base64url uncompressed P-256 public key (applicationServerKey)."""
    global _pub_cache
    if _pub_cache is None:
        from cryptography.hazmat.primitives import serialization
        from py_vapid import b64urlencode
        raw = _vapid().public_key.public_bytes(
            serialization.Encoding.X962,
            serialization.PublicFormat.UncompressedPoint)
        _pub_cache = b64urlencode(raw)
    return _pub_cache


# Push endpoints the server will POST to. A subscription's endpoint is
# whatever the browser hands us, and the server then makes an outbound
# request to it on every message - without this list any signed-in user
# could point that request at anything reachable from the server (the
# compose network, the cloud metadata address, ...). The browsers we
# target use exactly these services; PIP_PUSH_HOSTS adds more (comma-
# separated hostnames, subdomains included) for other browsers.
PUSH_HOSTS = ("fcm.googleapis.com", "push.apple.com",
              "push.services.mozilla.com", "notify.windows.com",
              "push.samsungosp.com")
PUSH_HOSTS += tuple(h.strip().lower() for h in
                    db.env("PUSH_HOSTS", "").split(",") if h.strip())


def valid_endpoint(endpoint) -> bool:
    """https, and a host that is one of PUSH_HOSTS or a subdomain of one."""
    if not isinstance(endpoint, str) or len(endpoint) > 2048:
        return False
    try:
        u = urlsplit(endpoint)
    except ValueError:
        return False
    host = (u.hostname or "").lower()
    return (u.scheme == "https" and bool(host)
            and any(host == h or host.endswith("." + h) for h in PUSH_HOSTS))


def save_subscription(user_id: int, sub: dict) -> None:
    if not valid_endpoint(sub.get("endpoint")):
        raise ValueError("push endpoint is not a known push service")
    keys = sub.get("keys") or {}
    if not isinstance(keys.get("p256dh"), str) \
            or not isinstance(keys.get("auth"), str):
        raise ValueError("push subscription has no keys")
    with db.conn() as c:
        c.execute("""INSERT INTO push_subs (endpoint,user_id,p256dh,auth)
                     VALUES (?,?,?,?)
                     ON CONFLICT(endpoint) DO UPDATE SET
                       user_id=excluded.user_id, p256dh=excluded.p256dh,
                       auth=excluded.auth""",
                  (sub["endpoint"], user_id,
                   sub["keys"]["p256dh"], sub["keys"]["auth"]))


def drop_subscription(endpoint: str, user_id: int) -> None:
    """Scoped to the owner: an endpoint URL is a capability to silence
    that install, so nobody else's knowledge of it may delete it."""
    with db.conn() as c:
        c.execute("DELETE FROM push_subs WHERE endpoint=? AND user_id=?",
                  (endpoint, user_id))


def send_to_user(user_id: int, payload: dict) -> int:
    """Push to every subscription of this user. Returns the number of pushes
    accepted by the push service; prunes dead (404/410) subscriptions."""
    from pywebpush import WebPushException, webpush

    with db.conn() as c:
        subs = db.all_(c, "SELECT * FROM push_subs WHERE user_id=?", (user_id,))
    accepted = 0
    for s in subs:
        info = {"endpoint": s["endpoint"],
                "keys": {"p256dh": s["p256dh"], "auth": s["auth"]}}
        try:
            webpush(info, json.dumps(payload),
                    vapid_private_key=_private_key(),
                    vapid_claims={"sub": VAPID_SUBJECT}, timeout=10)
            accepted += 1
            with db.conn() as c:
                c.execute("UPDATE push_subs SET last_ok=datetime('now') "
                          "WHERE endpoint=?", (s["endpoint"],))
            stats.event("push.ok", user_id=user_id,
                        msg_id=payload.get("msg_id") or None)
        except WebPushException as e:
            code = e.response.status_code if e.response is not None else None
            if code in (404, 410):
                drop_subscription(s["endpoint"], user_id)
                log.info("pruned dead subscription for user %d (%s)",
                         user_id, code)
                stats.event("push.pruned", user_id=user_id, detail=str(code))
            else:
                log.warning("push failed for user %d: %s", user_id, e)
                stats.event("push.fail", user_id=user_id,
                            detail=f"{code or 'no response'}")
        except Exception as e:                     # DNS/timeout/etc: transient
            log.warning("push error for user %d: %s", user_id, e)
            stats.event("push.fail", user_id=user_id,
                        detail=type(e).__name__)
    return accepted
