"""voice: spoken prompts for the device voice-control accessibility flow.

A box with voice control enabled (devices.voice, admin toggle) asks its
questions out loud: "You have new messages - want to hear them?", "Send
a message to Mom?". The clips are rendered here with a local TTS and
served to boxes as .vmsg files (same 16 kHz opus container the firmware
already plays), synced like theme assets: content-hash versioned,
immutable with ?v=, listed only once fully rendered.

TTS backend, best available at runtime:
  - piper  (PIP_PIPER_VOICE=/path/to/voice.onnx; the Docker image ships
    one) - natural voice, used in production
  - espeak-ng - robotic but dependency-free fallback for dev machines
  - none - prompts simply aren't offered; the firmware degrades to a
    chime + on-screen text, so the flow still works

Rendering is lazy and runs in a worker thread (single uvicorn worker:
nothing TTS-shaped may block the event loop). ensure_user() renders
what's missing for one device user and notifies their boxes when new
clips land; a version is sha256(backend:voice:text)[:8], so a renamed
contact or a changed phrase re-renders and re-syncs by itself. Old
versions of a clip are pruned per-key; keys are never shared between
users' *contact* prompts (ask_send-<username>), and canned keys render
identically for everyone, so the store needs no cross-user GC."""
import hashlib
import logging
import os
import re
import shutil
import subprocess
import tempfile
import threading

from . import db, mqtt, vmsg

log = logging.getLogger("voice")

PROMPTS_DIR = os.path.join(db.DATA_DIR, "prompts")

# canned questions; per-contact "ask_send-<username>" is generated below.
# Keys are firmware API: voice.c looks clips up by these names.
PHRASES = {
    "ask_play": "You have new messages. Do you want to hear them?",
    "ask_confirm": "Send it?",
    "cancelled": "Okay, never mind.",
}
ASK_SEND = "Send a message to {name}?"

MAX_PROMPT_SECONDS = 10
_render_lock = threading.Lock()      # one render pass at a time


def backend() -> tuple[str, str] | None:
    """(name, voice-identifier) of the best available TTS, or None."""
    piper_voice = db.env("PIPER_VOICE", "")
    if piper_voice and os.path.exists(piper_voice) and shutil.which("piper"):
        return ("piper", os.path.basename(piper_voice))
    if shutil.which("espeak-ng"):
        return ("espeak-ng", "default")
    return None


def _tts_wav(text: str) -> bytes:
    """Render text to wav bytes with the active backend (raises on none).
    Temp files stay inside DATA_DIR: the systemd sandbox on self-hosted
    deployments only permits the data dir and a private /tmp."""
    be = backend()
    if not be:
        raise RuntimeError("no TTS backend available")
    with tempfile.TemporaryDirectory(dir=db.DATA_DIR) as td:
        out = os.path.join(td, "prompt.wav")
        if be[0] == "piper":
            subprocess.run(
                ["piper", "--model", db.env("PIPER_VOICE"),
                 "--output_file", out],
                input=text.encode(), check=True, capture_output=True)
        else:
            subprocess.run(["espeak-ng", "-w", out, text],
                           check=True, capture_output=True)
        with open(out, "rb") as f:
            return f.read()


def version_of(text: str) -> str:
    """8-hex hash keyed on backend+voice+text: any change re-renders."""
    be = backend() or ("none", "none")
    raw = f"{be[0]}:{be[1]}:{text}".encode()
    return hashlib.sha256(raw).hexdigest()[:8]


def prompt_path(key: str, ver: str) -> str:
    return os.path.join(PROMPTS_DIR, f"{key}-{ver}.vmsg")


def _phrase_table(user_id: int) -> dict[str, str]:
    """key -> spoken text for one device user: canned + their contacts."""
    table = dict(PHRASES)
    with db.conn() as c:
        rows = db.all_(c, """SELECT u.username, u.display_name
                             FROM perms p JOIN users u ON u.id=p.recipient
                             WHERE p.sender=? AND p.recipient != p.sender
                             ORDER BY u.display_name""", (user_id,))
    for r in rows:
        table[f"ask_send-{r['username']}"] = \
            ASK_SEND.format(name=r["display_name"])
    return table


def manifest(user_id: int) -> list[dict]:
    """[{key, ver}] for every prompt already rendered at its current
    version - never offer what isn't on disk (themes.available's rule)."""
    out = []
    for key, text in _phrase_table(user_id).items():
        ver = version_of(text)
        if os.path.exists(prompt_path(key, ver)):
            out.append({"key": key, "ver": ver})
    return out


def _render_user(user_id: int) -> int:
    """Render whatever is missing for one user; returns clips added."""
    os.makedirs(PROMPTS_DIR, exist_ok=True)
    added = 0
    for key, text in _phrase_table(user_id).items():
        ver = version_of(text)
        path = prompt_path(key, ver)
        if os.path.exists(path):
            continue
        try:
            wav = _tts_wav(text)
            data, _dur = vmsg.transcode_to_vmsg(wav, MAX_PROMPT_SECONDS)
        except Exception as e:
            log.error("prompt %s render failed: %s", key, e)
            continue
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
        # prune superseded versions of this key (renames, phrase edits)
        for name in os.listdir(PROMPTS_DIR):
            if (name.startswith(key + "-") and name.endswith(".vmsg")
                    and name != os.path.basename(path)
                    and re.fullmatch(re.escape(key) + r"-[0-9a-f]{8}\.vmsg",
                                     name)):
                os.unlink(os.path.join(PROMPTS_DIR, name))
        added += 1
        log.info("prompt %s-%s rendered (%d B)", key, ver, len(data))
    return added


def ensure_user(user_id: int) -> None:
    """Fire-and-forget: render this user's missing clips off-thread and
    poke their boxes when something new landed."""
    if not backend():
        return

    def work():
        with _render_lock:
            if _render_user(user_id):
                mqtt.notify_user(user_id, '{"event":"voice"}')

    threading.Thread(target=work, daemon=True,
                     name=f"voice-render-{user_id}").start()


def ensure_all() -> None:
    """Same, for every user owning a voice-enabled device (startup, and
    after contact/permission edits that may need new name clips)."""
    if not backend():
        return
    with db.conn() as c:
        ids = [r["user_id"] for r in db.all_(
            c, "SELECT DISTINCT user_id FROM devices WHERE voice=1")]
    for uid in ids:
        ensure_user(uid)
