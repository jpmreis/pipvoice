/* Pip service worker: web push + a light network-first shell cache.
   Push is the real job here; offline support is incidental. */
"use strict";
const CACHE = "pip-v5";
const SHELL = ["./", "style.css", "app.js", "manifest.json", "install.html",
  "setup.html", "setup.js", "vendor/esptool-js.js",
  "fonts/montserrat-400.woff2", "fonts/montserrat-600.woff2",
  "fonts/montserrat-700.woff2", "icons/icon-192.png"];

/* Message audio, prefetched on push and read back by app.js. Its own cache
   so a shell version bump doesn't throw away a message that arrived but
   hasn't been played yet. Bounded: these are the only entries here that
   grow without limit. */
const AUDIO_CACHE = "pip-audio-v1";
const AUDIO_KEEP = 12;
const audioPath = (id) => `/v1/messages/${id}/audio.m4a`;

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener("activate", (e) => {
  e.waitUntil((async () => {
    for (const k of await caches.keys())
      if (k !== CACHE && k !== AUDIO_CACHE) await caches.delete(k);
    await self.clients.claim();
  })());
});

self.addEventListener("fetch", (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== "GET") return;
  // Versioned theme images are immutable by contract (?v= is a content
  // hash): serve cache-first so backgrounds/thumbs never re-download on
  // refresh - the browser HTTP cache alone isn't enough on iOS, which
  // evicts it aggressively. A new version is a new URL; the old entry
  // for that path is pruned on write.
  if (url.pathname.startsWith("/v1/themes/") && url.searchParams.has("v")) {
    e.respondWith((async () => {
      const c = await caches.open(CACHE);
      const hit = await c.match(e.request);
      if (hit) return hit;
      const fresh = await fetch(e.request);
      if (fresh.ok) {
        for (const k of await c.keys())
          if (new URL(k.url).pathname === url.pathname) await c.delete(k);
        c.put(e.request, fresh.clone());
      }
      return fresh;
    })());
    return;
  }
  if (url.pathname.startsWith("/v1/")) return;
  e.respondWith((async () => {
    try {
      const fresh = await fetch(e.request);
      const c = await caches.open(CACHE);
      c.put(e.request, fresh.clone());
      return fresh;
    } catch (err) {
      const hit = await caches.match(e.request);
      if (hit) return hit;
      throw err;
    }
  })());
});

async function trimAudioCache(c) {
  const keys = await c.keys();          // insertion order: oldest first
  for (let i = 0; i < keys.length - AUDIO_KEEP; i++) await c.delete(keys[i]);
}

/* Pull the message down while the phone is still in a pocket.

   A push only says a message exists. Without this nothing is downloaded
   until the tap, so on weak wifi the banner genuinely does arrive well
   ahead of the message — the notification and the audio were racing, and
   the audio started last. The server renders the .m4a before it pushes
   (notify.py), so by the time we get here the file is guaranteed to be
   sitting there ready. */
async function prefetchMessage(id) {
  if (!id || !self.caches) return;
  const url = audioPath(id);
  const c = await caches.open(AUDIO_CACHE);
  if (await c.match(url)) return;
  const r = await fetch(url, { credentials: "same-origin" });
  if (!r.ok) return;      // signed out, or the audio aged out: not fatal
  await c.put(url, r);
  await trimAudioCache(c);
}

/* Home-screen badge, from the count the push carries (notify.py). This is
   the case that actually needs a badge: with the app open, app.js keeps it
   honest itself. Reaction pushes send no count and leave it alone - the
   badge counts unheard messages, and a reaction isn't one. */
function setAppBadge(n) {
  if (typeof n !== "number" || n < 0) return;
  try {
    const p = n > 0 ? navigator.setAppBadge?.(n) : navigator.clearAppBadge?.();
    p?.catch(() => {});
  } catch (err) {}
}

self.addEventListener("push", (e) => {
  let data = {};
  try { data = e.data.json(); } catch (err) {}
  e.waitUntil((async () => {
    // Banner first, always. A push event gets a limited budget, and if it
    // ends without a notification the browser posts its own "site updated
    // in the background" notice instead — a slow prefetch must never cost
    // us the banner it is trying to make useful.
    await self.registration.showNotification(data.title || "Pip", {
      body: data.body || "New voice message",
      icon: "icons/icon-192.png",
      tag: data.msg_id || "pip",
      data: { msg_id: data.msg_id || "" },
    });
    setAppBadge(data.unread);
    try { await prefetchMessage(data.msg_id); } catch (err) {}
    // an app already open shouldn't wait out its 60 s poll to show the row
    for (const cl of await self.clients.matchAll(
                       { type: "window", includeUncontrolled: true }))
      cl.postMessage({ type: "new-message", msg_id: data.msg_id || "" });
  })());
});

self.addEventListener("notificationclick", (e) => {
  e.notification.close();
  e.waitUntil((async () => {
    const wins = await self.clients.matchAll({ type: "window",
                                              includeUncontrolled: true });
    for (const w of wins) {
      if (w.url.includes("/app")) { await w.focus(); return; }
    }
    await self.clients.openWindow("/app/");
  })());
});
