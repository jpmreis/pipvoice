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
for bad in ({"username": "Bad Name", "display_name": "X", "color": "F06292"},
            {"username": "okay", "display_name": "X", "color": "red"},
            {"username": "okay", "display_name": "X", "color": "F06292",
             "email": "not an address"},
            {"username": "ella", "display_name": "Dup", "color": "F06292"}):
    ok(c.post("/admin/users", data=bad).status_code in (400, 409),
       f"admin user form rejects {list(bad.values())[0]!r}")

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

# --- voice control (accessibility): flag, prompts, per-device config ---
r = c.get("/v1/device", headers=H_E)
ok(r.status_code == 200 and {k: r.json()[k] for k in ("voice", "prompts")} == {"voice": False, "prompts": []},
   "voice: /v1/device defaults off")
ok(c.get("/v1/device").status_code == 404,
   "voice: /v1/device rejects non-device identities")

r = c.post("/admin/devices/pip-ella-01/voice", data={"on": "1"})
ok(r.status_code == 303, "admin voice toggle -> on")
from app import db as vdb
with vdb.conn() as cc:
    ok(vdb.one(cc, "SELECT voice FROM devices WHERE id='pip-ella-01'")
       ["voice"] == 1, "voice flag stored on the device row")

# deterministic prompt rendering: fake the TTS backend (CI machines have
# neither piper nor espeak-ng) and render synchronously
from app import voice as _voice
import math, struct as _st
_voice.backend = lambda: ("test", "v0")
def _fake_wav(text):
    pcm = b"".join(_st.pack("<h", int(8000 * math.sin(i / 8)))
                   for i in range(16000))                      # 1 s tone
    hdr = (b"RIFF" + _st.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
           + _st.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
           + b"data" + _st.pack("<I", len(pcm)))
    return hdr + pcm
_voice._tts_wav = _fake_wav
ok(_voice._render_user(2) >= 4, "voice prompts rendered (canned + contact)")

r = c.get("/v1/device", headers=H_E)
vd = r.json()
vkeys = {p["key"] for p in vd["prompts"]}
ok(vd["voice"] and {"ask_play", "ask_confirm", "cancelled",
                    "ask_send-grandma"} <= vkeys,
   "voice manifest lists canned + per-contact clips")
vp = next(p for p in vd["prompts"] if p["key"] == "ask_send-grandma")
r = c.get(f"/v1/voice/{vp['key']}.vmsg?v={vp['ver']}", headers=H_E)
ok(r.status_code == 200 and r.content[:4] == b"VMSG"
   and "immutable" in r.headers["cache-control"],
   "prompt clip served as immutable .vmsg")
ok(c.get(f"/v1/voice/{vp['key']}.vmsg?v=00000000",
         headers=H_E).status_code == 404, "stale prompt version 404s")

# device admin flips it off from the PWA endpoint
r = c.post("/admin/devices/pip-ella-01/admin", data={"admin_id": "1"})
ok(r.status_code == 303, "papa set as pip-ella-01's device admin")
r = c.get("/v1/managed")
ok(any(x["device_id"] == "pip-ella-01" and x["voice"] is True
       for x in r.json()), "managed list carries the voice flag")
r = c.post("/v1/managed/pip-ella-01/voice", json={"on": False})
ok(r.status_code == 200 and r.json()["voice"] is False,
   "device admin turns voice off")
# consent rule: a device admin may only add people who already talk
# with THEM (or themselves); papa has no perms row to grandma here
r = c.post("/v1/managed/pip-ella-01/contacts", json={"username": "grandma"})
ok(r.status_code == 403, "device admin cannot add a stranger as box contact")
r = c.post("/v1/managed/pip-ella-01/contacts", json={"username": "@papa"})
ok(r.status_code == 200 and r.json()["existed"] is False,
   "device admin can add themselves as box contact")
