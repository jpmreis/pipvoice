/* Pip PWA — vanilla JS, mirrors the device UX (ui_*.c) where it makes sense. */
"use strict";
const $ = (id) => document.getElementById(id);

const MAX_S = 90;                       // mirrors device max_message_s
// reaction keys mirror the server's REACTIONS set; glyphs are UI-only
const REACT_LABEL = { heart: "❤️", up: "👍", down: "👎", haha: "Ha Ha",
                      bang: "!!", quest: "?", joy: "😂" };
const REACT_KEYS = Object.keys(REACT_LABEL);
const REACT_TEXT = new Set(["haha", "bang", "quest"]);   // text chips, not emoji
const standalone =
  matchMedia("(display-mode: standalone)").matches ||
  navigator.standalone === true;
let me = null, contacts = [], inbox = [], reactions = [];
let currentContact = null;

/* ---------------- tiny plumbing ---------------- */
let curScreen = "";
function show(id) {
  curScreen = id;
  document.querySelectorAll("section").forEach(s =>
    s.classList.toggle("on", s.id === id));
  applyBg();
  window.scrollTo(0, 0);
}

/* ---------------- background theme ---------------- */
// ?v=<content hash> lets the server mark theme images immutable, so the
// browser stops re-fetching them; a changed master flips the hash → new URL
const themeUrl = (name, ver) =>
  `/v1/themes/${name}/web.jpg` + (ver ? `?v=${ver}` : "");

/* The last-applied theme is remembered in localStorage and painted
   synchronously at boot, BEFORE /v1/me answers - otherwise every cold
   start shows a black home screen until the network round-trip finishes
   and the background "pops in", even though the image bytes were cached
   all along. /v1/me later reconciles if the theme changed elsewhere. */
const THEME_LS = "pip-theme";
let bgTheme = null;              // {name, fg, ver} currently painted

function paintBg(name, fg, ver) {
  bgTheme = name ? { name, fg, ver } : null;
  if (name) {
    const url = themeUrl(name, ver);
    $("bg").style.backgroundImage = `url(${url})`;
    new Image().src = url;       // warm fetch+decode even while bg is hidden
  }
  document.body.classList.toggle("fg-black", !!name && fg === "black");
  applyBg();
}

function applyTheme() {
  paintBg(me && me.theme, me && me.theme_fg, me && me.theme_ver);
  try {
    if (me && me.theme)
      localStorage.setItem(THEME_LS, JSON.stringify(
        { name: me.theme, fg: me.theme_fg, ver: me.theme_ver }));
    else if (me)
      localStorage.removeItem(THEME_LS);
  } catch (e) {}
}

function applyBg() {
  // bg only behind home/record (mirrors the device; settings etc. stay black)
  const on = bgTheme &&
    (curScreen === "scr-home" || curScreen === "scr-record");
  document.body.classList.toggle("themed", !!on);
  $("bg").style.display = on ? "block" : "none";
}

try {   // paint the remembered theme immediately, no network needed
  const t = JSON.parse(localStorage.getItem(THEME_LS) || "null");
  if (t) paintBg(t.name, t.fg, t.ver);
} catch (e) {}

/* ---------------- self-update ---------------- */
/* Global version, shared with the firmware: "<fw version>+web.<hash>" from
   /v1/version — a change on EITHER half means the fleet moved, and the PWA
   drops SW caches and hard-reloads so installed apps don't run stale JS for
   days. Baseline comes from whichever answers first: the explicit check
   (boot / wake-to-visible / 60s poll) or the X-Pip-Version header stamped
   on every API response — the header is what makes an update land on the
   next tap instead of the next poll, our stand-in for the devices' MQTT
   firmware notify (browsers can't hold an MQTT subscription in the
   background; the visibility check IS the wake-up path). */
let pipVer = null, pipRefreshing = false;
function noteVersion(version) {
  if (!version) return;
  if (pipVer === null) { pipVer = version; $("set-version").textContent = "Pip " + version; return; }
  if (version !== pipVer) refreshApp();
}
async function refreshApp() {
  // never yank an in-progress or unsent recording; the next check retries
  if (pipRefreshing || curScreen === "scr-record") return;
  pipRefreshing = true;
  try {
    const reg = await navigator.serviceWorker?.getRegistration();
    if (reg) await reg.update();
    // drop the cached shell but KEEP theme images and prefetched message
    // audio: both are immutable at their URL (themes are content-hashed,
    // a message id names one recording forever), so an update never stales
    // them - wiping them would force a re-download after every release,
    // and for audio that means losing a prefetch a message is waiting on
    if (window.caches)
      for (const k of await caches.keys()) {
        if (k === AUDIO_CACHE) continue;
        const c = await caches.open(k);
        for (const req of await c.keys())
          if (!new URL(req.url).pathname.startsWith("/v1/themes/"))
            await c.delete(req);
      }
  } catch (e) {}
  location.reload();
}
async function checkVersion() {
  try {
    const r = await fetch("/v1/version");
    if (!r.ok) return;
    noteVersion((await r.json()).version);
  } catch (e) {}
}

