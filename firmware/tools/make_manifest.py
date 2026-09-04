#!/usr/bin/env python3
"""Package built firmwares into release assets.

    make_manifest.py --version 1.4.0 \
        --build amoled-1.8=firmware/build-amoled-1.8 \
        --build amoled-1.75b=firmware/build-amoled-1.75b \
        --build amoled-2.16=firmware/build-amoled-2.16 \
        --changelog CHANGELOG.md --out dist

Copies each board's flash bundle out of its ESP-IDF build tree under
stable release names, writes ONE manifest.json - an ESP Web
Tools-compatible superset: one `builds[]` entry per board, each part
carrying `kind` and `sha256`, which the Pip server verifies when it
ingests a release (server/app/release.py) - and extracts the version's
CHANGELOG section into RELEASE_NOTES.md for the GitHub Release body.

Asset naming: the amoled-1.8 keeps the legacy names (pip-<ver>.bin,
pip-<ver>.<kind>.bin) so pre-multi-SKU servers keep working; other
boards are pip-<ver>-<board>[.<kind>].bin.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

# (kind, path in the ESP-IDF build tree, flash offset) — offsets mirror
# firmware/partitions.csv and the server's /v1/setup flash manifest
PARTS = [
    ("bootloader", "bootloader/bootloader.bin", 0x0),
    ("parttable", "partition_table/partition-table.bin", 0x8000),
    ("app", "pip.bin", 0x20000),
]

# board key (PIP_BOARD / server boards.py) -> manifest board name
# (server boards.py by_manifest_name); keep the two in lockstep
BOARDS = {
    "amoled-1.8":   "waveshare-esp32-s3-touch-amoled-1.8",
    "amoled-1.75b": "waveshare-esp32-s3-touch-amoled-1.75-b",
    "amoled-2.16":  "waveshare-esp32-s3-touch-amoled-2.16",
}
DEFAULT_BOARD = "amoled-1.8"


def asset_name(version: str, board: str, kind: str) -> str:
    suffix = "" if board == DEFAULT_BOARD else f"-{board}"
    return (f"pip-{version}{suffix}.bin" if kind == "app"
            else f"pip-{version}{suffix}.{kind}.bin")


def changelog_section(text: str, version: str) -> str:
    """The body under `## [<version>]`, up to the next `## ` heading."""
    lines = text.splitlines()
    out, keep = [], False
    for ln in lines:
        if ln.startswith("## "):
            keep = ln.startswith(f"## [{version}]")
            continue
        if keep:
            out.append(ln)
    return "\n".join(out).strip()


def package_build(version: str, board: str, build_dir: str,
                  out_dir: str) -> dict:
    parts = []
    for kind, src, offset in PARTS:
        src_path = os.path.join(build_dir, src)
        if not os.path.exists(src_path):
            raise FileNotFoundError(f"missing build artifact: {src_path}")
        name = asset_name(version, board, kind)
        shutil.copyfile(src_path, os.path.join(out_dir, name))
        with open(src_path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        parts.append({"path": name, "offset": offset,
                      "kind": kind, "sha256": sha})
    return {"board": BOARDS[board], "chipFamily": "ESP32-S3",
            "parts": parts}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--build", action="append", required=True,
                    metavar="BOARD=DIR",
                    help="board key = ESP-IDF build dir (repeatable)")
    ap.add_argument("--changelog", help="CHANGELOG.md to extract notes from")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", a.version):
        print(f"bad version: {a.version}", file=sys.stderr)
        return 1
    os.makedirs(a.out, exist_ok=True)

    builds = []
    for spec in a.build:
        board, _, build_dir = spec.partition("=")
        if board not in BOARDS or not build_dir:
            print(f"bad --build spec: {spec!r} "
                  f"(boards: {', '.join(BOARDS)})", file=sys.stderr)
            return 1
        try:
            builds.append(package_build(a.version, board, build_dir, a.out))
        except FileNotFoundError as e:
            print(str(e), file=sys.stderr)
            return 1

    manifest = {"name": "Pip", "version": a.version, "builds": builds}
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    notes = f"Pip v{a.version}"
    if a.changelog:
        with open(a.changelog) as f:
            section = changelog_section(f.read(), a.version)
        if section:
            notes = section
        else:
            print(f"warning: no CHANGELOG section for {a.version}",
                  file=sys.stderr)
    with open(os.path.join(a.out, "RELEASE_NOTES.md"), "w") as f:
        f.write(notes + "\n")

    print(f"packaged {len(builds)} board build(s) for v{a.version} "
          f"into {a.out}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
