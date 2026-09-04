"""Pip server. Run: uvicorn app.main:app --host 127.0.0.1 --port 8080"""
import logging
import os
import re
import threading

import functools

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, HTMLResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

import asyncio

from . import api, db, mqtt, presence, themes, voice
from .api import router as api_router
from .admin import router as admin_router
from .cleanup import cleanup_loop

logging.basicConfig(level=logging.INFO)
db.init()
# Theme variants render in a thread: a no-op when cached, but the first
# boot after a new rendition set (e.g. the per-board sizes) runs ~40
# ffmpeg passes, and the deploy health check must not wait on them.
# available() gates on files existing, so themes appear as they land.
threading.Thread(target=themes.render_all, name="themes",
                 daemon=True).start()

app = FastAPI(title="Pip", docs_url=None, redoc_url=None)
app.include_router(api_router)
app.include_router(admin_router)


@app.middleware("http")
async def _stamp_version(request, call_next):
    # every /v1 response carries the global version, so the PWA notices an
    # update on its next API interaction instead of waiting out the poll -
    # the browser-side stand-in for the devices' MQTT firmware notify
    resp = await call_next(request)
    if request.url.path.startswith("/v1/"):
        resp.headers["X-Pip-Version"] = api.global_version()
    return resp

# the phone-user PWA: /app/ (html=True serves index.html at the root)
app.mount("/app", StaticFiles(
    directory=os.path.join(os.path.dirname(__file__), "web"), html=True),
    name="app")


@app.on_event("startup")
async def _start_cleanup():
    asyncio.create_task(cleanup_loop())
    presence.start_subscriber()
    # voice-control prompt clips: re-render whatever a deploy changed
    # (phrase text, TTS voice) for every voice-enabled box. Runs in its
    # own thread (voice.ensure_user) - unlike themes.render_all above,
    # TTS is too slow for the startup path.
    voice.ensure_all()
    # refresh the broker ACL so already-provisioned devices gain their
    # presence/<id> write line without re-provisioning; best-effort like
    # everything mosquitto (dev machines have no ACL dir)
    try:
        mqtt._rebuild_acl()
        mqtt._reload_broker()
    except Exception:
        pass


@functools.lru_cache(maxsize=None)
def _public_page(name: str) -> str:
    """pages/ live outside the /app static mount because they are served
    processed: __BASE__/__HOST__ tokens are filled from PIP_BASE_URL so
    the same tree serves any deployment, and <!--if-hosted--> /
    <!--if-selfhost--> blocks are kept or dropped by db.hosted() (a
    self-hosted family's pages must not carry the hosted instance's
    claims — waitlist, who runs the server, providers)."""
    base = db.env("BASE_URL", "").rstrip("/")
    host = base.split("//")[-1] or "this server"
    path = os.path.join(os.path.dirname(__file__), "pages", name)
    with open(path, encoding="utf-8") as f:
        page = f.read().replace("__BASE__", base).replace("__HOST__", host)
    drop = "selfhost" if db.hosted() else "hosted"
    return re.sub(rf"<!--if-{drop}-->.*?<!--end-{drop}-->", "", page,
                  flags=re.S)


@app.get("/")
def root():
    """Public landing page. Lives in pages/ (processed, see _public_page)
    and is served at / — outside the PWA's /app/ manifest scope."""
    return HTMLResponse(_public_page("home.html"))


@app.get("/install")
def install():
    """Pretty public URL for the emailed install instructions. The page
    itself lives inside /app/ so it shares the PWA's manifest scope -
    Chrome only offers its native install prompt to in-scope pages."""
    return RedirectResponse("/app/install.html")


@app.get("/privacy")
def privacy():
    """Public plain-language privacy policy (linked from the landing
    footer); lives in web/ like the landing page, served outside /app/."""
    return HTMLResponse(_public_page("privacy.html"))


@app.get("/waitlist")
def waitlist():
    """Public waitlist signup page (form posts to /v1/waitlist). Hosted
    instance only — a self-hosted family has no public signup; the admin
    adds users by hand."""
    if not db.hosted():
        raise HTTPException(404, "waitlist is not enabled")
    return FileResponse(
        os.path.join(os.path.dirname(__file__), "pages", "waitlist.html"))


@app.get("/healthz")
def healthz():
    return {"ok": True}