let themeList = null;
async function loadThemes() {
  if (!themeList) themeList = await api("/themes");
  const el = $("themes");
  el.innerHTML = "";
  const add = (name, label, fg, ver) => {
    const b = document.createElement("button");
    b.className = "theme-opt" + ((me.theme || null) === name ? " sel" : "");
    b.title = label;
    if (name) b.style.backgroundImage = `url(${themeUrl(name, ver)})`;
    else b.textContent = "None";
    b.onclick = async () => {
      try {
        await api("/theme", {
          method: "POST", headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name }),
        });
        me.theme = name; me.theme_fg = fg; me.theme_ver = ver || null;
        applyTheme();
        loadThemes();                    // refresh the selection ring
      } catch (e) { toast("Could not save", "danger"); }
    };
    el.appendChild(b);
  };
  add(null, "None", null, null);
  for (const t of themeList) add(t.name, t.label, t.fg, t.ver);
}

let toastTimer = null;
function toast(msg, kind = "") {
  const t = $("toast");
  t.textContent = msg;
  t.className = "show " + kind;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.className = "", 2500);
}

async function api(path, opts = {}) {
  const r = await fetch("/v1" + path, opts);
  noteVersion(r.headers.get("X-Pip-Version"));
  if (r.status === 401) { showLogin(); throw new Error("auth"); }
  if (!r.ok) {
    const err = new Error((await r.text()).slice(0, 200));
    err.status = r.status;
    throw err;
  }
  const ct = r.headers.get("content-type") || "";
  return ct.includes("json") ? r.json() : r;
}

/* ---------------- brand sounds ---------------- */
/* Same motif and timbre as the device chime (audio.c chime_note): two
   struck-tine notes a fifth apart, the second landing while the first
   still rings. Sent rises, received falls. Synthesized so device and
   PWA stay identical with no audio assets. */
let sndCtx = null;
function sndUnlock() {
  if (!sndCtx && window.AudioContext) sndCtx = new AudioContext();
  if (sndCtx && sndCtx.state === "suspended") sndCtx.resume().catch(() => {});
}
// iOS keeps the context suspended until a user gesture; any tap unlocks it
["click", "touchend"].forEach(evt =>
  document.addEventListener(evt, sndUnlock, { capture: true, passive: true }));

// partial: [freq multiple, level, decay time-constant s] - overtones die
// faster than the fundamental, which is what reads as "instrument"; the
// inharmonic 5.4x is the brief metallic glint of the strike
const PIP_PARTIALS = [
  [1.0, 1.0, 0.150],
  [2.0, 0.3, 0.090],
  [3.0, 0.12, 0.055],
  [5.4, 0.1, 0.025],
];
function pipStrike(t0, f) {
  for (const [mult, level, tau] of PIP_PARTIALS) {
    const o = sndCtx.createOscillator(), g = sndCtx.createGain();
    o.type = "sine";
    o.frequency.value = f * mult;
    g.gain.setValueAtTime(0, t0);
    g.gain.linearRampToValueAtTime(0.2 * level, t0 + 0.005);
    g.gain.setTargetAtTime(0, t0 + 0.005, tau);
    o.connect(g).connect(sndCtx.destination);
    o.start(t0);
    o.stop(t0 + 0.65);
  }
}
function playPips(freqs) {
  if (!sndCtx || sndCtx.state !== "running") return;
  const t = sndCtx.currentTime;
  pipStrike(t, freqs[0]);
  pipStrike(t + 0.15, freqs[1]); // second note lands while the first rings
}
const sndSent = () => playPips([880, 1320]);
const sndReceived = () => playPips([1320, 880]);

/* ---------------- boot ---------------- */
async function boot() {
  if ("serviceWorker" in navigator) {
    try { await navigator.serviceWorker.register("sw.js"); } catch (e) {}
  }
  checkVersion();                      // records the baseline version
  const splashMin = new Promise(res => setTimeout(res, 2100));
  try {
    me = await (await fetch("/v1/me")).json();
    if (me.username === undefined) me = null;
  } catch (e) { me = null; }
  if (me) { $("splash-hello").textContent = "Hello " + me.display_name; applyTheme(); }
  await splashMin;
  $("splash").classList.add("gone");
  if (!me) { showLogin(); return; }
  gate();
}

function gate() {
  // notification onboarding only makes sense installed: browsers can't
  // push for Pip until it's on the home screen (install lives at /install)
  if (standalone && "Notification" in window &&
      Notification.permission === "default") {
    show("scr-notify");
    return;
  }
  if ("Notification" in window && Notification.permission === "granted")
    syncPush();
  home();
}