r = c.delete("/v1/managed/pip-ella-01/contacts/papa")
ok(r.status_code == 200, "device admin removes a box contact")
r = c.get("/v1/device", headers=H_E)
ok({k: r.json()[k] for k in ("voice", "prompts")} == {"voice": False, "prompts": []},
   "voice off again: config reflects it")

# --- send a message ella -> grandma ---
# a real 1 s recording in the firmware's container: the server now decodes
# every VMSG at ingest (that is what bounds the memory an upload can
# cost), so a stub the decoder rejects is refused, not stored
import math as _math, struct as _struct
from app import vmsg as _vm
_pcm1 = b"".join(_struct.pack("<h", int(8000 * _math.sin(t / 16000 * 2 * _math.pi * 440)))
                 for t in range(16000))
vmsg = _vm._encode_vmsg(_pcm1, 1)
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": "grandma", "duration": "7"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200, "message uploaded")
mid = r.json()["id"]
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": "grandma", "duration": "1"},
           files={"audio": ("m.vmsg", io.BytesIO(b"VMSG" + bytes(8) + b"\x02\x00ab" * 50))})
ok(r.status_code == 400, "undecodable VMSG upload is refused")
# 3 MB of 1-byte silent opus frames would decode to ~2 GB of PCM
_bomb = vmsg[:12] + (_struct.pack("<H", 1) + b"\x08") * 1_000_000
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": "grandma", "duration": "1"},
           files={"audio": ("m.vmsg", io.BytesIO(_bomb))})
ok(r.status_code == 400 and "longer than" in r.text,
   "decode bomb refused at the length cap")

# the box's MQTT notify carries the whole .meta so it can fetch the audio
# directly instead of listing the inbox first (one TLS handshake, not two),
# and must stay small enough for the firmware's fixed payload buffer
from app import notify as _notify
import json as _json
_p = _notify._device_payload(mid)
_d = _json.loads(_p)
ok(_d["msg_id"] == mid and _d["from"] == "ella" and _d["from_name"] == "Ella"
   and _d["dur"] == 7 and _d["ts"] > 1700000000 and len(_d["color"]) == 6,
   "device notify carries sender, colour, ts and duration")
ok(len(_p) < 256, f"device notify fits the firmware buffer ({len(_p)} B)")

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
ok(r.status_code == 200 and r.content[:12] == vmsg[:12]
   and _vm._vmsg_to_pcm(r.content)[0], "audio downloads as a decodable VMSG")
ok(c.post(f"/v1/messages/{mid}/ack", headers=H_G).status_code == 200, "ack")
ok(c.delete(f"/v1/messages/{mid}", headers=H_G).status_code == 200, "delete")
ok(c.get("/v1/inbox", headers=H_G).json() == [], "inbox empty after delete")

# ================= groups (per-box broadcast lists) =================
# papa (1) is pip-ella-01's device admin; the box's only contact is
# grandma (3). A stranger (explicit id, removed again) proves the rule.
from app import db as gdb, api as _api
with gdb.conn() as cc:
    cc.execute("""INSERT INTO users (id,username,display_name,color,
                  password_hash,email) VALUES (99,'stranger','Stranger',
                  '999999','','stranger@example.com')""")
r = c.post("/v1/managed/pip-ella-01/groups",
           json={"name": "Family", "members": ["grandma", "stranger"]})
ok(r.status_code == 403, "group member must be a box contact (or the admin)")
with gdb.conn() as cc:
    cc.execute("DELETE FROM users WHERE id=99")
ok(c.post("/v1/managed/pip-ella-01/groups",
          json={"name": "x" * 17}).status_code == 400,
   "group name capped at 16 characters")
ok(c.post("/v1/managed/pip-grandma-01/groups",
          json={"name": "Nope"}).status_code == 404,
   "groups on a box you don't manage -> 404")
r = c.post("/v1/managed/pip-ella-01/groups",
           json={"name": "Family", "members": ["@grandma", "papa", "ella"]})
