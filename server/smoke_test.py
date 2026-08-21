"""End-to-end smoke test: admin bootstrap, users, perms, devices,
message send/inbox/audio/ack/delete, firmware manifest."""
import io, os, sys
os.environ["PIP_DATA"] = "/tmp/vmtest"
os.environ["PIP_BASE_URL"] = "https://voice.test"
os.environ["PIP_LOCAL_AUTH"] = "1"   # exercise self-host password auth too
import shutil; shutil.rmtree("/tmp/vmtest", ignore_errors=True)

from fastapi.testclient import TestClient
from app.main import app

c = TestClient(app, follow_redirects=False)
def ok(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: sys.exit(1)

# --- login-code helper: SMTP is unset in tests, so plant a known code ---
def plant_code(username, code="123456", expires="+10 minutes"):
    """Returns the user's email; the code is ready to verify."""
    from app import db as _db
    from app.auth import token_hash as _th
    with _db.conn() as cc:
        u_ = _db.one(cc, "SELECT id,email FROM users WHERE username=?",
                     (username,))
        cc.execute("""INSERT OR REPLACE INTO login_codes
                      (user_id,code_hash,expires,attempts)
                      VALUES (?,?,datetime('now',?),0)""",
                   (u_["id"], _th(code), expires))
    return u_["email"]

# --- bootstrap admin via web ---
r = c.post("/admin/login", data={"action": "bootstrap", "username": "papa",
                                 "email": "papa@example.com"})
ok(r.status_code == 303, "bootstrap admin created")
cookie = r.cookies.get("pip_session"); c.cookies.set("pip_session", cookie)

# --- create two users ---
for u, n, col in [("ella", "Ella", "F06292"), ("grandma", "Grandma", "BA68C8")]:
    r = c.post("/admin/users", data={"username": u, "display_name": n,
                                     "color": col,
                                     "email": u + "@example.com"})
    ok(r.status_code == 303, f"user {u} created")

# --- permissions: ella <-> grandma both ways (ids 2 and 3) ---
r = c.post("/admin/perms", data={"p_2_3": "on", "p_3_2": "on"})
ok(r.status_code == 303, "permissions saved")

# --- provision devices ---
import re
r = c.post("/admin/devices", data={"device_id": "pip-ella-01", "user_id": 2,
                                   "pin": "1234"})
tok_ella = re.search(r"API token: <code>([^<]+)</code>", r.text).group(1)
ok(r.status_code == 200 and "nvs" in r.text.lower(), "device pip-ella-01 + NVS csv")
r = c.post("/admin/devices", data={"device_id": "pip-grandma-01", "user_id": 3,
                                   "pin": "1234"})
tok_g = re.search(r"API token: <code>([^<]+)</code>", r.text).group(1)
ok(r.status_code == 200, "device pip-grandma-01 provisioned")

H_E = {"Authorization": f"Bearer {tok_ella}"}
H_G = {"Authorization": f"Bearer {tok_g}"}

# --- contacts respect permissions ---
r = c.get("/v1/contacts", headers=H_E)
ok(r.status_code == 200 and [x["device_id"] for x in r.json()] == ["grandma"],
   "ella's contacts = [grandma]")

# --- send a message ella -> grandma ---
vmsg = b"VMSG" + bytes(8) + b"\x02\x00ab" * 50
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": "grandma", "duration": "7"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200, "message uploaded")
mid = r.json()["id"]

# --- permission enforcement: grandma has no perm towards papa ---
# email-code login: unknown address answers ok (no enumeration), wrong and
# expired codes fail, a planted good code signs in and is single-use
r = c.post("/v1/auth/request-code", data={"email": "stranger@example.com"})
ok(r.status_code == 200 and r.json()["ok"], "request-code: unknown email ok")
em_g = plant_code("grandma")
ok(c.post("/v1/auth/verify-code",
          data={"email": em_g, "code": "999999"}).status_code == 401,
   "wrong code rejected")
plant_code("ella", expires="-1 minutes")
ok(c.post("/v1/auth/verify-code",
          data={"email": "ella@example.com",
                "code": "123456"}).status_code == 401,
   "expired code rejected")
r = c.post("/v1/auth/verify-code", data={"email": em_g, "code": "123456"})
ok(r.status_code == 200, "email-code login works")
ok(c.post("/v1/auth/verify-code",
          data={"email": em_g, "code": "123456"}).status_code == 401,
   "code is single-use")
utok = {"Authorization": "Bearer " + r.json()["token"]}
r = c.post("/v1/messages", headers=utok,
           data={"recipient_id": "papa", "duration": "1"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 403, "perm check blocks grandma -> papa")

# --- grandma inbox via DEVICE token and via USER token: same message ---
for hdr, label in [(H_G, "device"), (utok, "user session")]:
    r = c.get("/v1/inbox", headers=hdr)
    ok(r.status_code == 200 and len(r.json()) == 1 and
       r.json()[0]["sender_name"] == "Ella", f"inbox via {label} token")

# --- download, ack, delete ---
r = c.get(f"/v1/messages/{mid}/audio", headers=H_G)
ok(r.status_code == 200 and r.content == vmsg, "audio downloads intact")
ok(c.post(f"/v1/messages/{mid}/ack", headers=H_G).status_code == 200, "ack")
ok(c.delete(f"/v1/messages/{mid}", headers=H_G).status_code == 200, "delete")
ok(c.get("/v1/inbox", headers=H_G).json() == [], "inbox empty after delete")

# --- wrong-owner access is denied ---
r = c.get(f"/v1/messages/{mid}/audio", headers=H_E)
ok(r.status_code == 404, "cross-user message access denied")

# --- firmware OTA ---
r = c.post("/admin/firmware", data={"version": "0.2.0", "notes": "t",
                                    "activate": "1"},
           files={"file": ("fw.bin", io.BytesIO(b"FWDATA"))})
ok(r.status_code == 303, "firmware uploaded+activated")
r = c.get("/v1/firmware", headers=H_E)
ok(r.json()["version"] == "0.2.0" and r.json()["url"].endswith("/v1/firmware/0.2.0.bin"),
   "manifest serves active version")
ok(c.get("/v1/firmware/0.2.0.bin", headers=H_E).content == b"FWDATA",
   "firmware binary downloads")

# --- global version: single variable (active firmware version) ---
r = c.get("/v1/version")
gv = r.json()["version"]
ok(r.status_code == 200 and gv == "0.2.0",
   "global version = active firmware version, nothing else")
ok(c.get("/v1/firmware", headers=H_E).headers.get("X-Pip-Version") == gv,
   "X-Pip-Version header stamped on /v1 responses")
ok(c.get("/v1/webver").json()["version"] == gv,
   "deprecated /v1/webver serves the same single version")

# --- background themes: versioned assets + device thumbnails ---
r = c.get("/v1/themes", headers=H_E)
ts = r.json()
ok(r.status_code == 200 and ts and
   all(re.fullmatch(r"[0-9a-f]{8}", t["ver"]) for t in ts),
   "themes list carries 8-hex content versions")
t0 = ts[0]
r = c.get(f"/v1/themes/{t0['name']}/thumb.bin", headers=H_E)
ok(r.status_code == 200 and len(r.content) == 108 * 132 * 2,
   "device thumbnail is raw RGB565 108x132")
r = c.get(f"/v1/themes/{t0['name']}/thumb.bin?v={t0['ver']}", headers=H_E)
ok("immutable" in r.headers.get("cache-control", ""),
   "matching ?v= caches immutable")
r = c.get(f"/v1/themes/{t0['name']}/web.jpg", headers=H_E)
ok(r.headers.get("cache-control") == "no-cache",
   "version-less asset must revalidate")
r = c.get(f"/v1/themes/{t0['name']}/device.bin?v=stale123", headers=H_E)
ok(r.status_code == 200 and r.headers.get("cache-control") == "no-cache"
   and len(r.content) == 368 * 448 * 2,
   "stale ?v= serves current bytes, uncached")
r = c.post("/v1/theme", headers=H_E, json={"name": t0["name"]})
ok(r.status_code == 200 and
   c.get("/v1/me", headers=H_E).json()["theme_ver"] == t0["ver"],
   "/v1/me reports the active theme's version")
c.post("/v1/theme", headers=H_E, json={"name": None})

# --- auth hygiene ---
bare = TestClient(app, follow_redirects=False)   # no header, no cookie
ok(bare.get("/v1/inbox").status_code == 401, "no token -> 401")
anon = TestClient(app, follow_redirects=False)
ok(anon.get("/admin/users").status_code == 303,
   "admin pages redirect unauthenticated to login")
# --- device revocation ---
r = c.post("/admin/devices/pip-ella-01/delete")
ok(r.status_code == 303, "device revoked via admin")
ok(bare.get("/v1/inbox", headers=H_E).status_code == 401,
   "revoked device token rejected")
r = c.post("/v1/auth/verify-code", data={"email": plant_code("ella"),
                                         "code": "123456"})
ok(r.status_code == 200, "ella's user account unaffected by revocation")

# --- retention cleanup ---
from app.cleanup import cleanup_once
from app import db as adb
import uuid as _uuid
old_id = _uuid.uuid4().hex
with adb.conn() as cc:
    cc.execute("""INSERT INTO messages (id,sender,recipient,duration,created,
                  delivered) VALUES (?,2,3,5,datetime('now','-40 days'),1)""",
               (old_id,))
open(adb.audio_path(old_id), "wb").write(b"VMSGold")
open(adb.audio_path("orphan123"), "wb").write(b"VMSGorphan")
res = cleanup_once(30)
ok(res["purged"] == 1 and res["swept"] == 1,
   "cleanup purged old delivered msg + swept orphan")
import os as _os
ok(not _os.path.exists(adb.audio_path(old_id)), "old audio removed")

# ================= PWA / phone-user features =================
# https base: session cookies are Secure, so httpx only sends them over https
web = TestClient(app, base_url="https://testserver", follow_redirects=False)
web.cookies.set("pip_session", cookie)          # admin session for setup

# --- phone user: no device, has email ---
r = web.post("/admin/users", data={"username": "webby", "display_name": "Webby",
                                   "color": "3DDC84",
                                   "email": "webby@example.com"})
ok(r.status_code == 303, "phone user webby created (with email)")
with adb.conn() as cc:
    wid = adb.one(cc, "SELECT id FROM users WHERE username='webby'")["id"]
    gid = adb.one(cc, "SELECT id FROM users WHERE username='grandma'")["id"]
    em = adb.one(cc, "SELECT email FROM users WHERE id=?", (wid,))["email"]
ok(em == "webby@example.com", "email column persisted")
r = web.post("/admin/perms", data={f"p_{gid}_{wid}": "on", f"p_{wid}_{gid}": "on"})
ok(r.status_code == 303, "webby <-> grandma perms")

# --- login sets an HttpOnly session cookie; cookie-only auth works ---
pweb = TestClient(app, base_url="https://testserver", follow_redirects=False)
r = pweb.post("/v1/auth/verify-code", data={"email": plant_code("webby"),
                                            "code": "123456"})
ok(r.status_code == 200 and "pip_session" in r.cookies, "login sets cookie")
set_cookie = r.headers.get("set-cookie", "")
ok("HttpOnly" in set_cookie and "Path=/" in set_cookie, "cookie is HttpOnly")
r = pweb.get("/v1/me")
ok(r.status_code == 200 and r.json()["username"] == "webby",
   "cookie-only auth works (/v1/me)")

# --- browser upload is transcoded to VMSG; device gets a decodable file ---
import math, struct as _st
sr, secs = 16000, 2
samples = (int(12000 * math.sin(2 * math.pi * 440 * t / sr))
           for t in range(sr * secs))
pcm = b"".join(_st.pack("<h", s) for s in samples)
wav = (_st.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(pcm), b"WAVE",
                b"fmt ", 16, 1, 1, sr, sr * 2, 2, 16, b"data", len(pcm)) + pcm)
r = pweb.post("/v1/messages", data={"recipient_id": "grandma", "duration": "2"},
              files={"audio": ("m.audio", io.BytesIO(wav))})
ok(r.status_code == 200, "browser (non-VMSG) upload accepted")
wmid = r.json()["id"]
r = web.get(f"/v1/messages/{wmid}/audio", headers=H_G)
ok(r.status_code == 200 and r.content[:4] == b"VMSG",
   "stored as VMSG for the device")
hdr = _st.unpack_from("<4H", r.content, 4)
ok(hdr[:3] == (1, 160, 20), "vmsg header matches firmware format")

# --- browser playback: decoded WAV + delivered flag lifecycle ---
r = web.get(f"/v1/messages/{wmid}/audio.wav", headers=H_G)
ok(r.status_code == 200 and r.content[:4] == b"RIFF", "audio.wav decodes to WAV")
r = web.get("/v1/inbox", headers=H_G)
ok(r.json()[0]["delivered"] is False, "inbox exposes delivered=false")
web.post(f"/v1/messages/{wmid}/ack", headers=H_G)
r = web.get("/v1/inbox", headers=H_G)
ok(r.json()[0]["delivered"] is True, "delivered=true after ack")
with adb.conn() as cc:
    dat = adb.one(cc, "SELECT delivered_at FROM messages WHERE id=?",
                  (wmid,))["delivered_at"]
ok(dat is not None, "ack stamps delivered_at")
web.delete(f"/v1/messages/{wmid}", headers=H_G)

# --- web push: subscribe, ladder, 410 prune, email fallback path ---
import pywebpush
import types
r = pweb.get("/v1/push/key")
ok(r.status_code == 200 and len(r.json()["key"]) > 60, "VAPID key generated")
sub = {"endpoint": "https://push.example/sub1",
       "keys": {"p256dh": "BPk", "auth": "aaa"}}
r = pweb.post("/v1/push/subscribe", json=sub)
ok(r.status_code == 200, "push subscribe stored")

pushes = []
real_webpush = pywebpush.webpush
pywebpush.webpush = lambda *a, **k: pushes.append(1)
vmsg_b = b"VMSG" + bytes(8) + b"\x02\x00ab" * 50
r = web.post("/v1/messages", headers=H_G,
             data={"recipient_id": "webby", "duration": "1"},
             files={"audio": ("m.vmsg", io.BytesIO(vmsg_b))})
ok(r.status_code == 200 and len(pushes) == 1,
   "notify ladder: phone user got web push (no MQTT)")
wmid2 = r.json()["id"]

def dead(*a, **k):
    raise pywebpush.WebPushException(
        "gone", response=types.SimpleNamespace(status_code=410))
pywebpush.webpush = dead
r = web.post("/v1/messages", headers=H_G,
             data={"recipient_id": "webby", "duration": "1"},
             files={"audio": ("m.vmsg", io.BytesIO(vmsg_b))})
ok(r.status_code == 200, "send survives dead subscription (falls to email)")
wmid3 = r.json()["id"]
with adb.conn() as cc:
    n = adb.one(cc, "SELECT COUNT(*) n FROM push_subs")["n"]
ok(n == 0, "410 pruned the dead subscription")
pywebpush.webpush = real_webpush

# --- reminder net: undelivered phone-user message gets one email pass ---
from app.cleanup import remind_once
with adb.conn() as cc:
    cc.execute("UPDATE messages SET created=datetime('now','-13 hours') "
               "WHERE id=?", (wmid3,))
remind_once(12)
with adb.conn() as cc:
    rem = adb.one(cc, "SELECT reminded FROM messages WHERE id=?", (wmid3,))
ok(rem["reminded"] == 1, "reminder marked (email skipped, SMTP unset)")

# --- sliding session expiry ---
from app.auth import token_hash as _th
tok_webby = pweb.cookies.get("pip_session")
with adb.conn() as cc:
    cc.execute("UPDATE sessions SET expires=datetime('now','+5 days') "
               "WHERE token_hash=?", (_th(tok_webby),))
pweb.get("/v1/me")
with adb.conn() as cc:
    exp = adb.one(cc, """SELECT expires > datetime('now','+25 days') ext
                         FROM sessions WHERE token_hash=?""",
                  (_th(tok_webby),))
ok(exp["ext"] == 1, "session expiry slides on use")

# --- logout clears session ---
r = pweb.post("/v1/auth/logout")
ok(r.status_code == 200, "logout")
ok(pweb.get("/v1/me").status_code == 401, "session gone after logout")

# ================= reactions =================
# the perms form replaces the whole matrix; restore ella<->grandma too
web.post("/admin/perms", data={"p_2_3": "on", "p_3_2": "on",
                               f"p_{gid}_{wid}": "on", f"p_{wid}_{gid}": "on"})
r = c.post("/v1/auth/verify-code", data={"email": plant_code("ella"),
                                         "code": "123456"})
utok_e = {"Authorization": "Bearer " + r.json()["token"]}
r = c.post("/v1/messages", headers=utok_e,
           data={"recipient_id": "grandma", "duration": "3"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
rmid = r.json()["id"]

ok(c.post(f"/v1/messages/{rmid}/reaction", headers=H_G,
          json={"reaction": "nope"}).status_code == 400,
   "invalid reaction key -> 400")
ok(c.post(f"/v1/messages/{rmid}/reaction", headers=utok_e,
          json={"reaction": "heart"}).status_code == 404,
   "sender cannot react to own message")
ok(c.post(f"/v1/messages/{rmid}/reaction", headers=H_G,
          json={"reaction": "heart"}).status_code == 200, "recipient reacts")
r = c.get("/v1/inbox", headers=H_G)
ok(r.json()[0]["reaction"] == "heart", "inbox row carries own reaction")
r = c.get("/v1/reactions", headers=utok_e)
ok(len(r.json()) == 1 and r.json()[0]["from"] == "grandma"
   and r.json()[0]["reaction"] == "heart" and r.json()[0]["msg_id"] == rmid,
   "sender sees unseen reaction")

# latest wins: still one row, key replaced
c.post(f"/v1/messages/{rmid}/reaction", headers=H_G, json={"reaction": "joy"})
r = c.get("/v1/reactions", headers=utok_e)
ok(len(r.json()) == 1 and r.json()[0]["reaction"] == "joy",
   "re-react replaces (latest wins)")

# seen lifecycle + re-react resets seen
c.post("/v1/reactions/seen", headers=utok_e, json={"from": "grandma"})
ok(c.get("/v1/reactions", headers=utok_e).json() == [],
   "reactions/seen clears the feed")
c.post(f"/v1/messages/{rmid}/reaction", headers=H_G, json={"reaction": "haha"})
ok(len(c.get("/v1/reactions", headers=utok_e).json()) == 1,
   "changed reaction becomes unseen again")

# clearing removes the row
c.post(f"/v1/messages/{rmid}/reaction", headers=H_G, json={"reaction": ""})
ok(c.get("/v1/reactions", headers=utok_e).json() == [], "empty key clears")

# reaction survives message deletion
c.post(f"/v1/messages/{rmid}/reaction", headers=H_G, json={"reaction": "up"})
c.delete(f"/v1/messages/{rmid}", headers=H_G)
r = c.get("/v1/reactions", headers=utok_e)
ok(len(r.json()) == 1 and r.json()[0]["reaction"] == "up",
   "reaction outlives message deletion")
ok(c.post(f"/v1/messages/{rmid}/reaction", headers=H_G,
          json={"reaction": "joy"}).status_code == 404,
   "reacting to a deleted message -> 404 (device drops its marker)")

# phone-user target gets a web push containing the glyph
pweb.post("/v1/auth/verify-code", data={"email": plant_code("webby"),
                                        "code": "123456"})
pweb.post("/v1/push/subscribe", json=sub)
r = pweb.post("/v1/messages", data={"recipient_id": "grandma", "duration": "1"},
              files={"audio": ("m.vmsg", io.BytesIO(vmsg_b))})
pmid = r.json()["id"]
bodies = []
pywebpush.webpush = lambda *a, **k: bodies.append(a[1])  # (sub_info, json)
r = c.post(f"/v1/messages/{pmid}/reaction", headers=H_G,
           json={"reaction": "heart"})
import json as _json
ok(r.status_code == 200 and len(bodies) == 1
   and "❤" in _json.loads(bodies[0])["body"],
   "phone-user sender gets push with emoji")
pywebpush.webpush = real_webpush

# reaction retention: old seen rows purge, old unseen rows survive
with adb.conn() as cc:
    cc.execute("""INSERT INTO reactions (msg_id,reactor,target,reaction,
                  created,seen) VALUES ('oldseen',3,2,'up',
                  datetime('now','-40 days'),1)""")
    cc.execute("""INSERT INTO reactions (msg_id,reactor,target,reaction,
                  created,seen) VALUES ('oldunseen',3,2,'up',
                  datetime('now','-40 days'),0)""")
cleanup_once(30)
with adb.conn() as cc:
    left = {r["msg_id"] for r in
            adb.all_(cc, "SELECT msg_id FROM reactions")}
ok("oldseen" not in left and "oldunseen" in left,
   "cleanup purges old seen reactions, keeps unseen")

# --- delete-on-delivery: past the grace window the audio always goes;
# phone-user rows go with it, device-user rows stay (the box mirrors the
# server inbox, so deleting the row would wipe the box's copy too) ---
ph_id, bx_id, un_id = (_uuid.uuid4().hex for _ in range(3))
with adb.conn() as cc:
    # delivered to a phone user (ella: her device was revoked above)
    cc.execute("""INSERT INTO messages (id,sender,recipient,duration,
                  delivered,delivered_at) VALUES (?,3,2,5,1,
                  datetime('now','-2 days'))""", (ph_id,))
    # delivered to a device user (grandma still has pip-grandma-01)
    cc.execute("""INSERT INTO messages (id,sender,recipient,duration,
                  delivered,delivered_at) VALUES (?,2,3,5,1,
                  datetime('now','-2 days'))""", (bx_id,))
    # never delivered and past retention: given up on
    cc.execute("""INSERT INTO messages (id,sender,recipient,duration,created)
                  VALUES (?,2,3,5,datetime('now','-40 days'))""", (un_id,))
for i in (ph_id, bx_id, un_id):
    open(adb.audio_path(i), "wb").write(b"VMSGx")
res = cleanup_once(30)
with adb.conn() as cc:
    left = {r["id"] for r in adb.all_(cc, "SELECT id FROM messages")}
ok(ph_id not in left and not _os.path.exists(adb.audio_path(ph_id)),
   "delivered phone-user message deleted after grace")
ok(bx_id in left and not _os.path.exists(adb.audio_path(bx_id)),
   "delivered device-user audio trimmed, row kept for the box")
ok(res["trimmed"] == 1, "trimmed audio counted")
ok(un_id not in left and not _os.path.exists(adb.audio_path(un_id)),
   "undelivered message purged after 30 days")

# ================= recording presence =================
from app import presence as apresence
r = c.post("/v1/presence", headers=H_G, json={"to": "ella", "state": "start"})
ok(r.status_code == 200, "presence start accepted")
r = c.get("/v1/presence", headers=utok_e)
ok(r.json() == [{"from": "grandma", "from_name": "Grandma"}],
   "peer sees who is recording")
ok(c.get("/v1/presence", headers=H_G).json() == [],
   "presence is directional (recorder sees nothing)")
r = c.post("/v1/presence", headers=H_G, json={"to": "ella", "state": "stop"})
ok(c.get("/v1/presence", headers=utok_e).json() == [], "stop clears presence")
ok(c.post("/v1/presence", headers=H_G,
          json={"to": "papa", "state": "start"}).status_code == 403,
   "presence blocked without message permission")
ok(c.post("/v1/presence", headers=H_G,
          json={"to": "ella", "state": "hi"}).status_code == 400,
   "bad presence state -> 400")

# TTL expiry prunes without a stop
c.post("/v1/presence", headers=H_G, json={"to": "ella", "state": "start"})
with apresence._lock:
    for peers in apresence._state.values():
        for k in peers:
            u_, d_, _ = peers[k]
            peers[k] = (u_, d_, 0.0)
ok(c.get("/v1/presence", headers=utok_e).json() == [],
   "expired presence entry pruned")

# device MQTT path: presence/<device_id> maps to the owning user
class _Msg:
    topic = "presence/pip-grandma-01"
    payload = b'{"to":"ella","state":"start"}'
apresence._on_message(None, None, _Msg())
ok(c.get("/v1/presence", headers=utok_e).json()[0]["from"] == "grandma",
   "device MQTT publish resolves to its user")
class _Bad:
    topic = "presence/pip-stranger-99"
    payload = b'{"to":"ella","state":"start"}'
apresence._on_message(None, None, _Bad())
ok(len(c.get("/v1/presence", headers=utok_e).json()) == 1,
   "unknown device presence ignored")

# --- PWA static shell is served ---
r = c.get("/app/")
ok(r.status_code == 200 and b"Pip" in r.content, "PWA index served at /app/")
for p in ["/app/sw.js", "/app/manifest.json", "/app/style.css", "/app/app.js",
          "/app/icons/icon-192.png", "/app/fonts/montserrat-400.woff2"]:
    ok(c.get(p).status_code == 200, f"static {p}")

# --- public install page ---
r = c.get("/install")
ok(r.status_code == 307 and r.headers["location"] == "/app/install.html",
   "/install redirects into the PWA scope")
r = c.get("/app/install.html")
ok(r.status_code == 200 and b"Add to Home Screen" in r.content,
   "install instructions page served")

# --- public privacy policy ---
r = c.get("/privacy")
ok(r.status_code == 200 and b"Privacy, in plain words" in r.content,
   "privacy policy page served")
ok(b"/privacy" in c.get("/").content, "landing footer links the policy")

# --- admin: email edit + install email (SMTP unset -> not sent) ---
r = web.post(f"/admin/users/{wid}/email", data={"email": "webby2@example.com"})
with adb.conn() as cc:
    em2 = adb.one(cc, "SELECT email FROM users WHERE id=?", (wid,))["email"]
ok(r.status_code == 303 and em2 == "webby2@example.com",
   "admin per-row email edit")
r = web.post(f"/admin/users/{wid}/send-install")
ok(r.status_code == 303 and r.headers["location"].endswith("sent=0"),
   "send-install reports not-sent while SMTP unset")

# --- self-host local passwords (PIP_LOCAL_AUTH=1) ---
r = c.get("/v1/auth/methods")
ok(r.status_code == 200 and r.json() == {"code": False, "password": True},
   "auth methods: password on, code off (no SMTP)")
r = web.post(f"/admin/users/{wid}/password", data={"password": "short"})
ok(r.status_code == 400, "short password rejected")
r = web.post(f"/admin/users/{wid}/password",
             data={"password": "hunter2secret"})
ok(r.status_code == 303, "admin sets a user's local password")
pw = TestClient(app, base_url="https://testserver", follow_redirects=False)
r = pw.post("/v1/auth/login-password",
            data={"email": "webby2@example.com", "password": "wrong-pass"})
ok(r.status_code == 401, "wrong password rejected")
r = pw.post("/v1/auth/login-password",
            data={"email": "webby2@example.com", "password": "hunter2secret"})
ok(r.status_code == 200 and r.json()["username"] == "webby"
   and r.cookies.get("pip_session"), "password sign-in returns a session")
ok(pw.get("/v1/contacts").status_code == 200,
   "password session authenticates API calls")

# --- flasher server-URL override (self-host: box points where told) ---
from app import provision as prov
csv_ = prov.build_nvs_csv("pip-x-01", "X", "tokentok", "mq", "1234",
                          "https://other.example:8443/")
ok("server_base,data,string,https://other.example:8443/v1" in csv_
   and "mqtt_host,data,string,other.example" in csv_,
   "NVS CSV honors the flasher's server URL override")
try:
    prov.build_nvs_csv("pip-x-01", "X", "t", "m", "1234", "ftp://nope")
    ok(False, "bad server URL rejected")
except Exception:
    ok(True, "bad server URL rejected")

print("ALL PASS")