/* ---------------- login (email code, or local password) ---------------- */
let loginEmail = "";
let authMethods = null;   // {code, password} from /v1/auth/methods
function showLogin() {
  $("login-step-email").style.display = "block";
  $("login-step-code").style.display = "none";
  $("login-err").textContent = "";
  show("scr-login");
  applyAuthMethods();
}
async function applyAuthMethods() {
  if (!authMethods) {
    try { authMethods = await (await fetch("/v1/auth/methods")).json(); }
    catch (e) { authMethods = { code: true, password: false }; }
  }
  const pw = !!authMethods.password;
  $("login-pass").style.display = pw ? "" : "none";
  $("login-pw-signin").style.display = pw ? "" : "none";
  // password first on self-host installs; the code button turns secondary
  // (and hides entirely when the server has no SMTP to send codes with)
  $("login-send").style.display = authMethods.code ? "" : "none";
  $("login-send").className = pw ? "btn ghost small" : "btn accent";
  // self-hosted servers have no public signup: the admin adds users by hand
  $("login-waitlist").style.display = authMethods.waitlist ? "flex" : "none";
}
async function passwordSignIn() {
  $("login-err").textContent = "";
  const email = $("login-email").value.trim().toLowerCase();
  const pass = $("login-pass").value;
  if (!email.includes("@") || !pass) {
    $("login-err").textContent = "Type your email and password";
    return;
  }
  const body = new FormData();
  body.append("email", email);
  body.append("password", pass);
  try {
    const r = await fetch("/v1/auth/login-password", { method: "POST", body });
    if (!r.ok) {
      $("login-err").textContent =
        r.status === 429 ? "Too many attempts - wait 15 minutes"
                         : "Wrong email or password";
      return;
    }
    me = await r.json();
    $("login-pass").value = "";
    applyTheme();
    gate();
  } catch (e) { $("login-err").textContent = "Network error - try again"; }
}
$("login-send").onclick = requestCode;
$("login-resend").onclick = requestCode;
$("login-pw-signin").onclick = passwordSignIn;
$("login-pass").addEventListener("keydown", e => {
  if (e.key === "Enter") passwordSignIn();
});
$("login-email").addEventListener("keydown", e => {
  if (e.key === "Enter") {
    if (authMethods && authMethods.password) $("login-pass").focus();
    else requestCode();
  }
});
$("login-verify").onclick = verifyCode;
$("login-code").addEventListener("keydown", e => {
  if (e.key === "Enter") verifyCode();
});
async function requestCode() {
  $("login-err").textContent = "";
  loginEmail = $("login-email").value.trim().toLowerCase();
  if (!loginEmail.includes("@")) {
    $("login-err").textContent = "Type your email address";
    return;
  }
  const body = new FormData();
  body.append("email", loginEmail);
  try {
    const r = await fetch("/v1/auth/request-code", { method: "POST", body });
    if (!r.ok) {
      $("login-err").textContent =
        r.status === 429 ? "Too many attempts - wait 15 minutes"
                         : "Something went wrong - try again";
      return;
    }
    $("login-step-email").style.display = "none";
    $("login-step-code").style.display = "block";
    $("login-sent-note").textContent =
      `If ${loginEmail} is in the family, a code is on its way`;
    $("login-code").value = "";
    $("login-code").focus();
  } catch (e) { $("login-err").textContent = "Network error - try again"; }
}
async function verifyCode() {
  $("login-err").textContent = "";
  const code = $("login-code").value.trim();
  if (code.length !== 6) {
    $("login-err").textContent = "The code has 6 digits";
    return;
  }
  const body = new FormData();
  body.append("email", loginEmail);
  body.append("code", code);
  try {
    const r = await fetch("/v1/auth/verify-code", { method: "POST", body });
    if (!r.ok) {
      $("login-err").textContent =
        r.status === 429 ? "Too many attempts - wait 15 minutes"
                         : "Wrong or expired code";
      return;
    }
    me = await r.json();
    $("login-code").value = "";
    applyTheme();
    gate();
  } catch (e) { $("login-err").textContent = "Network error - try again"; }
}

/* ---------------- notification onboarding ---------------- */
$("notify-btn").onclick = async () => {
  const p = await Notification.requestPermission();
  if (p === "granted") { await syncPush(); toast("Notifications on", "ok"); home(); }
  else $("notify-denied").style.display = "block";
};
$("notify-skip").onclick = home;

async function syncPush() {
  try {
    const reg = await navigator.serviceWorker.ready;
    let sub = await reg.pushManager.getSubscription();
    if (!sub) {
      const { key } = await api("/push/key");
      sub = await reg.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: urlB64ToU8(key),
      });
    }
    await api("/push/subscribe", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(sub.toJSON()),
    });
  } catch (e) { console.warn("push sync failed", e); }
}

function urlB64ToU8(s) {
  const pad = "=".repeat((4 - (s.length % 4)) % 4);
  const raw = atob((s + pad).replace(/-/g, "+").replace(/_/g, "/"));
  return Uint8Array.from(raw, c => c.charCodeAt(0));
}

/* ---------------- home ---------------- */
async function home() {
  presenceUI(false);
  show("scr-home");
  $("hello").textContent = "Hello " + (me ? me.display_name : "");
  await Promise.all([loadContacts(), loadInbox(), loadReactions()]);
}