ok(r.status_code == 200 and r.json()["group"]["name"] == "Family",
   "device admin creates a group")
grp = r.json()["group"]["id"]
ok(re.fullmatch(r"g-[0-9a-f]{8}", grp) is not None,
   "group id can never look like a username")
ok(sorted(m["username"] for m in r.json()["group"]["members"])
   == ["grandma", "papa"], "members listed; the box itself is implicit")
for i in range(7):
    ok(c.post("/v1/managed/pip-ella-01/groups",
              json={"name": f"g{i}"}).status_code == 200, f"group g{i} created")
ok(c.post("/v1/managed/pip-ella-01/groups",
          json={"name": "ninth"}).status_code == 400, "at most 8 groups per box")
r = c.get("/v1/managed")
ok(any(d["device_id"] == "pip-ella-01" and d["groups"][0]["id"] == grp
       for d in r.json()), "managed list carries the groups")
with gdb.conn() as cc:
    for g in gdb.all_(cc, "SELECT id FROM groups WHERE name LIKE 'g_'"):
        ok(c.delete(f"/v1/managed/pip-ella-01/groups/{g['id']}").status_code
           == 200, f"filler group {g['id']} deleted")

# tiles: every member sees the group first; the tile is what a box
# replies to, so it must survive the firmware's 12-slot cut
tiles = c.get("/v1/contacts", headers=H_E).json()
ok(tiles[0]["device_id"] == grp and tiles[0]["group"] is True
   and tiles[0]["name"] == "Family" and tiles[1]["device_id"] == "grandma",
   "box sees the group tile first, then its contacts")
ok(c.get("/v1/contacts", headers=H_G).json()[0]["device_id"] == grp,
   "member box sees the group tile")
ok(c.get("/v1/contacts").json()[0]["device_id"] == grp,
   "admin (phone member) sees the group tile")
_voice._render_user(2)
ok(any(p["key"] == f"ask_send-{grp}" for p in _voice.manifest(2)),
   "voice prompt rendered for the group tile")

# "Sender · Group" within the box's 23-byte name budget, never splitting
# a codepoint; the group name wins when the sender can't fit
ok(gdb.group_label("Grandmother Josephine", "Family")
   == "Grandmother J · Family", "label clips the sender to fit")
ok(gdb.group_label("Grandma", "Família Reis 16c") == "Família Reis 16c",
   "label keeps the group name when the sender can't fit")
_lab = gdb.group_label("À" * 10, "Family")
ok(len(_lab.encode()) <= 23 and _lab.encode().decode("utf-8") == _lab
   and _lab.endswith(" · Family"), "label never splits a multibyte char")

# a group send from ella's box: one ordinary row + audio copy per other
# member (grandma's box, papa's phone)
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": grp, "duration": "4"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200 and r.json()["copies"] == 2,
   "group send fans out to the other members")
gorigin = r.json()["id"]
with gdb.conn() as cc:
    copies = gdb.all_(cc, "SELECT id, recipient, group_id, origin FROM messages "
                          "WHERE origin=?", (gorigin,))
ok(len(copies) == 2 and {x["recipient"] for x in copies} == {1, 3}
   and all(x["group_id"] == grp for x in copies)
   and len({x["id"] for x in copies}) == 2 and gorigin in {x["id"] for x in copies},
   "one row per member, sender excluded, origin is a copy")
ok(all(os.path.exists(gdb.audio_path(x["id"])) for x in copies),
   "one audio file per copy")
ok(_api._inflight == {}, "inflight reservation released after a group send")
g_copy = next(x["id"] for x in copies if x["recipient"] == 3)
papa_copy = next(x["id"] for x in copies if x["recipient"] == 1)
gm = c.get("/v1/inbox", headers=H_G).json()[0]
ok(gm["id"] == g_copy and gm["sender_id"] == grp
   and gm["sender_name"] == "Ella · Family" and gm["from"] == "ella"
   and gm["from_name"] == "Ella" and gm["group"]["name"] == "Family",
   "box inbox files the copy under the group tile")
