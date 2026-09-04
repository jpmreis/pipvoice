/* Pip setup: create a device user + flash the box over Web Serial.
 * Chrome/Edge desktop only. Talks to /v1/setup/* (see server api.py);
 * the flashing itself is esptool-js (vendor/esptool-js.js).
 *
 * Gates before flashing (mirrors the manifest's chip block):
 *   chip family ESP32-S3, PSRAM cap 1 (8 MB octal - S3R8), flash >= 16 MB.
 * Board-level hardware (screen, touch, codec, PMU) is only verifiable by
 * running the firmware: after flashing we reopen the same port and watch
 * the boot log for "boot complete" and the PIP-HW summary line. */
import { ESPLoader, Transport } from "./vendor/esptool-js.js";

const $ = (id) => document.getElementById(id);
const ESPRESSIF_USB_VID = 0x303a;
const DEFAULT_BOARD = "amoled-1.8";

let manifest = null;          // parts + chip gates, from create/rekey
let flashName = "";           // display name for the box being flashed
let flashStartMs = 0;         // last_seen newer than this = box is online
let port = null;
let boardCatalog = [];        // /v1/setup/boards; drives the model picker
let selectedBoard = DEFAULT_BOARD;

function boardName(key) {
  const b = boardCatalog.find((x) => x.board === key);
  return b ? b.name : "Waveshare ESP32-S3-Touch-AMOLED-1.8";
}

// board of the image being flashed: the manifest's, else the picker's
function boardHelp() {
  const key = (manifest && manifest.board) || selectedBoard;
  return `Pip needs the ${boardName(key)} board - ` +
         "this device doesn't match it.";
}

function state(id) {
  for (const s of ["st-unsupported", "st-login", "st-start", "st-flash",
                   "st-done"])
    $(s).style.display = s === id ? "block" : "none";
}

function step(id, cls) {          // cls: act | ok | err
  const el = $(id);
  el.classList.remove("act", "ok", "err", "dimmed");
  if (cls) el.classList.add(cls);
}

function detail(text) { $("fl-detail").textContent = text || ""; }

async function api(path, opts = {}) {
  const r = await fetch("/v1" + path, opts);
  if (!r.ok) {
    const err = new Error((await r.text()).slice(0, 300) || r.statusText);
    err.status = r.status;
    throw err;
  }
  const ct = r.headers.get("content-type") || "";
  return ct.includes("json") ? r.json() : r;
}

function apiJson(path, body) {
  return api(path, { method: "POST",
                     headers: { "Content-Type": "application/json" },
                     body: JSON.stringify(body) });
}

function friendly(e) {
  try { return JSON.parse(e.message).detail || e.message; }
  catch { return e.message; }
}

/* ---------------- entry ---------------- */
async function init() {
  if (!("serial" in navigator)) {
    $("uns-url").textContent = location.origin + location.pathname;
    state("st-unsupported");
    return;
  }
  try { await api("/me"); }
  catch (e) { state("st-login"); return; }
  state("st-start");
  if (!$("nu-server").value) $("nu-server").value = location.origin;
  loadBoards();               // model picker; optional, defaults to the 1.8
  /* ?flash=<device_id> deep link (Settings -> Manage device, admin
     Devices page): jump straight to re-flashing that box. Server admins
     can flash devices outside their /managed list, so an unknown id
     still gets a row - the server decides whether the rekey is allowed. */
  const target = /^[a-z0-9-]{1,31}$/.test(
    new URLSearchParams(location.search).get("flash") || "")
    ? new URLSearchParams(location.search).get("flash") : null;
  let managed = [];
  try { managed = await api("/managed"); }
  catch (e) { /* resume list is optional */ }
  if (target && !managed.some((d) => d.device_id === target))
    managed.unshift({ device_id: target, name: target, username: null });
  if (managed.length) {
    $("resume-card").style.display = "block";
    const list = $("resume-list");
    list.innerHTML = "";
    for (const d of managed) {
      const row = document.createElement("div");
      row.className = "devrow";
      row.innerHTML =
        `<div class="who"><b></b><br><span class="small dim"></span></div>
         <input class="pin" maxlength="4" inputmode="numeric"
                placeholder="new PIN" style="width:110px;margin:0">
         <button class="btn">Flash</button>`;
      row.querySelector("b").textContent = d.name;
      row.querySelector("span").textContent =
        (d.username ? "@" + d.username + " · " : "") + d.device_id;
      row.querySelector("button").onclick = () => rekey(d, row);
      list.appendChild(row);
      if (d.device_id === target) {
        row.style.outline = "2px solid var(--accent)";
        row.style.borderRadius = "8px";
        setTimeout(() => {
          row.scrollIntoView({ block: "center" });
          row.querySelector(".pin").focus();
        }, 0);
      }
    }
  }
}