async function loadContacts() {
  contacts = await api("/contacts");
  const el = $("contacts");
  el.innerHTML = "";
  for (const c of contacts) {
    const b = document.createElement("button");
    b.className = "contact";
    b.dataset.id = c.device_id;
    const initial = (c.name || "?").charAt(0).toUpperCase();
    b.innerHTML = `<span class="avatar" style="background:#${esc(c.color)}">${esc(initial)}</span>
                   <span class="cname">${esc(c.name)}</span>`;
    b.onclick = () => openRecord(c);
    el.appendChild(b);
  }
  renderReactionChips();
}

/* ---------------- reactions (sender side: chips on contact tiles) ---------------- */
async function loadReactions() {
  try { reactions = await api("/reactions"); } catch (e) { return; }
  renderReactionChips();
}

function renderReactionChips() {
  document.querySelectorAll(".contact").forEach(b => {
    // newest-first from the server, so find() = latest per contact
    const r = reactions.find(x => x.from === b.dataset.id);
    let chip = b.querySelector(".rchip");
    if (!r) { if (chip) chip.remove(); return; }
    if (!chip) {
      chip = document.createElement("span");
      chip.className = "rchip";
      b.querySelector(".avatar").appendChild(chip);
    }
    chip.textContent = REACT_LABEL[r.reaction] || "";
    chip.classList.toggle("txt", REACT_TEXT.has(r.reaction));
  });
}

function clearReactionsFrom(contactId) {
  if (!reactions.some(x => x.from === contactId)) return;
  api("/reactions/seen", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ from: contactId }),
  }).catch(() => {});
  reactions = reactions.filter(x => x.from !== contactId);
  renderReactionChips();
}

/* ---------------- reactions (recipient side: long-press a message) ---------------- */
function addLongPress(el, fn) {
  let t = null, x0 = 0, y0 = 0;
  const clear = () => { clearTimeout(t); t = null; };
  el.addEventListener("pointerdown", e => {
    x0 = e.clientX; y0 = e.clientY;
    clearTimeout(t);
    t = setTimeout(() => {
      t = null;
      if (navigator.vibrate) navigator.vibrate(10);
      // swallow the click that follows the release (self-removes if the
      // release happens off-element and no click ever fires)
      const eat = ev => { ev.stopPropagation(); ev.preventDefault(); };
      el.addEventListener("click", eat, { capture: true, once: true });
      setTimeout(() => el.removeEventListener("click", eat, { capture: true }), 600);
      fn();
    }, 500);
  });
  el.addEventListener("pointermove", e => {
    if (t && Math.hypot(e.clientX - x0, e.clientY - y0) > 12) clear();
  });
  el.addEventListener("pointerup", clear);
  el.addEventListener("pointercancel", clear);
  el.addEventListener("contextmenu", e => e.preventDefault());
}

let sheetCtx = null;
function openReactSheet(m, row) {
  sheetCtx = { m, row };
  const p = $("rsheet-panel");
  p.innerHTML = "";
  for (const k of REACT_KEYS) {
    const b = document.createElement("button");
    b.className = "ropt" + (REACT_TEXT.has(k) ? " txt" : "") +
                  (m.reaction === k ? " sel" : "");
    b.textContent = REACT_LABEL[k];
    b.onclick = () => pickReaction(k);
    p.appendChild(b);
  }
  $("rsheet").style.display = "flex";
}
$("rsheet").onclick = e => { if (e.target.id === "rsheet") closeReactSheet(); };
function closeReactSheet() { $("rsheet").style.display = "none"; sheetCtx = null; }

async function pickReaction(k) {
  const { m, row } = sheetCtx;
  const key = m.reaction === k ? "" : k;    // tapping the current one clears
  closeReactSheet();
  try {
    await api(`/messages/${m.id}/reaction`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ reaction: key }),
    });
    m.reaction = key;
    const b = row.querySelector(".react-badge");
    b.textContent = REACT_LABEL[key] || "";
    b.classList.toggle("txt", REACT_TEXT.has(key));
    b.style.display = key ? "" : "none";
  } catch (e) { toast("Could not react", "danger"); }
}

/* ---------------- unread count ----------------
   The pill on the inbox tab and, on an installed PWA, the badge on the
   home-screen icon. One function because three call sites each kept their
   own copy of this arithmetic and delete quietly forgot to run it. */
function refreshUnread() {
  const n = inbox.filter(m => !m.delivered).length;
  $("inbox-badge").style.display = n ? "inline-flex" : "none";
  $("inbox-badge").textContent = n;
  setAppBadge(n);
}

/* Home-screen badge. Wants an installed PWA and notification permission -
   on iOS the same pair web push already needs, so anyone getting Pip
   notifications can carry one. A no-op in a plain tab, and on Firefox.
   Nothing here is worth an error: a badge is decoration. */