pm = c.get("/v1/inbox").json()[0]
ok(pm["id"] == papa_copy and pm["sender_id"] == grp and pm["from_name"] == "Ella",
   "phone inbox carries the group and the real sender")
_p = _notify._device_payload(g_copy)
_d = _json.loads(_p)
ok(_d["from"] == grp and _d["from_name"] == "Ella · Family"
   and _d["dur"] == 4, "device notify for a group copy wears the group identity")
ok(len(_p) < 256, f"group device notify fits the firmware buffer ({len(_p)} B)")
ok(c.get(f"/v1/messages/{g_copy}/audio", headers=H_G).status_code == 200,
   "a copy downloads like any message")
ok(c.get(f"/v1/messages/{papa_copy}/audio", headers=H_G).status_code == 404,
   "a member cannot fetch another member's copy")

# caps: per (sender, group), independent of the direct channels
for i in range(4):
    r = c.post("/v1/messages", headers=H_E,
               data={"recipient_id": grp, "duration": "1"},
               files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200, "five group sends within the window allowed")
ok(c.post("/v1/messages", headers=H_E,
          data={"recipient_id": grp, "duration": "1"},
          files={"audio": ("m.vmsg", io.BytesIO(vmsg))}).status_code == 429,
   "sixth group send refused (cap is per sender+group)")
r = c.post("/v1/messages", headers=H_E,
           data={"recipient_id": "grandma", "duration": "1"},
           files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200, "direct channel unaffected by the group cap")
ok(c.post("/v1/messages", headers=H_E,
          data={"recipient_id": "g-00000000", "duration": "1"},
          files={"audio": ("m.vmsg", io.BytesIO(vmsg))}).status_code == 404,
   "unknown group -> 404")

# reactions go to the real sender; presence lights the group tile
r = c.post("/v1/auth/verify-code", data={"email": plant_code("ella"),
                                         "code": "123456"})
utok_ella = {"Authorization": "Bearer " + r.json()["token"]}
ok(c.post(f"/v1/messages/{g_copy}/reaction", headers=H_G,
          json={"reaction": "heart"}).status_code == 200, "member reacts to a copy")
ok(any(x["msg_id"] == g_copy and x["from"] == "grandma"
       for x in c.get("/v1/reactions", headers=utok_ella).json()),
   "reaction to a group copy reaches the real sender")
c.post("/v1/reactions/seen", headers=utok_ella, json={"all": True})
r = c.post("/v1/presence", headers=H_E, json={"to": grp, "state": "start"})
ok(r.status_code == 200
   and c.get("/v1/presence").json() == [{"from": grp, "from_name": "Ella"}]
   and c.get("/v1/presence", headers=H_G).json()[0]["from"] == grp,
   "recording to a group lights the group tile for the other members")
c.post("/v1/presence", headers=H_E, json={"to": grp, "state": "stop"})
ok(c.get("/v1/presence").json() == [], "stop clears group presence")

# membership follows the contact list: a removed contact leaves the
# group (and can't record or send to it), whatever path removed them
ok(c.delete("/v1/managed/pip-ella-01/contacts/grandma").status_code == 200,
   "device admin removes grandma from the box")
ok(all(t["device_id"] != grp for t in c.get("/v1/contacts", headers=H_G).json()),
   "removed contact loses the group tile")
ok(c.post("/v1/presence", headers=H_G,
          json={"to": grp, "state": "start"}).status_code == 403,
   "non-member cannot record to the group")
ok(c.post("/v1/messages", headers=H_G,
          data={"recipient_id": grp, "duration": "1"},
          files={"audio": ("m.vmsg", io.BytesIO(vmsg))}).status_code == 403,
   "non-member cannot send to the group")
ok(c.get(f"/v1/messages/{g_copy}/audio", headers=H_G).status_code == 200,
   "copies already delivered stay fetchable")
