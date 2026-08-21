"""presence: ephemeral "X is recording to you" state.

In-memory only (single worker, family scale — same precedent as the login
rate limiter). Recorders refresh their entry every ~10 s while recording;
entries expire after TTL so a crashed or powered-off recorder can't leave
a stuck indicator. Devices publish to presence/<device_id> over MQTT
(broker ACL authenticates the publisher); the PWA uses POST /v1/presence.
Fan-out to the recipient's devices rides the existing notify topic.
"""
import json
import logging
import threading
import time

from . import db, mqtt

log = logging.getLogger("presence")

TTL = 20.0          # seconds without a keepalive before an entry expires

# recipient_user_id -> {sender_user_id: (username, display_name, expires)}
_state: dict = {}
_lock = threading.Lock()


def set_recording(sender_id: int, to_username: str, state: str) -> bool:
    """Record (or clear) that sender is recording to to_username, and fan
    the event out to that user's devices. False when not permitted."""
    with db.conn() as c:
        rcpt = db.one(c, "SELECT id FROM users WHERE username=?",
                      (to_username,))
        if not rcpt:
            return False
        # presence flows in the message direction: only leak it along an
        # allowed sender->recipient edge of the perms matrix
        allowed = db.one(c, "SELECT 1 FROM perms WHERE sender=? AND recipient=?",
                         (sender_id, rcpt["id"]))
        if not allowed:
            return False
        u = db.one(c, "SELECT username, display_name FROM users WHERE id=?",
                   (sender_id,))
    with _lock:
        peers = _state.setdefault(rcpt["id"], {})
        if state == "start":
            peers[sender_id] = (u["username"], u["display_name"],
                                time.monotonic() + TTL)
        else:
            peers.pop(sender_id, None)
    # republished on every keepalive: the device side refreshes its own
    # display TTL from each event
    mqtt.notify_user(rcpt["id"], json.dumps(
        {"event": "recording", "from": u["username"],
         "from_name": u["display_name"], "state": state}))
    return True


def get_for(user_id: int) -> list:
    """Who is recording to user_id right now (PWA short-poll)."""
    now = time.monotonic()
    out = []
    with _lock:
        peers = _state.get(user_id, {})
        for sid in [s for s, v in peers.items() if v[2] <= now]:
            del peers[sid]
        for username, display, _exp in peers.values():
            out.append({"from": username, "from_name": display})
    return out


def _on_message(_client, _userdata, msg) -> None:
    try:
        device_id = msg.topic.split("/")[1]
        body = json.loads(msg.payload)
        with db.conn() as c:
            d = db.one(c, "SELECT user_id FROM devices WHERE id=?",
                       (device_id,))
        if not d:
            return
        state = body.get("state", "")
        if state in ("start", "stop"):
            set_recording(d["user_id"], body.get("to", ""), state)
    except Exception as e:
        log.warning("presence message dropped: %s", e)


def start_subscriber() -> None:
    """Persistent MQTT subscription to presence/+. Best-effort: on dev
    machines without a broker this logs and presence device->server is off
    (the PWA HTTP path still works)."""
    try:
        import paho.mqtt.client as mqtt_client
        cl = mqtt_client.Client(
            mqtt_client.CallbackAPIVersion.VERSION2,
            client_id="pip-presence")
        if mqtt.MQTT_PASS:
            cl.username_pw_set(mqtt.MQTT_USER, mqtt.MQTT_PASS)
        cl.on_connect = lambda c, *_: c.subscribe("presence/+", qos=0)
        cl.on_message = _on_message
        cl.connect_async(mqtt.MQTT_HOST, mqtt.MQTT_PORT)
        cl.loop_start()   # own thread, auto-reconnect
    except Exception as e:
        log.warning("presence subscriber not started: %s", e)