function setAppBadge(n) {
  try {
    const p = n > 0 ? navigator.setAppBadge?.(n) : navigator.clearAppBadge?.();
    p?.catch(() => {});
  } catch (e) {}
}

let knownMsgIds = null;
async function loadInbox() {
  inbox = await api("/inbox");
  // ding only for messages that appeared since the last load, never on
  // the first load of a session (those aren't "arrivals")
  const fresh = knownMsgIds &&
    inbox.some(m => !knownMsgIds.has(m.id) && !m.delivered);
  knownMsgIds = new Set(inbox.map(m => m.id));
  if (fresh) sndReceived();
  const el = $("inbox");
  el.innerHTML = "";
  $("inbox-empty").style.display = inbox.length ? "none" : "block";
  refreshUnread();
  for (const m of inbox) {
    const row = document.createElement("div");
    row.className = "msg" + (m.delivered ? "" : " unheard");
    const senderInitial = (m.sender_name || "?").charAt(0).toUpperCase();
    row.innerHTML =
      `<span class="cdot" style="background:#${esc(m.sender_color)}">${esc(senderInitial)}</span>
       <span class="who"><span class="name">${esc(m.sender_name)}</span><br>
         <span class="meta">${esc(m.when)} &middot; ${fmtDur(m.duration)}</span></span>
       <span class="react-badge"></span>
       <button class="play">&#9654;&#xFE0E;</button>
       <button class="del">&#10005;</button>`;
    const badge = row.querySelector(".react-badge");
    badge.textContent = REACT_LABEL[m.reaction] || "";
    badge.classList.toggle("txt", REACT_TEXT.has(m.reaction));
    badge.style.display = m.reaction ? "" : "none";
    row.querySelector(".play").onclick = () => playMessage(m, row);
    row.querySelector(".del").onclick = () => delMessage(m, row);
    addLongPress(row, () => openReactSheet(m, row));
    el.appendChild(row);
  }
  // hold blobs only for what is still in the inbox, and get the newest
  // unheard ones ready before they are tapped
  const live = new Set(inbox.map(m => m.id));
  for (const id of [...audioUrls.keys()]) if (!live.has(id)) forgetAudio(id);
  for (const m of inbox.filter(m => !m.delivered).slice(0, AUDIO_WARM))
    warmAudio(m.id);
}

/* ---------------- message audio ----------------
   The service worker prefetches a message's audio into AUDIO_CACHE the
   moment its push lands (sw.js), so by the time the app is opened the
   bytes are usually already on the phone. We resolve them to object URLs
   *ahead* of the tap and keep them in a map, because play() has to be
   called synchronously inside the click handler — an await in between
   loses the user gesture and iOS refuses to start the audio.

   A miss is never fatal: the src falls back to the network URL, which is
   what every play did before this existed. */
const AUDIO_CACHE = "pip-audio-v1";      // must match sw.js
const AUDIO_WARM = 3;                    // newest unheard, the likely taps
const audioUrls = new Map();             // msg id -> object URL (or null)

function msgAudioPath(id) { return `/v1/messages/${id}/audio.m4a`; }

async function warmAudio(id) {
  if (audioUrls.has(id)) return;
  audioUrls.set(id, null);               // claim it: no duplicate fetches
  try {
    const url = msgAudioPath(id);
    let r = null;
    if (window.caches) {
      const c = await caches.open(AUDIO_CACHE);
      r = await c.match(url);
      if (!r) {
        r = await fetch(url);
        if (r.ok) await c.put(url, r.clone());
      }
    } else {
      r = await fetch(url);
    }
    if (r && r.ok) audioUrls.set(id, URL.createObjectURL(await r.blob()));
    else audioUrls.delete(id);           // retry on the next render
  } catch (e) { audioUrls.delete(id); }
}

function forgetAudio(id) {
  const u = audioUrls.get(id);
  if (u) URL.revokeObjectURL(u);
  audioUrls.delete(id);
  if (window.caches)
    caches.open(AUDIO_CACHE)
      .then(c => c.delete(msgAudioPath(id))).catch(() => {});
}

const player = new Audio();
let playingRow = null;
function stopPlayback() {
  player.pause();
  if (playingRow) playingRow.classList.remove("playing");
  playingRow = null;
}
player.addEventListener("ended", stopPlayback);

async function playMessage(m, row) {
  if (playingRow === row) { stopPlayback(); return; }
  stopPlayback();
  // set before the first await, so the tap still counts as a user gesture
  player.src = audioUrls.get(m.id) || msgAudioPath(m.id);
  try { await player.play(); } catch (e) { toast("Playback failed", "danger"); return; }
  row.classList.add("playing");
  playingRow = row;
  if (!m.delivered) {
    m.delivered = true;
    row.classList.remove("unheard");
    api(`/messages/${m.id}/ack`, { method: "POST" }).catch(() => {});
    refreshUnread();
  }
}

