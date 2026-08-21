/* Pip service worker: web push + a light network-first shell cache.
   Push is the real job here; offline support is incidental. */
"use strict";
const CACHE = "pip-v4";
const SHELL = ["./", "style.css", "app.js", "manifest.json", "install.html",
  "setup.html", "setup.js", "vendor/esptool-js.js",
  "fonts/montserrat-400.woff2", "fonts/montserrat-600.woff2",
  "fonts/montserrat-700.woff2", "icons/icon-192.png"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener("activate", (e) => {
  e.waitUntil((async () => {
    for (const k of await caches.keys())
      if (k !== CACHE) await caches.delete(k);
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

self.addEventListener("push", (e) => {
  let data = {};
  try { data = e.data.json(); } catch (err) {}
  e.waitUntil(self.registration.showNotification(data.title || "Pip", {
    body: data.body || "New voice message",
    icon: "icons/icon-192.png",
    tag: data.msg_id || "pip",
    data: { msg_id: data.msg_id || "" },
  }));
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