/* The model picker. The server says which models have firmware; the
 * rest render greyed out as "coming soon". If the endpoint is missing
 * or fails (older server), the picker stays hidden and the default
 * board is used - exactly the pre-multi-model behavior. */
async function loadBoards() {
  try { boardCatalog = await api("/setup/boards"); }
  catch { return; }
  if (!Array.isArray(boardCatalog) || boardCatalog.length < 2) return;
  const grid = $("board-grid");
  grid.innerHTML = "";
  for (const b of boardCatalog) {
    const el = document.createElement("button");
    el.type = "button";
    el.className = "board-opt" + (b.available ? "" : " soon");
    el.innerHTML = `<img alt=""><div class="bl"></div><div class="bs"></div>`;
    el.querySelector("img").src = b.img;
    el.querySelector(".bl").textContent = b.label;
    el.querySelector(".bs").textContent = b.blurb;
    el.title = b.name;
    if (!b.available) {
      const badge = document.createElement("div");
      badge.className = "badge";
      badge.textContent = "coming soon";
      el.appendChild(badge);
    }
    el.onclick = () => {
      if (!b.available) return;
      selectedBoard = b.board;
      for (const o of grid.children) o.classList.remove("sel");
      el.classList.add("sel");
    };
    if (b.board === selectedBoard && b.available) el.classList.add("sel");
    grid.appendChild(el);
  }
  $("board-card").style.display = "block";
}

async function create() {
  $("nu-err").textContent = "";
  const name = $("nu-name").value.trim();
  const username = $("nu-user").value.trim();
  const pin = $("nu-pin").value.trim();
  if (!name || !username || !/^\d{4}$/.test(pin)) {
    $("nu-err").textContent =
      "Fill in a name, a username and a 4-digit Settings PIN.";
    return;
  }
  const server_url = $("nu-server").value.trim();
  $("nu-create").disabled = true;
  try {
    const r = await apiJson("/setup/device",
                            { name, username, pin, server_url,
                              board: selectedBoard });
    startFlash(name, r.device_id, r.manifest, "nu-err");
  } catch (e) {
    $("nu-err").textContent = friendly(e);
  } finally {
    $("nu-create").disabled = false;
  }
}

async function rekey(d, row) {
  $("re-err").textContent = "";
  const pin = row.querySelector(".pin").value.trim();
  if (!/^\d{4}$/.test(pin)) {
    $("re-err").textContent = "Enter a new 4-digit Settings PIN first.";
    return;
  }
  try {
    const r = await apiJson(`/setup/${d.device_id}/rekey`,
                            { pin, server_url: $("nu-server").value.trim() });
    startFlash(r.name || d.name, d.device_id, r.manifest, "re-err");
  } catch (e) {
    $("re-err").textContent = friendly(e);
  }
}

function startFlash(name, deviceId, mf, errBox) {
  if (mf.problem) {
    $(errBox).textContent = "Cannot flash yet: " + mf.problem;
    return;
  }
  manifest = mf;
  flashName = name;
  flashStartMs = Date.now();
  $("fl-name").textContent = name;
  $("fl-device").textContent = deviceId +
    (mf.board_label ? " · " + mf.board_label + " model" : "");
  for (const s of ["stp-connect", "stp-check", "stp-flash", "stp-verify"])
    step(s, s === "stp-connect" ? "act" : "dimmed");
  $("fl-connect").style.display = "block";
  $("fl-anyport").style.display = "none";
  $("fl-back").style.display = "none";
  $("fl-bar").style.display = "none";
  detail("");
  state("st-flash");
}

