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


# ---- ingest loudness normalization ----
# Prod finding 2026-09-03 (amplitude survey of 26 messages): every source
# masters peaks at/near 0 dBFS but average level around -20 dBFS, so the
# box speaker's clean ceiling is spent on transients and everything sounds
# quiet; hot box recordings additionally sit 10% of samples in the limiter
# zone. Fix at ingest, once, for every source: static gain toward a target
# mean level, with a lookahead limiter absorbing the peaks that gain pushes
# over the ceiling. Static gain (not dynamic normalization) on purpose -
# no pumping, no noise-floor breathing between words.
TARGET_MEAN_DB = -14.0     # volumedetect mean_volume target (incl. pauses)
PEAK_CEILING_DB = -1.0     # true ceiling after the limiter
LIMITER_GIVE_DB = 8.0      # how far past peak-fit the limiter may absorb
MAX_BOOST_DB = 12.0        # don't amplify room tone into hiss
MAX_CUT_DB = -6.0          # tame hot recordings, don't crush them


def _measure_pcm(pcm: bytes, sr: int) -> tuple[float, float]:
    """(mean_volume dB, max_volume dB) of raw s16 mono via volumedetect."""
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-f", "s16le", "-ar", str(sr), "-ac", "1",
         "-i", "pipe:0", "-af", "volumedetect", "-f", "null", "-"],
        input=pcm, capture_output=True)
    mean_db = max_db = None
    for line in p.stderr.decode(errors="replace").splitlines():
        if "mean_volume:" in line:
            mean_db = float(line.split("mean_volume:")[1].split("dB")[0])
        elif "max_volume:" in line:
            max_db = float(line.split("max_volume:")[1].split("dB")[0])
    if mean_db is None or max_db is None:
        raise ValueError("volumedetect produced no measurement")
    return mean_db, max_db


def normalize_pcm(pcm: bytes, sr: int = SAMPLE_RATE) -> bytes:
    """Level-normalize raw s16 mono PCM. Returns the input unchanged when
    the level is already right or anything goes wrong - loudness is never
    worth losing a message over."""
    try:
        mean_db, max_db = _measure_pcm(pcm, sr)
        gain = min(TARGET_MEAN_DB - mean_db,
                   PEAK_CEILING_DB - max_db + LIMITER_GIVE_DB)
        gain = max(min(gain, MAX_BOOST_DB), MAX_CUT_DB)
        if abs(gain) < 1.0:
            return pcm
        limit = 10.0 ** (PEAK_CEILING_DB / 20.0)
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-f", "s16le", "-ar", str(sr), "-ac", "1", "-i", "pipe:0",
             "-af", f"volume={gain:.1f}dB,"
                    f"alimiter=level=false:limit={limit:.3f}",
             "-f", "s16le", "-ar", str(sr), "-ac", "1", "pipe:1"],
            input=pcm, capture_output=True)
        if p.returncode != 0 or not p.stdout:
            raise ValueError(p.stderr.decode(errors="replace")[-200:])
        log.info("normalized: mean %.1f max %.1f -> gain %+.1f dB",
                 mean_db, max_db, gain)
        return p.stdout
    except Exception as e:
        log.warning("normalization skipped: %s", e)
        return pcm


def normalize_vmsg(data: bytes) -> bytes:
    """Level-normalize a box-recorded .vmsg at ingest: decode, normalize,
    re-encode. The tandem 16 kbps Opus generation is the price of a level
    fix the firmware can't do; when the level is already right (gain under
    the 1 dB threshold) the original bytes pass through untouched, and any
    failure also returns them untouched."""
    try:
        pcm, sr = _vmsg_to_pcm(data)
        if not pcm:
            return data
        out = normalize_pcm(pcm, sr)
        if out is pcm:                      # already at level: keep the
            return data                     # original single-generation opus
        _ver, _sr100, _frame_ms, dur = struct.unpack_from("<4H", data, 4)
        return _encode_vmsg(out, dur)
    except Exception as e:
        log.warning("vmsg normalization skipped: %s", e)
        return data


def _encode_vmsg(pcm: bytes, duration_s: int) -> bytes:
    """Raw s16 mono 16 kHz PCM -> vmsg bytes (firmware encoder settings)."""
    enc = opuslib.Encoder(SAMPLE_RATE, 1, opuslib.APPLICATION_VOIP)
    try:
        # mirror the firmware encoder (opus_file.c). These CTLs fail on
        # arm64 macOS dev machines (ctypes can't do variadic calls there);
        # they work on the x86_64 prod host, and defaults still decode fine.
        enc.bitrate = 16000
        enc.complexity = 3
        enc.signal = opuslib.SIGNAL_VOICE
    except opuslib.OpusError:
        log.warning("opus encoder CTLs unavailable; using libopus defaults")

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
    return bytes(out)


def transcode_to_vmsg(data: bytes, max_seconds: int) -> tuple[bytes, int]:
    """Browser upload -> (vmsg bytes, duration seconds). Raises ValueError
    on undecodable audio or over-length recordings."""
    pcm = _ffmpeg_decode(data)
    duration_s = len(pcm) // (SAMPLE_RATE * 2)
    if duration_s > max_seconds:
        raise ValueError(f"recording longer than {max_seconds}s limit")
    pcm = normalize_pcm(pcm)
    return _encode_vmsg(pcm, duration_s), duration_s


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