async function delMessage(m, row) {
  if (!confirm(`Delete the message from ${m.sender_name}?`)) return;
  if (playingRow === row) stopPlayback();
  await api(`/messages/${m.id}`, { method: "DELETE" });
  forgetAudio(m.id);
  row.remove();
  inbox = inbox.filter(x => x.id !== m.id);
  $("inbox-empty").style.display = inbox.length ? "none" : "block";
  refreshUnread();          // deleting an unheard message lowers the count
  toast("Deleted");
}

/* ---------------- record ---------------- */
let rec = null, recChunks = [], recStart = 0, recTimer = null;
let recBlob = null, recSecs = 0, recStream = null;

function pickMime() {
  for (const m of ["audio/mp4", "audio/webm;codecs=opus", "audio/webm"])
    if (window.MediaRecorder && MediaRecorder.isTypeSupported(m)) return m;
  return "";
}

function openRecord(c) {
  currentContact = c;
  stopPlayback();
  clearReactionsFrom(c.device_id);   // opening the contact marks its badge seen
  $("rec-title").textContent = "To " + c.name;
  resetRecord();
  show("scr-record");
  presenceUI(true);
}

/* ---------------- recording presence ---------------- */
let presenceKA = null, presencePoll = null;

function postPresence(state) {
  if (!currentContact) return;
  api("/presence", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ to: currentContact.device_id, state }),
  }).catch(() => {});
}

async function checkPeerPresence() {
  if (curScreen !== "scr-record" || !currentContact) return;
  try {
    const list = await api("/presence");
    const e = list.find(x => x.from === currentContact.device_id);
    if (e) $("peer-rec-name").textContent = e.from_name + " is recording…";
    $("peer-rec").style.display = e ? "flex" : "none";
  } catch (e) {}
}

function presenceUI(on) {
  clearInterval(presencePoll);
  presencePoll = null;
  $("peer-rec").style.display = "none";
  if (on) {
    checkPeerPresence();
    presencePoll = setInterval(checkPeerPresence, 4000);
  }
}

function resetRecord() {
  recBlob = null; recSecs = 0;
  $("rectimer").textContent = "0:00";
  $("rectimer").classList.remove("live");
  $("preview").style.display = "none";
  $("recbtn").style.display = "block";
  $("recbtn").textContent = "REC";
  $("rechint").textContent = "Tap to record";
}

$("rec-back").onclick = () => { cancelRecord(); releaseStream(); home(); };

const recbtn = $("recbtn");
recbtn.onclick = () => { rec ? stopRecord() : startRecord(); };
recbtn.addEventListener("contextmenu", e => e.preventDefault());

// The mic stream is kept alive between takes on this screen so re-records
// don't re-prompt; released when leaving (back/send) so the OS mic
// indicator clears. iOS still re-asks once per app launch - that's the
// platform, not us.
async function ensureStream() {
  if (recStream && recStream.getTracks().some(t => t.readyState === "live"))
    return recStream;
  recStream = await navigator.mediaDevices.getUserMedia({ audio: true });
  return recStream;
}
function releaseStream() {
  if (recStream) { recStream.getTracks().forEach(t => t.stop()); recStream = null; }
}

async function startRecord() {
  if (rec) return;
  try {
    await ensureStream();
  } catch (err) {
    toast("Microphone access is needed to record", "danger");
    return;
  }
  const mime = pickMime();
  rec = new MediaRecorder(recStream, mime ? { mimeType: mime } : {});
  recChunks = [];
  rec.ondataavailable = ev => { if (ev.data.size) recChunks.push(ev.data); };
  rec.start();
  postPresence("start");             // + keepalive: server TTL is 20 s
  clearInterval(presenceKA);
  presenceKA = setInterval(() => postPresence("start"), 10000);
  recStart = Date.now();
  recbtn.classList.add("rec");
  recbtn.textContent = "STOP";
  $("rechint").textContent = "Tap again when you're done";
  $("rectimer").classList.add("live");
  recTimer = setInterval(() => {
    const s = Math.floor((Date.now() - recStart) / 1000);
    $("rectimer").textContent = fmtClock(Math.min(s, MAX_S));
    if (s >= MAX_S) stopRecord();
  }, 200);
}

function stopRecord() {
  if (!rec) return;
  const r = rec; rec = null;
  clearInterval(recTimer);
  clearInterval(presenceKA);
  postPresence("stop");
  recbtn.classList.remove("rec");
  const elapsed = (Date.now() - recStart) / 1000;
  r.onstop = () => {
    if (elapsed < 1) { resetRecord(); toast("Too short - try again"); return; }
    recSecs = Math.min(Math.round(elapsed), MAX_S);
    recBlob = new Blob(recChunks, { type: r.mimeType || "audio/webm" });
    $("rectimer").classList.remove("live");
    $("rectimer").textContent = fmtClock(recSecs);
    $("recbtn").style.display = "none";
    $("rechint").textContent = "";
    $("preview").style.display = "block";
  };
  r.stop();
}

function cancelRecord() {
  if (rec) { const r = rec; rec = null; clearInterval(recTimer);
             r.onstop = () => {}; r.stop();
             clearInterval(presenceKA); postPresence("stop"); }
  recbtn.classList.remove("rec");
}