/* ---------------- connect / identify / flash ---------------- */
async function connect(filtered) {
  try {
    port = await navigator.serial.requestPort(filtered
      ? { filters: [{ usbVendorId: ESPRESSIF_USB_VID }] } : {});
  } catch (e) {
    // user cancelled, or nothing matched the filter: offer the full list
    $("fl-anyport").style.display = "block";
    return;
  }
  $("fl-connect").style.display = "none";
  $("fl-anyport").style.display = "none";
  await run();
}

function fail(stepId, msg) {
  step(stepId, "err");
  detail(msg);
  $("fl-back").style.display = "block";
}

async function run() {
  step("stp-connect", "ok");
  step("stp-check", "act");
  detail("Talking to the board…");

  const transport = new Transport(port, false);
  const esploader = new ESPLoader({
    transport, baudrate: 460800,
    terminal: { clean() {}, writeLine() {}, write() {} },
  });

  let desc;
  try {
    desc = await esploader.main();
  } catch (e) {
    try { await transport.disconnect(); } catch {}
    fail("stp-check", "No ESP32 responded on that port. Unplug the box, " +
         "plug it back in and try again (or hold the top button while " +
         "plugging in). If this isn't a Pip box: " + boardHelp());
    return;
  }

  try {
    const chipName = esploader.chip.CHIP_NAME;
    if (chipName !== manifest.chip.family) {
      fail("stp-check", `This is an ${chipName} - Pip needs an ` +
           `${manifest.chip.family}. ${boardHelp()}`);
      return;
    }
    const psram = await esploader.chip.getPsramCap(esploader);
    if (psram !== manifest.chip.psramCap) {
      fail("stp-check", "This chip doesn't have the 8 MB PSRAM Pip needs " +
           "(the firmware cannot boot on it). " + boardHelp());
      return;
    }
    const sizeStr = await esploader.detectFlashSize();   // e.g. "16MB"
    const mb = parseInt(sizeStr, 10) || 0;
    if (mb < manifest.chip.minFlashMB) {
      fail("stp-check", `This board has ${sizeStr} of flash - Pip needs ` +
           `${manifest.chip.minFlashMB} MB. ${boardHelp()}`);
      return;
    }
    step("stp-check", "ok");
    detail(`${desc}, ${sizeStr} flash – compatible.`);

    step("stp-flash", "act");
    detail("Downloading Pip " + manifest.version + "…");
    const parts = [];
    for (const p of manifest.parts) {
      const r = await fetch(p.url);
      if (!r.ok) throw new Error(`download failed: ${p.name} (${r.status})`);
      parts.push({ data: new Uint8Array(await r.arrayBuffer()),
                   address: p.offset });
    }
    const total = parts.reduce((n, p) => n + p.data.length, 0);
    const done = parts.map(() => 0);
    $("fl-bar").style.display = "block";
    detail("Installing… keep the box plugged in.");
    await esploader.writeFlash({
      fileArray: parts,
      flashMode: manifest.flash.mode,
      flashFreq: manifest.flash.freqMHz + "m",
      flashSize: manifest.flash.sizeMB + "MB",
      eraseAll: false,
      compress: true,
      reportProgress(i, written, totalI) {
        done[i] = totalI ? (written / totalI) * parts[i].data.length
                         : parts[i].data.length;
        const pct = done.reduce((a, b) => a + b, 0) / total * 100;
        $("fl-bar").firstElementChild.style.width = pct.toFixed(1) + "%";
      },
    });
  } catch (e) {
    fail("stp-flash", "Flashing failed: " + (e.message || e) +
         " – unplug the box, plug it back in and start over.");
    try { await transport.disconnect(); } catch {}
    return;
  }
  $("fl-bar").style.display = "none";
  step("stp-flash", "ok");

  await verify(transport);
}