c.post("/admin/perms", data={"p_2_3": "on", "p_3_2": "on"})   # restore
r = c.patch(f"/v1/managed/pip-ella-01/groups/{grp}",
            json={"name": "Fam", "members": ["grandma"]})
ok(r.status_code == 200 and r.json()["group"]["name"] == "Fam"
   and [m["username"] for m in r.json()["group"]["members"]] == ["grandma"],
   "device admin renames and replaces the members")
ok(all(t["device_id"] != grp for t in c.get("/v1/contacts").json()),
   "dropped member loses the tile")
ok(c.get("/v1/inbox", headers=H_G).json()[0]["sender_name"].endswith("· Fam"),
   "existing copies follow the rename")
c.post("/admin/perms", data={})                   # matrix rewrite: nobody talks
with gdb.conn() as cc:
    left = gdb.all_(cc, "SELECT user_id FROM group_members WHERE group_id=?", (grp,))
ok([x["user_id"] for x in left] == [2],
   "admin perms rewrite prunes members who stopped being contacts")
c.post("/admin/perms", data={"p_2_3": "on", "p_3_2": "on"})   # restore

# the group goes: tiles vanish, copies fall back to the real sender
ok(c.delete(f"/v1/managed/pip-ella-01/groups/{grp}").status_code == 200,
   "device admin deletes the group")
ok(all(t["device_id"] != grp for t in c.get("/v1/contacts", headers=H_G).json()),
   "group tile gone")
gm = next(x for x in c.get("/v1/inbox", headers=H_G).json() if x["id"] == g_copy)
ok(gm["sender_id"] == "ella" and gm["sender_name"] == "Ella"
   and gm["group"] is None, "copies of a deleted group show the real sender")
# device deletion cascades its groups (foreign_keys=ON per connection)
r = c.post("/admin/devices", data={"device_id": "pip-tmp-01", "user_id": 3,
                                   "pin": "1234"})
c.post("/admin/devices/pip-tmp-01/admin", data={"admin_id": "1"})
r = c.post("/v1/managed/pip-tmp-01/groups", json={"name": "Tmp"})
ok(r.status_code == 200, "group on a throwaway box")
c.post("/admin/devices/pip-tmp-01/delete")
with gdb.conn() as cc:
    ok(gdb.one(cc, "SELECT COUNT(*) n FROM groups WHERE device_id='pip-tmp-01'")["n"] == 0
       and gdb.one(cc, "SELECT COUNT(*) n FROM group_members WHERE group_id=?",
                   (r.json()["group"]["id"],))["n"] == 0,
       "deleting a device cascades its groups and members")
# leave the inboxes as we found them
for hdr in (H_G, None):
    for m in c.get("/v1/inbox", headers=hdr).json():
        c.delete(f"/v1/messages/{m['id']}", headers=hdr)
ok(c.get("/v1/inbox", headers=H_G).json() == [] and c.get("/v1/inbox").json() == [],
   "group copies deleted by their recipients")

# --- analytics (stats.py): events, diag header, counters, rollup, page ---
from app import stats as _stats, db as _adb
with _adb.conn() as cc:
    kinds = {(r["kind"], r["dim"]) for r in _adb.all_(
        cc, "SELECT kind, dim FROM events WHERE msg_id=?", (mid,))}
    dl = _adb.one(cc, "SELECT value FROM events WHERE kind='msg.delivered' "
                      "AND msg_id=?", (mid,))
ok(kinds == {("msg.sent", "box"), ("msg.delivered", "box"),
             ("msg.deleted", "box")}, f"message lifecycle logged: {kinds}")
ok(dl is not None and dl["value"] is not None and dl["value"] >= 0,
   "delivery latency recorded on first ack")
ok(_stats.route_group("/v1/messages/abc/audio.m4a") == "audio"
   and _stats.route_group("/v1/inbox") == "inbox"
   and _stats.route_group("/admin/analytics") == "admin"
   and _stats.route_group("/v1/setup/nvs/xyz.bin") == "setup",
   "route groups classify paths")
