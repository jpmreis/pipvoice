"""vmsg: Python side of the .vmsg voice container (see firmware opus_file.c).

Browser clients can't produce the container, so uploads that aren't already
VMSG are transcoded here to the firmware encoder's exact settings — 16 kHz
mono, 20 ms frames, 16 kbps VOIP — and inbox audio is decoded to WAV for
<audio> playback. ffmpeg handles arbitrary browser containers (AAC/MP4 from
iOS Safari, WebM/Opus elsewhere); opuslib (libopus) does the Opus work.
"""
import os
import struct
import subprocess
import tempfile

import opuslib

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


def vmsg_to_wav(data: bytes) -> bytes:
    """.vmsg bytes -> WAV (s16 mono) for browser playback."""
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

    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(pcm), b"WAVE",
                      b"fmt ", 16, 1, 1, sr, sr * 2, 2, 16,
                      b"data", len(pcm))
    return hdr + bytes(pcm)