/* ---------------- post-flash reset + verification ---------------- */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function reattach(dbg) {
  // The chip reset killed the USB device (USB-Serial-JTAG lives inside
  // the chip), so the port re-enumerates. Wait out the dying enumeration
  // first - reopening the corpse "succeeds" and then delivers nothing.
  await sleep(1500);
  const t0 = Date.now();
  for (let i = 0; i < 15; i++) {
    try {
      await port.open({ baudRate: 115200 });
      dbg.push(`reopened original port at ${((Date.now() - t0) / 1000).toFixed(1)}s`);
      return port;
    } catch (e) {
      if (String(e).includes("already open")) {
        dbg.push("original port still open");
        return port;
      }
      if (i === 0) dbg.push("first open failed: " + (e.name || e));
    }
    try {
      for (const cand of await navigator.serial.getPorts()) {
        if (cand === port) continue;
        try {
          await cand.open({ baudRate: 115200 });
          dbg.push(`opened re-enumerated port at ${((Date.now() - t0) / 1000).toFixed(1)}s`);
          return cand;
        } catch {}
      }
    } catch {}
    await sleep(700);
  }
  dbg.push("no port would open in 12s");
  return null;
}

/* Read boot output from an opened port. We attach 1-3s into the boot, so
 * the earliest lines may be gone. Full verdict: "boot complete" / the
 * PIP-HW line. Fallback: any app-level log tag proves the firmware is
 * up. Repeated ROM reset banners with no app output = boot loop. A port
 * that delivers zero bytes for 6s is a dead handle - give up on it. */
async function watchBoot(p, budgetMs, dbg) {
  const decoder = new TextDecoder();
  const reader = p.readable.getReader();
  const t0 = Date.now();
  let buf = "", bytes = 0, started = false, complete = false;
  let hw = null, resets = 0;
  let pending = null;   // a timed-out read stays pending; reuse, don't drop
  const APP_TAG = /[IWE] \(\d+\) (pip|wifi|provisioning|config|sync|audio|board|main_task)/;
  try {
    while (Date.now() - t0 < budgetMs) {
      if (!bytes && Date.now() - t0 > 6000) { dbg.push("port silent for 6s"); break; }
      pending = pending || reader.read();
      const race = await Promise.race([
        pending,
        new Promise((r) => setTimeout(() => r("timeout"), 1000)),
      ]);
      if (race === "timeout") {
        if (hw || (complete && Date.now() - t0 > 8000)) break;
        continue;
      }
      pending = null;
      if (race.done) { dbg.push("stream ended"); break; }
      bytes += race.value.length;
      buf += decoder.decode(race.value, { stream: true });
      if (buf.length > 65536) buf = buf.slice(-32768);
      resets = (buf.match(/rst:0x/g) || []).length;
      if (!started && (buf.includes("Pip starting") || APP_TAG.test(buf))) {
        started = true;
        detail("Pip firmware is running…");
      }
      if (buf.includes("boot complete")) complete = true;
      const m = buf.match(/PIP-HW ([^\r\n]+)/);
      if (m) { hw = m[1]; break; }
      if (resets >= 3 && !started) break;   // boot loop
    }
  } catch (e) { dbg.push("read error: " + (e.name || e)); }
  try { reader.releaseLock(); await p.close(); } catch {}
  dbg.push(`${bytes} bytes read`);
  return { bytes, started, complete, hw, resets };
}