diag = {**H_G, "X-Pip-Diag": "v=9.9.9;b=amoled-1.8;r=-50;h=90000;m=30000;"
                             "u=5000;rr=1;s=48656c6c6f20e298ba"}
ok(c.get("/v1/inbox", headers=diag).status_code == 200, "diag header accepted")
with _adb.conn() as cc:
    drow = _adb.one(cc, "SELECT * FROM devices WHERE id='pip-grandma-01'")
    fw_ev = _adb.one(cc, "SELECT detail FROM events WHERE kind='device.fw' "
                         "AND device_id='pip-grandma-01'")
    ssid_leak = _adb.one(cc, "SELECT COUNT(*) n FROM events "
                             "WHERE detail LIKE '%Hello%'")["n"]
ok(drow["fw_version"] == "9.9.9" and drow["rssi"] == -50
   and drow["heap"] == 90000 and drow["heap_max"] == 30000
   and drow["reset"] == "poweron" and drow["boot_at"], "diag stored on device")
ok(drow["ssid"] == "Hello ☺" and ssid_leak == 0,
   "hex-encoded SSID decoded onto the device row, never into events")
ok(fw_ev and fw_ev["detail"] == "? -> 9.9.9", "first firmware report logged")
c.get("/v1/inbox", headers={**H_G, "X-Pip-Diag": "s=zz;v=9.9.9"})
c.get("/v1/inbox", headers={**H_G, "X-Pip-Diag": "s=" + "41" * 40})
with _adb.conn() as cc:
    drow = _adb.one(cc, "SELECT ssid FROM devices WHERE id='pip-grandma-01'")
ok(drow["ssid"] == "Hello ☺", "malformed or oversized SSID ignored")
c.get("/v1/inbox", headers={**H_E, "X-Pip-Diag": "s=486f6d65"})
ok(any(d["device_id"] == "pip-ella-01" and d["ssid"] == "Home"
       for d in c.get("/v1/managed").json()),
   "device admin sees the box's Wi-Fi network")
c.get("/v1/inbox", headers={**H_G, "X-Pip-Diag": "v=9.9.9;u=10;rr=4"})
with _adb.conn() as cc:
    boot = _adb.one(cc, "SELECT detail FROM events WHERE kind='device.boot' "
                        "AND device_id='pip-grandma-01'")
    fw_n = _adb.one(cc, "SELECT COUNT(*) n FROM events WHERE kind='device.fw'"
                        " AND device_id='pip-grandma-01'")["n"]
ok(boot and boot["detail"] == "panic", "reboot detected from uptime drop")
ok(fw_n == 1, "unchanged firmware version is not re-logged")
c.get("/v1/inbox", headers={**H_G, "X-Pip-Diag": "garbage;;r=x;v="})
ok(True, "malformed diag header ignored")
ok(c.get("/v1/nope", headers=H_G).status_code == 404, "404 for the 4xx counter")
ok(_stats.flush() > 0, "counters flushed to hourly")
with _adb.conn() as cc:
    hr = {r["metric"]: r for r in _adb.all_(
        cc, "SELECT metric, dim, n, total, lo, hi FROM hourly "
            "WHERE metric IN ('req','req.status','diag.rssi','req.client') "
            "AND dim IN ('inbox','4xx','pip-grandma-01','box')")}
ok(hr.get("req") and hr["req"]["n"] >= 3 and hr["req"]["hi"] is not None,
   "request counter has latency")
ok(hr.get("req.status") and hr["req.status"]["n"] >= 1, "4xx counted")
ok(hr.get("diag.rssi") and hr["diag.rssi"]["lo"] == -50, "rssi aggregated")
ok(hr.get("req.client") and hr["req.client"]["n"] >= 3, "box requests counted")
with _adb.conn() as cc:
    cc.execute("""INSERT INTO events (ts, kind) VALUES
                  (datetime('now','-8 days'), 'msg.sent')""")
    cc.execute("""INSERT INTO hourly (hour, metric, n) VALUES
                  (strftime('%Y-%m-%d %H', 'now', '-8 days'), 'req', 5)""")
