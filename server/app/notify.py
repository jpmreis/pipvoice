"""notify: fan a new-message event out to the recipient's clients.

A user is either a device user (has rows in devices -> MQTT, firmware
unchanged) or a phone user (installed PWA -> web push; email as fallback).
Ladder for phone users: push every live subscription; if none were accepted
(dead/no subscriptions), email immediately. cleanup.py adds a delayed
reminder email for messages still undelivered hours later.
"""
import json
import logging
import smtplib
from email.message import EmailMessage

from . import db, mqtt, push

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


def message_created(recipient_id: int, msg_id: str, sender_name: str) -> None:
    if is_device_user(recipient_id):
        mqtt.notify_user(recipient_id, '{"msg_id":"%s"}' % msg_id)
        return
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
