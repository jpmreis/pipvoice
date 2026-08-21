#!/usr/bin/env python3
"""Package a built firmware into release assets.

    make_manifest.py --version 0.1.30 --build firmware/build \
                     --changelog CHANGELOG.md --out dist

Copies the flash bundle out of the ESP-IDF build tree under stable
release names, writes manifest.json — an ESP Web Tools-compatible
superset: each part carries `kind` and `sha256`, which the Pip server
verifies when it ingests a release (server/app/release.py) — and
extracts the version's CHANGELOG section into RELEASE_NOTES.md for the
GitHub Release body.

The manifest's `builds` list is per board; there is currently one
supported board. A new board means a new entry here plus a matrix leg
in .github/workflows/release.yml.
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

BOARD = "waveshare-esp32-s3-touch-amoled-1.8"


def asset_name(version: str, kind: str) -> str:
    return (f"pip-{version}.bin" if kind == "app"
            else f"pip-{version}.{kind}.bin")


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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--build", required=True, help="ESP-IDF build dir")
    ap.add_argument("--changelog", help="CHANGELOG.md to extract notes from")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", a.version):
        print(f"bad version: {a.version}", file=sys.stderr)
        return 1
    os.makedirs(a.out, exist_ok=True)

    parts = []
    for kind, src, offset in PARTS:
        src_path = os.path.join(a.build, src)
        if not os.path.exists(src_path):
            print(f"missing build artifact: {src_path}", file=sys.stderr)
            return 1
        name = asset_name(a.version, kind)
        shutil.copyfile(src_path, os.path.join(a.out, name))
        with open(src_path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        parts.append({"path": name, "offset": offset,
                      "kind": kind, "sha256": sha})

    manifest = {"name": "Pip", "version": a.version,
                "builds": [{"board": BOARD, "chipFamily": "ESP32-S3",
                            "parts": parts}]}
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

    print(f"packaged {len(parts)} parts for v{a.version} into {a.out}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