$("prev-play").onclick = () => {
  stopPlayback();
  player.src = URL.createObjectURL(recBlob);
  player.play();
};
$("prev-again").onclick = resetRecord;
$("prev-send").onclick = async () => {
  if (!recBlob) return;
  $("prev-send").disabled = true;
  const fd = new FormData();
  fd.append("recipient_id", currentContact.device_id);
  fd.append("duration", String(recSecs));
  fd.append("audio", recBlob, "m.audio");
  try {
    await api("/messages", { method: "POST", body: fd });
    sndSent();
    toast(`On its way to ${currentContact.name}`, "ok");
    releaseStream();
    home();
  } catch (e) {
    toast(e.status === 429
      ? `Slow down - ${currentContact.name} has 5 messages from you already`
      : "Send failed - try again", "danger");
  } finally { $("prev-send").disabled = false; }
};

/* ---------------- settings ---------------- */
$("btn-settings").onclick = () => {
  $("set-me").textContent = me ? `${me.display_name} (@${me.username})` : "";
  const p = ("Notification" in window) ? Notification.permission : "unsupported";
  $("set-notify-state").textContent =
    "Notifications: " + (standalone ? p : "install the app to enable");
  const managed = (me && me.managed) || [];
  $("set-manage").style.display = managed.length ? "block" : "none";
  $("set-manage").textContent =
    managed.length > 1 ? "Manage devices" : "Manage device";
  show("scr-settings");
};
$("set-back").onclick = home;
/* Guarded: a phone can briefly run this app.js against a stale cached
   index.html without these elements - an unguarded binding here throws
   and kills the whole script before boot() ever hides the splash. */
const _setBg = $("set-background"), _bgBack = $("bg-back");
if (_setBg) _setBg.onclick = () => {
  loadThemes().catch(() => {});
  show("scr-background");
};
if (_bgBack) _bgBack.onclick = () => show("scr-settings");
/* device setup + web flashing lives on its own page (needs Web Serial,
   i.e. a computer running Chrome/Edge - the page says so elsewhere) */
$("set-setup").onclick = () => { location.href = "setup.html"; };
$("set-test").onclick = async () => {
  if (!("Notification" in window) || Notification.permission !== "granted") {
    toast("Turn on notifications first (Settings shows how)", "danger");
    return;
  }
  await syncPush();          // self-heal: re-register before testing
  try {
    const { accepted } = await api("/push/test", { method: "POST" });
    toast(accepted ? "Test notification sent" : "No live subscription - is the app installed?",
          accepted ? "ok" : "danger");
  } catch (e) { toast("Test failed", "danger"); }
};
$("set-logout").onclick = async () => {
  try { await api("/auth/logout", { method: "POST" }); } catch (e) {}
  me = null;
  setAppBadge(0);      // a signed-out phone must not wear someone's count
  showLogin();
};

/* ---------------- device admin: manage a device's contacts ---------------- */
/* Only shown to users who administer a device (me.managed non-empty).
   Contacts are added by typing the exact @username - deliberately no
   picker, so the full user list is never exposed; the server 404s unknown
   handles and rate limits repeated misses. */
$("set-manage").onclick = openManage;
$("mng-back").onclick = () => show("scr-settings");

async function openManage() {
  show("scr-manage");
  const el = $("manage-list");
  el.innerHTML = `<p class="dim">Loading…</p>`;
  let list;
  try { list = await api("/managed"); }
  catch (e) { el.innerHTML = `<p class="dim">Could not load - try again.</p>`; return; }
  el.innerHTML = "";
  for (const d of list) el.appendChild(manageCard(d));
  if (!list.length) el.innerHTML = `<p class="dim">No devices assigned to you.</p>`;
}

