"""mqtt: publish notifies to devices; manage mosquitto credentials.

Degrades gracefully when mosquitto isn't installed (dev/test machines):
every operation logs and continues, devices fall back to inbox polling.
"""
import logging
import os
import subprocess
import threading

from . import db, stats

log = logging.getLogger("mqtt")

# passwd rewrite + ACL rebuild + broker reload is a global side effect;
# concurrent provisions (PWA setup flow) must not interleave the files
_prov_lock = threading.Lock()

MQTT_HOST = db.env("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(db.env("MQTT_PORT", "1883"))
MQTT_USER = db.env("MQTT_USER", "server")
MQTT_PASS = db.env("MQTT_PASS", "")
# these live in a directory the app user can write (mosquitto_passwd rewrites
# via a temp file, so it needs write access to the directory, not just the file)
PASSWD_FILE = db.env("MQTT_PASSWD", "/etc/mosquitto/pipvoice/passwd")
ACL_FILE = db.env("MQTT_ACL", "/etc/mosquitto/pipvoice/acl")


def notify_user(user_id: int, payload: str = "{}") -> None:
    """Publish to every device of this user."""
    try:
        import paho.mqtt.publish as publish
        with db.conn() as c:
            ids = [r["id"] for r in
                   db.all_(c, "SELECT id FROM devices WHERE user_id=?", (user_id,))]
        msgs = [(f"dev/{d}/notify", payload, 1, False) for d in ids]
        if msgs:
            publish.multiple(msgs, hostname=MQTT_HOST, port=MQTT_PORT,
                             auth={"username": MQTT_USER, "password": MQTT_PASS}
                             if MQTT_PASS else None)
    except Exception as e:                      # broker down / not installed
        log.warning("notify skipped: %s", e)
        stats.event("mqtt.fail", user_id=user_id, detail=type(e).__name__)


def notify_all(payload: str = "{}", retain: bool = True) -> None:
    """Publish to every provisioned device (e.g. new firmware activated).

    Retained by default: a box that is offline at activation time would
    otherwise miss the push forever (clean MQTT session) and wait for its
    daily check - the broker re-delivers a retained notify the moment it
    reconnects and subscribes. Safe because firmware notifies are
    idempotent: an up-to-date box just no-ops the manifest check."""
    try:
        import paho.mqtt.publish as publish
        with db.conn() as c:
            ids = [r["id"] for r in db.all_(c, "SELECT id FROM devices")]
        msgs = [(f"dev/{d}/notify", payload, 1, retain) for d in ids]
        if msgs:
            publish.multiple(msgs, hostname=MQTT_HOST, port=MQTT_PORT,
                             auth={"username": MQTT_USER, "password": MQTT_PASS}
                             if MQTT_PASS else None)
    except Exception as e:                      # broker down / not installed
        log.warning("notify_all skipped: %s", e)
        stats.event("mqtt.fail", detail=type(e).__name__)


def provision_device(device_id: str, mqtt_password: str) -> None:
    """Add broker credentials for a new device and rebuild the ACL."""
    try:
        with _prov_lock:
            subprocess.run(["mosquitto_passwd", "-b", PASSWD_FILE,
                            device_id, mqtt_password], check=True)
            _rebuild_acl()
            _reload_broker()
    except Exception as e:
        log.warning("mosquitto provisioning skipped: %s", e)


def revoke_device(device_id: str) -> None:
    """Remove broker credentials for a deleted device."""
    try:
        with _prov_lock:
            subprocess.run(["mosquitto_passwd", "-D", PASSWD_FILE, device_id],
                           check=False)
            _rebuild_acl()
            _reload_broker()
    except Exception as e:
        log.warning("mosquitto revoke skipped: %s", e)


def _rebuild_acl() -> None:
    lines = [f"user {MQTT_USER}", "topic write dev/+/notify",
             "topic read presence/+", ""]
    with db.conn() as c:
        for r in db.all_(c, "SELECT id FROM devices"):
            lines += [f"user {r['id']}",
                      f"topic read dev/{r['id']}/notify",
                      f"topic write presence/{r['id']}", ""]
    with open(ACL_FILE, "w") as f:
        f.write("\n".join(lines))


def _reload_broker() -> None:
    subprocess.run(["sudo", "systemctl", "reload", "mosquitto"], check=False)
