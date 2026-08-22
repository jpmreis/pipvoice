"""vmsg: Python side of the .vmsg voice container (see firmware opus_file.c).

Browser clients can't produce the container, so uploads that aren't already
VMSG are transcoded here to the firmware encoder's exact settings — 16 kHz
mono, 20 ms frames, 16 kbps VOIP — and inbox audio is re-rendered for
<audio> playback. ffmpeg handles arbitrary browser containers (AAC/MP4 from
iOS Safari, WebM/Opus elsewhere); opuslib (libopus) does the Opus work.

Playback format: AAC-in-MP4, rendered once at send time and cached next to
the .vmsg. The WAV path is still served for clients running an older cached
app.js, but nothing new should use it — decoded PCM is 32 KB/s, so a 20 s
message was a 640 KB download on a phone that had just been told, over
push, that it had mail. That gap is what made bad wifi feel broken.
"""
import logging
import os
import struct
import subprocess
import tempfile

import opuslib

from . import db

log = logging.getLogger("vmsg")

SAMPLE_RATE = 16000
FRAME_MS = 20
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000          # 320
FRAME_BYTES = FRAME_SAMPLES * 2                         # s16 mono
MAX_PACKET = 400                                        # firmware reader cap


def _ffmpeg_decode(data: bytes) -> bytes:
    """Any audio container -> raw s16le 16 kHz mono. Via a temp file:
    MP4 needs seekable input, so stdin piping is not an option."""
    with tempfile.NamedTemporaryFile(suffix=".audio", delete=False) as tf:
        tf.write(data)
        src = tf.name
    try:
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", src,
             "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1",
             "-ar", str(SAMPLE_RATE), "pipe:1"],
            capture_output=True)
    finally:
        os.unlink(src)
    if p.returncode != 0 or not p.stdout:
        raise ValueError("audio decode failed: "
                         + p.stderr.decode(errors="replace")[-300:])
    return p.stdout


def transcode_to_vmsg(data: bytes, max_seconds: int) -> tuple[bytes, int]:
    """Browser upload -> (vmsg bytes, duration seconds). Raises ValueError
    on undecodable audio or over-length recordings."""
    pcm = _ffmpeg_decode(data)
    duration_s = len(pcm) // (SAMPLE_RATE * 2)
    if duration_s > max_seconds:
        raise ValueError(f"recording longer than {max_seconds}s limit")

    enc = opuslib.Encoder(SAMPLE_RATE, 1, opuslib.APPLICATION_VOIP)
    try:
        # mirror the firmware encoder (opus_file.c). These CTLs fail on
        # arm64 macOS dev machines (ctypes can't do variadic calls there);
        # they work on the x86_64 prod host, and defaults still decode fine.
        enc.bitrate = 16000
        enc.complexity = 3
        enc.signal = opuslib.SIGNAL_VOICE
    except opuslib.OpusError:
        import logging
        logging.getLogger("vmsg").warning(
            "opus encoder CTLs unavailable; using libopus defaults")

    out = bytearray(b"VMSG")
    out += struct.pack("<4H", 1, SAMPLE_RATE // 100, FRAME_MS,
                       min(duration_s, 0xFFFF))
    for off in range(0, len(pcm) - FRAME_BYTES + 1, FRAME_BYTES):
        pkt = enc.encode(bytes(pcm[off:off + FRAME_BYTES]), FRAME_SAMPLES)
        if not pkt or len(pkt) > MAX_PACKET:
            raise ValueError("opus packet out of range")
        out += struct.pack("<H", len(pkt)) + pkt
    if len(out) <= 12:
        raise ValueError("empty recording")
    return bytes(out), duration_s


def _vmsg_to_pcm(data: bytes) -> tuple[bytes, int]:
    """.vmsg bytes -> (raw s16 mono PCM, sample rate)."""
    if len(data) < 12 or data[:4] != b"VMSG":
        raise ValueError("not a vmsg file")
    _ver, sr100, frame_ms, _dur = struct.unpack_from("<4H", data, 4)
    sr = sr100 * 100
    frame_samples = sr * frame_ms // 1000
    dec = opuslib.Decoder(sr, 1)

    pcm = bytearray()
    off = 12
    while off + 2 <= len(data):
        (n,) = struct.unpack_from("<H", data, off)
        off += 2
        if n == 0 or n > MAX_PACKET or off + n > len(data):
            break
        pcm += dec.decode(bytes(data[off:off + n]), frame_samples)
        off += n
    return bytes(pcm), sr


def vmsg_to_wav(data: bytes) -> bytes:
    """.vmsg bytes -> WAV (s16 mono). Legacy playback path; see the module
    docstring — new clients fetch the .m4a."""
    pcm, sr = _vmsg_to_pcm(data)
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(pcm), b"WAVE",
                      b"fmt ", 16, 1, 1, sr, sr * 2, 2, 16,
                      b"data", len(pcm))
    return hdr + pcm


# 32 kbps AAC-LC over a 16 kbps Opus source: the second encode is
# transparent enough at this rate, and it is 8x smaller than the PCM.
PLAYBACK_BITRATE = "32k"


def vmsg_to_m4a(data: bytes) -> bytes:
    """.vmsg bytes -> AAC in MP4, playable by every browser we target."""
    pcm, sr = _vmsg_to_pcm(data)
    if not pcm:
        raise ValueError("empty recording")
    # MP4 muxing needs a seekable output, so this can't pipe to stdout
    with tempfile.NamedTemporaryFile(suffix=".m4a", delete=False) as tf:
        dst = tf.name
    try:
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "s16le", "-ar", str(sr), "-ac", "1", "-i", "pipe:0",
             "-c:a", "aac", "-b:a", PLAYBACK_BITRATE,
             # moov atom first: <audio> can start on the leading bytes
             # instead of waiting out the whole download
             "-movflags", "+faststart", "-f", "mp4", dst],
            input=pcm, capture_output=True)
        if p.returncode != 0:
            raise ValueError("aac encode failed: "
                             + p.stderr.decode(errors="replace")[-300:])
        with open(dst, "rb") as f:
            out = f.read()
    finally:
        try:
            os.unlink(dst)
        except FileNotFoundError:
            pass
    if not out:
        raise ValueError("aac encode produced nothing")
    return out


def ensure_playback(msg_id: str) -> str | None:
    """Render the .m4a once, next to the .vmsg, and return its path (None
    if the source audio is gone or ffmpeg refused it — callers fall back
    to the WAV). Written to a temp name and renamed into place: the whole
    point is that a client fetching this can never open a half file."""
    dst = db.playback_path(msg_id)
    if os.path.exists(dst):
        return dst
    src = db.audio_path(msg_id)
    if not os.path.exists(src):
        return None
    with open(src, "rb") as f:
        data = f.read()
    try:
        out = vmsg_to_m4a(data)
    except Exception as e:
        log.warning("playback render failed for %s: %s", msg_id, e)
        return None
    tmp = f"{dst}.{os.getpid()}.part"
    try:
        with open(tmp, "wb") as f:
            f.write(out)
        os.replace(tmp, dst)
    except OSError as e:
        log.warning("playback write failed for %s: %s", msg_id, e)
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        return None
    return dst