async function verify(transport) {
  step("stp-verify", "act");
  detail("Restarting… watching the box start up.");
  const dbg = [];

  // esptool-js's after("hard_reset") only DEasserts RTS - no edge on
  // USB-Serial-JTAG, the chip stays parked in the flasher stub and the
  // box looks frozen until a manual power cycle. Pulse EN ourselves.
  try {
    await transport.setRTS(true);          // EN low: chip in reset
    await sleep(100);
    await transport.setRTS(false);         // EN high: normal boot
  } catch (e) {
    softDone("The box was flashed but we couldn't restart it over USB - " +
             "unplug it and power it back on. It should show the Pip " +
             "hello and then a QR code.");
    try { await transport.disconnect(); } catch {}
    return;
  }
  try { await transport.disconnect(); } catch {}

  // a dead handle can open-then-stay-silent: allow one rescan pass
  let res = null;
  for (let attempt = 0; attempt < 2; attempt++) {
    const p = await reattach(dbg);
    if (!p) break;
    res = await watchBoot(p, attempt === 0 ? 45000 : 30000, dbg);
    if (res.bytes) break;
  }
  const { started = false, complete = false, hw = null, resets = 0 } =
    res || {};

  if (!res || !res.bytes) {
    softDone("The box restarted, but we couldn't listen to it over USB. " +
             "If its screen shows the Pip hello and then a QR code, " +
             "everything worked. [debug: " + dbg.join("; ") + "]");
    return;
  }
  if (started && !complete && !hw) {
    // attached too late for the summary, but the app is clearly up
    softDone("The box restarted and Pip is running. Its screen should " +
             "now show a QR code.");
    return;
  }
  if (complete || hw) {
    /* PIP-HW is k=v pairs; `board=` (firmware >= 1.3.6) is the model the
     * image was compiled for, everything else is a peripheral verdict.
     * Peripheral failures + a model mismatch = the wrong image is on the
     * box (the classic cross-flash: the hardware "works" but its touch/
     * codec drivers are for another model's chips). */
    const problems = [];
    let imgBoard = null;
    for (const kv of (hw || "").trim().split(/\s+/)) {
      const [k, v] = kv.split("=");
      if (k === "board") { imgBoard = v; continue; }
      if (v && v !== "ok") problems.push(k);
    }
    if (imgBoard && manifest.board && imgBoard !== manifest.board) {
      // server bookkeeping error: the artifact itself is for another model
      fail("stp-verify", `The installed firmware is built for the ` +
           `${boardName(imgBoard)} but this box is registered as a ` +
           `${boardName(manifest.board)}. Start over and flash again - ` +
           "if it repeats, ask for help.");
      return;
    }
    if (problems.length && imgBoard) {
      fail("stp-verify", "The firmware started but its " +
           problems.join(", ") + " did not respond. This usually means " +
           `the box is not a ${boardName(imgBoard)} (the model that was ` +
           "picked) - check the model, then start over to flash it " +
           "with the right firmware.");
      return;
    }
    const notes = problems.length
      ? "The box started, but reported problems with: " +
        problems.join(", ") + ". It may still work - if not, ask for help."
      : "The box hardware checked out.";
    step("stp-verify", problems.length ? "err" : "ok");
    finish(notes);
  } else if (resets >= 3) {
    fail("stp-verify", "The chip is fine, but the firmware couldn't start " +
         "- most likely this box isn't the model you picked (its screen " +
         "never initialized). " + boardHelp() + " Check the model and " +
         "start over to flash the right firmware.");
  } else {
    softDone("We couldn't confirm the startup over USB. If the box's " +
             "screen shows the Pip hello and then a QR code, everything " +
             "worked. [debug: " + dbg.join("; ") + "]");
  }
}

function softDone(msg) {
  step("stp-verify", "ok");
  finish(msg);
}

function finish(notes) {
  $("dn-notes").textContent = notes;
  state("st-done");
  pollOnline(manifest.device_id);
}

/* USB can't see past WiFi onboarding - the server can. devices.last_seen
 * bumps on every device-token request, so a value newer than the flash
 * start means the box booted, onboarded AND authenticated. */
async function pollOnline(deviceId) {
  const el = $("dn-online");
  for (let i = 0; i < 120; i++) {          // up to 10 min
    try {
      // per-device endpoint: /managed misses admin-flashed boxes
      const d = await api(`/setup/${deviceId}/online`);
      const ls = d.last_seen
        ? Date.parse(d.last_seen.replace(" ", "T") + "Z") : 0;
      if (ls > flashStartMs) {
        el.textContent = "The box is online and connected to Pip ✓";
        el.style.color = "var(--ok)";
        return;
      }
    } catch {}
    await sleep(5000);
  }
  el.textContent = "Still waiting for the box - once it joins your WiFi " +
                   "it will show up in your app.";
}

/* ---------------- wire up ---------------- */
$("nu-create").onclick = create;
$("fl-connect").onclick = () => connect(true);
$("fl-anyport").onclick = () => connect(false);
$("fl-back").onclick = () => { port = null; init(); };
init();
