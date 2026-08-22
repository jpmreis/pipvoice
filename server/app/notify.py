"""notify: fan a new-message event out to the recipient's clients.

A user is either a device user (has rows in devices -> MQTT, firmware
unchanged) or a phone user (installed PWA -> web push; email as fallback).
Ladder for phone users: push every live subscription; if none were accepted
(dead/no subscriptions), email immediately. cleanup.py adds a delayed
reminder email for messages still undelivered hours later.

Sequencing rule for everything in here: a notify is a promise that the
message is *fetchable now*. Nothing announces before the bytes the client
will ask for exist. The audio file is written before the row is inserted
(api.send_message), the phone's .m4a is rendered before the push goes out,
and the device notify carries the message's metadata so the box can go
straight to the audio instead of listing the inbox first.
"""
import json
import logging
import smtplib
from datetime import datetime, timezone
from email.message import EmailMessage

from . import db, mqtt, push, vmsg

log = logging.getLogger("notify")

SMTP_HOST = db.env("SMTP_HOST", "")
SMTP_PORT = int(db.env("SMTP_PORT", "587"))
SMTP_USER = db.env("SMTP_USER", "")
SMTP_PASS = db.env("SMTP_PASS", "")
SMTP_FROM = db.env("SMTP_FROM", SMTP_USER or "pip@localhost")


def app_url() -> str:
    base = (db.env("BASE_URL", "") or "").rstrip("/")
    return f"{base}/app/" if base else "/app/"


def is_device_user(user_id: int) -> bool:
    with db.conn() as c:
        n = db.one(c, "SELECT COUNT(*) n FROM devices WHERE user_id=?",
                   (user_id,))["n"]
    return n > 0


def _epoch(iso: str) -> int:
    """sqlite datetime('now') strings are UTC; return unix seconds."""
    try:
        return int(datetime.fromisoformat(iso)
                   .replace(tzinfo=timezone.utc).timestamp())
    except Exception:
        return 0


def _device_payload(msg_id: str) -> str:
    """The new-message notify for a box, carrying everything the firmware
    needs to write the .meta itself. With this the box does one request
    (the audio) instead of three (GET /inbox, audio, ack) — on a weak link
    each of those is a fresh TLS handshake, which is where the delay the
    family noticed actually lived.

    Kept small on purpose: the firmware reads it into a fixed buffer, and
    a truncated payload just degrades to the old list-then-fetch path."""
    with db.conn() as c:
        m = db.one(c, """SELECT m.duration, m.created, u.username,
                                u.display_name, u.color
                         FROM messages m JOIN users u ON u.id=m.sender
                         WHERE m.id=?""", (msg_id,))
    if not m:
        return json.dumps({"msg_id": msg_id})
    return json.dumps({"msg_id": msg_id, "from": m["username"],
                       "from_name": m["display_name"],
                       "color": m["color"] or "999999",
                       "ts": _epoch(m["created"]),
                       "dur": m["duration"] or 0}, separators=(",", ":"))


def message_created(recipient_id: int, msg_id: str, sender_name: str) -> None:
    if is_device_user(recipient_id):
        mqtt.notify_user(recipient_id, _device_payload(msg_id))
        return
    # Render the compressed playback file first: the service worker starts
    # prefetching it the instant the push lands, and a push that outruns
    # its own audio is exactly the race this is here to close.
    vmsg.ensure_playback(msg_id)
    # title is the sender: iOS already shows the app name ("Pip") on the
    # banner, so a "Pip" title would just repeat it
    accepted = push.send_to_user(recipient_id, {
        "title": sender_name, "body": "New voice message",
        "msg_id": msg_id})
    if accepted == 0:
        send_email(recipient_id,
                   f"{sender_name} sent you a voice message on Pip",
                   f"{sender_name} sent you a voice message.\n\n"
                   f"Listen here: {app_url()}\n")


# wire keys -> display glyphs, for push notification bodies
REACTION_GLYPH = {"heart": "❤️", "up": "\U0001f44d",
                  "down": "\U0001f44e", "haha": "“Ha Ha”",
                  "bang": "“!!”", "quest": "“?”",
                  "joy": "\U0001f602"}


def reaction_created(target_id: int, reactor_name: str,
                     reactor_username: str, reaction: str,
                     msg_id: str) -> None:
    """Fan a reaction out to the original sender. No email fallback:
    reactions are nice-to-have, not delivery-critical."""
    if is_device_user(target_id):
        mqtt.notify_user(target_id, json.dumps(
            {"event": "reaction", "msg_id": msg_id,
             "from": reactor_username, "from_name": reactor_name,
             "reaction": reaction}))
        return
    push.send_to_user(target_id, {
        "title": reactor_name,
        "body": f"Reacted {REACTION_GLYPH.get(reaction, reaction)} "
                f"to your message",
        "msg_id": msg_id})


def send_email(user_id: int, subject: str, body: str,
               html: str | None = None) -> bool:
    """Best-effort; returns True only when the mail was handed to SMTP."""
    if not SMTP_HOST:
        log.warning("email skipped (PIP_SMTP_HOST unset)")
        return False
    with db.conn() as c:
        u = db.one(c, "SELECT email, display_name FROM users WHERE id=?",
                   (user_id,))
    if not u or not u["email"]:
        log.info("email skipped: user %d has no address", user_id)
        return False
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = SMTP_FROM
    msg["To"] = u["email"]
    msg.set_content(body)
    if html:
        msg.add_alternative(html, subtype="html")
    try:
        with smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=15) as s:
            s.starttls()
            if SMTP_USER:
                s.login(SMTP_USER, SMTP_PASS)
            s.send_message(msg)
        return True
    except Exception as e:
        log.warning("email to user %d failed: %s", user_id, e)
        return False