function manageCard(d) {
  const card = document.createElement("div");
  card.className = "card";
  card.innerHTML =
    `<div style="font-weight:600">${esc(d.name)}</div>
     <div class="small dim">@${esc(d.username)} &middot; ${esc(d.device_id)}</div>
     <div class="small dim" style="font-weight:600;margin-top:14px">CAN EXCHANGE MESSAGES WITH</div>
     <div class="mng-contacts"></div>
     <p class="small dim mng-empty" style="display:none">Nobody yet - add an @username below.</p>
     <div class="mng-addrow">
       <input class="mng-input" placeholder="@username" autocapitalize="none"
              autocorrect="off" spellcheck="false">
       <button class="btn accent mng-add">Add</button>
     </div>
     <div class="small mng-err" style="color:var(--danger)"></div>
     <label class="small" style="display:flex;gap:8px;align-items:center;margin-top:14px">
       <input type="checkbox" class="mng-voice" style="flex:none" ${d.voice ? "checked" : ""}>
       <span style="flex:1;min-width:0">Voice control (accessibility)
         &mdash; hands-free "Hey Pip"</span>
     </label>
     <p class="small dim" style="margin:14px 0 0">
       <a href="setup.html?flash=${esc(d.device_id)}"
          style="color:inherit">Re-flash this box</a>
       &mdash; needs a computer with Chrome/Edge; gives it new
       credentials.</p>`;
  const wrap = card.querySelector(".mng-contacts");
  const empty = card.querySelector(".mng-empty");
  const err = card.querySelector(".mng-err");
  const input = card.querySelector(".mng-input");
  const addBtn = card.querySelector(".mng-add");
  const voiceBox = card.querySelector(".mng-voice");

  voiceBox.onchange = async () => {
    const on = voiceBox.checked;
    voiceBox.disabled = true;
    try {
      await api(`/managed/${d.device_id}/voice`, {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ on }),
      });
      d.voice = on;
      toast(on ? "Voice control on - the box listens for \"Hey Pip\""
               : "Voice control off", "ok");
    } catch (e) {
      voiceBox.checked = !on;          // revert: the server didn't take it
      toast("Could not change voice control - try again", "danger");
    }
    voiceBox.disabled = false;
  };

  const render = () => {
    wrap.innerHTML = "";
    empty.style.display = d.contacts.length ? "none" : "block";
    for (const ct of d.contacts) {
      const row = document.createElement("div");
      row.className = "msg";           // reuse inbox row styling
      const initial = (ct.name || "?").charAt(0).toUpperCase();
      row.innerHTML =
        `<span class="cdot" style="background:#${esc(ct.color)}">${esc(initial)}</span>
         <span class="who"><span class="name">${esc(ct.name)}</span><br>
           <span class="meta">@${esc(ct.username)}</span></span>
         <button class="del">&#10005;</button>`;
      row.querySelector(".del").onclick = async () => {
        if (!confirm(`Stop ${d.name} and ${ct.name} from exchanging messages?`)) return;
        try {
          await api(`/managed/${d.device_id}/contacts/${ct.username}`,
                    { method: "DELETE" });
          d.contacts = d.contacts.filter(x => x.username !== ct.username);
          render();
          toast(`${ct.name} removed`);
        } catch (e) { toast("Could not remove - try again", "danger"); }
      };
      wrap.appendChild(row);
    }
  };

  const add = async () => {
    err.textContent = "";
    const raw = input.value.trim();
    if (!raw) return;
    const shown = raw.startsWith("@") ? raw : "@" + raw;
    addBtn.disabled = true;
    try {
      const r = await api(`/managed/${d.device_id}/contacts`, {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: raw }),
      });
      input.value = "";
      if (r.existed) {
        toast(`${r.contact.name} is already on the list`);
      } else {
        d.contacts.push(r.contact);
        d.contacts.sort((a, b) => a.name.localeCompare(b.name));
        toast(`${r.contact.name} added`, "ok");
      }
      render();
    } catch (e) {
      err.textContent =
        e.status === 404 ? `No user called ${shown} - check the spelling` :
        e.status === 400 ? `${shown} is this device's own username` :
        e.status === 429 ? "Too many attempts - wait 15 minutes" :
        "Could not add - try again";
    } finally { addBtn.disabled = false; }
  };
  addBtn.onclick = add;
  input.addEventListener("keydown", e => { if (e.key === "Enter") add(); });

  render();
  return card;
}

/* ---------------- helpers / refresh ---------------- */
function esc(s) {
  return String(s).replace(/[&<>"']/g,
    ch => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch]));
}
function fmtDur(s) { return s >= 60 ? `${Math.floor(s / 60)}m${s % 60}s` : `${s}s`; }
function fmtClock(s) { return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; }

document.addEventListener("visibilitychange", () => {
  if (document.hidden) return;
  checkVersion();
  if (me && $("scr-home").classList.contains("on")) {
    loadInbox().catch(() => {});
    loadReactions();
  }
  if (me) syncPushIfGranted();
});

/* The service worker tells us the moment a push lands, so an app that is
   already open picks the message up now instead of on its next 60 s poll.
   loadInbox warms the audio for what it renders; when we're elsewhere in
   the app, warm it directly so it's ready if the user navigates home. */
navigator.serviceWorker?.addEventListener("message", (e) => {
  if (!e.data || e.data.type !== "new-message") return;
  if (me && $("scr-home").classList.contains("on")) {
    loadInbox().catch(() => {});
    loadReactions();
  } else if (e.data.msg_id) {
    warmAudio(e.data.msg_id);
  }
});
// messages to an addEventListener (rather than onmessage) listener stay
// queued until this is called - without it the first one can be lost
navigator.serviceWorker?.startMessages?.();
setInterval(() => {
  if (document.hidden) return;
  checkVersion();
  if (me && $("scr-home").classList.contains("on")) {
    loadInbox().catch(() => {});
    loadReactions();
  }
}, 60000);
function syncPushIfGranted() {
  // not gated on standalone: desktop browsers do web push in a plain tab
  if ("Notification" in window &&
      Notification.permission === "granted") syncPush();
}

boot();
