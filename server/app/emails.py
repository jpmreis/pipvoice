"""emails: the two branded transactional mails (install invite, login
code). Jinja templates in templates/email/, rendered standalone (no
request), delivered through notify.send_email. Never log a code."""
import os

import jinja2

from . import db, notify
from .auth import LOCAL_AUTH

_env = jinja2.Environment(
    loader=jinja2.FileSystemLoader(
        os.path.join(os.path.dirname(__file__), "templates", "email")),
    autoescape=True)


def _base_url() -> str:
    return (db.env("BASE_URL", "") or "").rstrip("/")


def _render(name: str, **ctx) -> str:
    return _env.get_template(name).render(base=_base_url(), **ctx)


def send_login_code(user_id: int, display_name: str, code: str) -> bool:
    first = display_name.split()[0] if display_name else "there"
    subject = f"{code} is your Pip code"
    text = (f"Hello {first}!\n\n"
            f"Your Pip sign-in code is: {code}\n\n"
            "It expires in 10 minutes, so type it in before it flies off.\n\n"
            "Didn't ask for a code? Someone probably typed their email\n"
            "wrong - you can safely ignore this.\n")
    html = _render("code.html", first=first, code=code)
    return notify.send_email(user_id, subject, text, html=html)


def send_install(user_id: int) -> bool:
    with db.conn() as c:
        u = db.one(c, "SELECT display_name, email FROM users WHERE id=?",
                   (user_id,))
    if not u or not u["email"]:
        return False
    first = u["display_name"].split()[0] if u["display_name"] else "there"
    base = _base_url()
    subject = f"{first}, your family has a walkie-talkie now"
    # with local passwords on (self-host) the "no password" boast is a lie
    coda = ("That's it.\n" if LOCAL_AUTH else
            "That's it.\nNo password. Passwords are for banks.\n")
    text = (f"Hello {first}!\n\n"
            "Your family put a little voice-message app called Pip on the\n"
            "internet, and your spot in it is ready.\n\n"
            f"Set it up here: {base}/install\n\n"
            f"When Pip asks who you are, use this email address\n"
            f"({u['email']}) - it will send you a 6-digit code. {coda}")
    html = _render("install.html", first=first, email=u["email"],
                   local_auth=LOCAL_AUTH)
    return notify.send_email(user_id, subject, text, html=html)