res = _stats.rollup_once()
ok(res["rolled"] > 0 and res["purged"] == 2, f"rollup + purge: {res}")
with _adb.conn() as cc:
    today = _stats.utcnow().strftime("%Y-%m-%d")
    dl = {r["metric"]: r["n"] for r in _adb.all_(
        cc, "SELECT metric, n FROM daily WHERE day=?", (today,))}
    leaked = _adb.one(cc, "SELECT COUNT(*) n FROM daily WHERE metric "
                          "IN ('user.active','diag.rssi','user.sent')")["n"]
ok(dl.get("msg.sent", 0) >= 1 and dl.get("active.users", 0) >= 1
   and dl.get("active.devices", 0) >= 1 and dl.get("users", 0) >= 3
   and "db.bytes" in dl, f"daily rollup rows: {sorted(dl)}")
ok(leaked == 0, "per-user/device dims never reach the daily table")
ok(_stats.presence_sweep() >= 1, "first sweep marks awake boxes online")
with _adb.conn() as cc:
    cc.execute("""UPDATE devices SET last_seen=datetime('now','-2 hours')
                  WHERE id='pip-grandma-01'""")
ok(_stats.presence_sweep() == 1, "quiet box flips to offline")
with _adb.conn() as cc:
    off = _adb.one(cc, "SELECT value FROM events WHERE kind='device.offline' "
                       "AND device_id='pip-grandma-01'")
    on = _adb.one(cc, "SELECT online FROM devices WHERE id='pip-grandma-01'")
ok(off and off["value"] >= 119 and on["online"] == 0,
   "offline event carries the quiet minutes")
ok(_stats.presence_sweep() == 0, "sweep is idempotent")
for q in ("", "?range=30d", "?range=all", "?range=bogus",
          "?device=pip-grandma-01", "?kind=msg.&user=3"):
    r = c.get("/admin/analytics" + q)
    ok(r.status_code == 200 and "pip-grandma-01" in r.text,
       f"analytics page renders {q or 'default'}")
r = c.get("/admin/analytics?kind=msg.")
ok("msg.sent" in r.text and "device.fw" not in r.text, "event log kind filter")
r = c.get("/admin/analytics?device=pip-grandma-01")
ok("device.boot" in r.text and "msg.sent" not in r.text
   and "data-spark" in r.text, "device drill-down: its events + sparklines")
r = c.get("/admin/analytics?range=30d&device=pip-grandma-01")
ok("? -&gt; 9.9.9" in r.text, "device.fw detail renders")

# --- wrong-owner access is denied ---
r = c.get(f"/v1/messages/{mid}/audio", headers=H_E)
ok(r.status_code == 404, "cross-user message access denied")

# --- firmware OTA ---
r = c.post("/admin/firmware", data={"version": "0.2.0", "notes": "t",
                                    "activate": "1"},
           files={"file": ("fw.bin", io.BytesIO(b"FWDATA"))})
ok(r.status_code == 303, "firmware uploaded+activated")
r = c.get("/v1/firmware", headers=H_E)
ok(r.json()["version"] == "0.2.0" and r.json()["url"].endswith(
       "/v1/firmware/0.2.0.bin?board=amoled-1.8"),
   "manifest serves active version (board-tagged URL)")
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
# list vers are the CALLER's format: H_E is a 1.8 box (base master), a
# phone user gets the -phone master's hash - and each matches only its
# own rendition, so per-format master changes bust exactly their clients
tp = next(t for t in c.get("/v1/themes", headers=utok).json()
          if t["name"] == t0["name"])
ok(tp["ver"] != t0["ver"],
   "phone-user list ver differs from the 1.8 box's (per-format masters)")
