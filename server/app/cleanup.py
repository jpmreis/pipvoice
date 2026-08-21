"""cleanup: privacy retention + phone-user reminder emails.
Messages leave the server on delivery: GRACE_HOURS after the first ack
the audio file is deleted, and for phone-user recipients the message row
goes with it (their PWAs stream from the server, so the grace window is
what lets a second phone on the same account fetch it, and allows short-
term replay). Device-user rows must outlive their audio: the box keeps
its own copy and mirrors the server inbox (sync.c reconcile drops any
local message missing from /v1/inbox), so deleting the row would wipe
the box too — those rows age out after RETENTION_DAYS, exactly like the
box's effective retention today. Undelivered messages give up after
RETENTION_DAYS. Audio files without a DB row are swept; phone users with
messages still undelivered after REMIND_HOURS get one reminder email
(push either never arrived or the subscription died silently)."""
import asyncio
import logging
import os

from . import db, notify

log = logging.getLogger("cleanup")
RETENTION_DAYS = int(db.env("RETENTION_DAYS", "30"))
GRACE_HOURS = int(db.env("DELIVERED_GRACE_H", "24"))
REMIND_HOURS = int(db.env("REMIND_H", "12"))


def cleanup_once(retention_days: int = RETENTION_DAYS,
                 grace_hours: int = GRACE_HOURS) -> dict:
    purged = trimmed = swept = 0
    with db.conn() as c:
        # delete-on-delivery, grace_hours after the first ack
        done = db.all_(c, """SELECT id, EXISTS(SELECT 1 FROM devices d
                                    WHERE d.user_id=m.recipient) AS boxed
                             FROM messages m WHERE delivered=1
                               AND delivered_at IS NOT NULL
                               AND delivered_at < datetime('now', ?)""",
                       (f"-{grace_hours} hours",))
        for r in done:
            if not r["boxed"]:
                c.execute("DELETE FROM messages WHERE id=?", (r["id"],))
                purged += 1
            try:
                os.remove(db.audio_path(r["id"]))
                if r["boxed"]:
                    trimmed += 1
            except FileNotFoundError:
                pass
        # row age-out: undelivered messages give up, and audio-less
        # device-recipient rows (kept above for the box's inbox) expire
        old = db.all_(c, """SELECT id FROM messages
                            WHERE created < datetime('now', ?)""",
                      (f"-{retention_days} days",))
        for r in old:
            c.execute("DELETE FROM messages WHERE id=?", (r["id"],))
            try:
                os.remove(db.audio_path(r["id"]))
            except FileNotFoundError:
                pass
            purged += 1
        # seen reactions age out like messages; unseen ones wait for the
        # sender however long it takes (tiny rows, no audio attached)
        c.execute("""DELETE FROM reactions WHERE seen=1
                     AND created < datetime('now', ?)""",
                  (f"-{retention_days} days",))
        ids = {r["id"] for r in db.all_(c, "SELECT id FROM messages")}
    for fn in os.listdir(db.AUDIO_DIR):
        if fn.endswith(".vmsg") and fn[:-5] not in ids:
            os.remove(os.path.join(db.AUDIO_DIR, fn))
            swept += 1
    if purged or trimmed or swept:
        log.info("purged %d messages, trimmed %d delivered audio files, "
                 "swept %d orphan files", purged, trimmed, swept)
    return {"purged": purged, "trimmed": trimmed, "swept": swept}


def remind_once(remind_hours: int = REMIND_HOURS) -> int:
    """One reminder email per stale undelivered message to a phone user."""
    with db.conn() as c:
        rows = db.all_(c, """SELECT m.id, m.recipient, u.display_name AS sender
                             FROM messages m JOIN users u ON u.id=m.sender
                             WHERE m.delivered=0 AND m.reminded=0
                               AND m.created < datetime('now', ?)
                               AND NOT EXISTS (SELECT 1 FROM devices d
                                               WHERE d.user_id=m.recipient)""",
                      (f"-{remind_hours} hours",))
    sent = 0
    for r in rows:
        if notify.send_email(
                r["recipient"],
                f"You have an unheard Pip message from {r['sender']}",
                f"{r['sender']} sent you a voice message a while ago and it "
                f"hasn't been played yet.\n\nListen here: {notify.app_url()}\n"):
            sent += 1
        with db.conn() as c:   # mark even on failure: never spam retries
            c.execute("UPDATE messages SET reminded=1 WHERE id=?", (r["id"],))
    if sent:
        log.info("sent %d reminder emails", sent)
    return sent


async def cleanup_loop():
    """Hourly: reminders need finer cadence than the old daily purge, and
    running retention hourly is harmless at this scale."""
    while True:
        try:
            cleanup_once()
            remind_once()
        except Exception as e:
            log.warning("cleanup failed: %s", e)
        await asyncio.sleep(3600)