r = c.get(f"/v1/themes/{t0['name']}/web.jpg?v={tp['ver']}", headers=utok)
ok("immutable" in r.headers.get("cache-control", ""),
   "phone ver matches web.jpg immutable")
r = c.get(f"/v1/themes/{t0['name']}/web.jpg?v={t0['ver']}", headers=utok)
ok(r.headers.get("cache-control") == "no-cache",
   "a 1.8 ver on web.jpg must not cache immutable")
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
ok(anon.get("/admin/analytics").status_code == 303,
   "analytics requires admin login")
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

# --- browser playback: compressed m4a, legacy WAV, delivered lifecycle ---
r = web.get(f"/v1/messages/{wmid}/audio.m4a", headers=H_G)
ok(r.status_code == 200 and r.content[4:8] == b"ftyp", "audio.m4a is an MP4")
m4a_len = len(r.content)
r = web.get(f"/v1/messages/{wmid}/audio.wav", headers=H_G)
ok(r.status_code == 200 and r.content[:4] == b"RIFF", "audio.wav decodes to WAV")
ok(m4a_len * 3 < len(r.content),
   f"m4a is far smaller than the wav ({m4a_len} vs {len(r.content)})")
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
ok(r.status_code == 400, "push endpoint outside the known services is refused")
sub["endpoint"] = "https://fcm.googleapis.com/fcm/send/sub1"
r = pweb.post("/v1/push/subscribe", json=sub)
ok(r.status_code == 200, "push subscribe stored")
# unsubscribe is scoped to the owner: grandma's session cannot drop it
r = c.post("/v1/push/unsubscribe", headers=utok, json={"endpoint": sub["endpoint"]})
with adb.conn() as cc:
    _n = adb.one(cc, "SELECT COUNT(*) n FROM push_subs")["n"]
ok(r.status_code == 200 and _n == 1, "another user's unsubscribe does not drop it")

pushes = []
real_webpush = pywebpush.webpush
pywebpush.webpush = lambda *a, **k: pushes.append(a[1])  # (sub_info, json)
r = web.post("/v1/messages", headers=H_G,
             data={"recipient_id": "webby", "duration": "1"},
             files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
ok(r.status_code == 200 and len(pushes) == 1,
   "notify ladder: phone user got web push (no MQTT)")
wmid2 = r.json()["id"]

# a push is a promise the message is fetchable: the compressed file the
# service worker prefetches must exist before the push goes out.
pushes.clear()
r = web.post("/v1/messages", headers=H_G,
             data={"recipient_id": "webby", "duration": "1"},
             files={"audio": ("m.audio", io.BytesIO(wav))})
seq_id = r.json()["id"]
ok(len(pushes) == 1 and _os.path.exists(adb.playback_path(seq_id)),
   "playback file rendered before the push fires")
# the home-screen badge count rides along: the service worker has no other
# way to know it while the app is closed. webby has wmid2 + seq_id unheard.
ok(_json.loads(pushes[0])["unread"] == 2,
   "push carries the unread count for the app badge")
pweb.delete(f"/v1/messages/{seq_id}")          # webby is the recipient
ok(not _os.path.exists(adb.playback_path(seq_id)),
   "delete removes the playback rendering too")

def dead(*a, **k):
    raise pywebpush.WebPushException(
        "gone", response=types.SimpleNamespace(status_code=410))
pywebpush.webpush = dead
r = web.post("/v1/messages", headers=H_G,
             data={"recipient_id": "webby", "duration": "1"},
             files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
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
              files={"audio": ("m.vmsg", io.BytesIO(vmsg))})
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
# (subset check, not equality: the endpoint grew a "waitlist" key and a
# strict compare silently killed every assertion after this one)
r = c.get("/v1/auth/methods")
m = r.json()
ok(r.status_code == 200 and m["code"] is False and m["password"] is True
   and m["waitlist"] is False,
   "auth methods: password on, code off (no SMTP), no waitlist (self-host)")
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
