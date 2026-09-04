#!/usr/bin/env python3
"""Generate per-format Pip theme masters with Gemini (Nano Banana).

Edits each existing master (server/app/themes/<name>.jpeg) into three
new formats, sequentially, resumable (existing outputs are skipped):

    <out>/<name>-square.jpeg   1:1, for the 2.16 (480x480 crop)
    <out>/<name>-round.jpeg    1:1, all content in an inscribed circle,
                               black corners, for the 1.75-B (466x466)
    <out>/<name>-phone.jpeg    9:16 portrait, for the PWA

Usage:
    pip install google-genai
    GEMINI_API_KEY=... python3 gen_theme_masters.py \
        --src ~/pip/server/app/themes --out ./masters-new \
        [--themes sea,pink] [--formats round,phone] \
        [--model gemini-2.5-flash-image]

The frame-style themes (olivia, portugal) and garden are the fussy
ones - re-run just those with --themes after reviewing.
"""
import argparse
import os
import sys
import time

from google import genai
from google.genai import types

BASE = {
    "square": (
        "Recompose this exact image onto a square 1:1 canvas. Preserve "
        "the original artwork's subject, style, palette and lighting "
        "precisely - extend and rearrange, do not reinvent. {theme} "
        "Keep every important element at least 10% away from all edges. "
        "High detail, no added text, no border, no watermark."),
    "round": (
        "Recompose this exact image onto a square 1:1 canvas where ALL "
        "content lives inside a centered circle that touches the four "
        "edges; fill the area outside the circle with solid pure black. "
        "Preserve the original artwork's subject, style, palette and "
        "lighting precisely - extend and rearrange, do not reinvent. "
        "{theme} Nothing important within 12% of the circle's rim; the "
        "composition must read as made-for-round, not cropped. No added "
        "text, no watermark."),
    "phone": (
        "Recompose this exact image onto a tall 9:16 portrait canvas. "
        "Preserve the original artwork's subject, style, palette and "
        "lighting precisely - extend the scene vertically, do not "
        "reinvent. {theme} Keep the key subject in the central vertical "
        "band, safe for top/bottom cropping; the top quarter and center "
        "should stay calm enough for white or black UI text. No added "
        "text, no border, no watermark."),
}

ASPECT = {"square": "1:1", "round": "1:1", "phone": "9:16"}

THEMES = {
    "cloud": {
        "*": "Soft blurred pastel smoke-waves (rose, sage, slate blue, "
             "a thread of amber) drifting over near-black; keep the "
             "darkest, calmest region in the center."},
    "dark": {
        "*": "Layered sinuous ribbon curves in deep navy, forest green, "
             "copper and amber gradients on dark; let the curves flow "
             "through the whole canvas."},
    "garden": {
        "*": "Cozy colored-pencil garden: the cottage with the red "
             "tiled roof in the upper area, the winding stone path "
             "leading to it, rose beds and hollyhocks at the sides, the "
             "tabby cat and butterflies; keep the 'My Garden' sign "
             "exactly as it is and add no other text.",
        "round": "Cozy colored-pencil garden: the winding stone path "
             "curling inward from the bottom of the circle up to the "
             "cottage with the red tiled roof near the top, rose beds "
             "and hollyhocks along the sides, the tabby cat and "
             "butterflies; keep the 'My Garden' sign exactly as it is "
             "and add no other text."},
    "olivia": {
        "*": "The purple sticker-doodle frame (roller skates, "
             "cassettes, vinyl, headphones, lightning bolts, hearts, "
             "butterflies, and the existing SOUR / GUTS / GOOD 4 U "
             "stickers, unchanged) arranged as a border hugging the "
             "canvas edge, with the soft lilac-to-pink gradient left "
             "EMPTY in the middle.",
        "round": "The purple sticker-doodle frame (roller skates, "
             "cassettes, vinyl, headphones, lightning bolts, hearts, "
             "butterflies, and the existing SOUR / GUTS / GOOD 4 U "
             "stickers, unchanged) arranged as a ring following the "
             "circle's rim, with the soft lilac-to-pink gradient left "
             "EMPTY in the middle."},
    "pink": {
        "*": "Pastel rainbow candy-clouds, stars and sparkles in pink, "
             "mint, lilac and lemon gradients filling the whole canvas "
             "evenly."},
    "sea": {
        "*": "Tropical beach at sunset: sun just above the horizon in "
             "the upper third, its reflection running down the water, "
             "wave foam on wet sand in the lower half, palm silhouettes "
             "at one side.",
        "round": "Tropical beach at sunset: sun and horizon inside the "
             "upper part of the circle, its reflection running down the "
             "water, wave foam on wet sand in the lower half, palm "
             "silhouettes curving along the left rim."},
    "benfica": {
        "*": "The red and white diagonal lightning-bolt split with the "
             "eagle, the club crest and the existing wordmark preserved "
             "exactly, faint stadium and footballs in the background "
             "texture; rebalance the elements for the new canvas and "
             "add nothing new."},
    "portugal": {
        "*": "The green-and-red swirl border with the cartoon lion "
             "mascots in Portugal kit kicking footballs and the small "
             "Portuguese flags, framing a pale geometric center left "
             "EMPTY.",
        "round": "The green-and-red swirl border with the cartoon lion "
             "mascots in Portugal kit kicking footballs and the small "
             "Portuguese flags following the circle's rim, framing a "
             "pale geometric center left EMPTY."},
    "namibia": {
        "*": "Savanna sunset: the lone acacia silhouette just "
             "off-center, red dunes behind, small elephant and "
             "wildebeest herds along the ground line, orange-to-indigo "
             "sky above; horizon in the lower third."},
    "mario": {
        "*": "The existing character artwork preserved exactly, waving "
             "from the bottom edge, with green hills, pipes, blocks and "
             "3D clouds in a wide blue sky; keep the sky open and calm "
             "above him.",
        "round": "The existing character artwork preserved exactly, "
             "peeking up from the bottom of the circle, with green "
             "hills, pipes, blocks and 3D clouds in a wide blue sky; "
             "keep the sky open and calm above him."},
}


def generate(client, model, src_path, prompt, aspect, out_path,
             retries=3):
    with open(src_path, "rb") as f:
        img = f.read()
    for attempt in range(retries):
        try:
            resp = client.models.generate_content(
                model=model,
                contents=[types.Part.from_bytes(data=img,
                                                mime_type="image/jpeg"),
                          prompt],
                config=types.GenerateContentConfig(
                    response_modalities=["IMAGE"],
                    image_config=types.ImageConfig(aspect_ratio=aspect)))
            for part in resp.candidates[0].content.parts:
                if part.inline_data and part.inline_data.data:
                    with open(out_path, "wb") as f:
                        f.write(part.inline_data.data)
                    return True
            print(f"  no image in response (attempt {attempt + 1}): "
                  f"{getattr(resp, 'text', '')[:200]}")
        except Exception as e:                      # rate limit, transient
            print(f"  error (attempt {attempt + 1}): {e}")
        time.sleep(15 * (attempt + 1))
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="dir with <name>.jpeg")
    ap.add_argument("--out", required=True)
    ap.add_argument("--themes", default=",".join(THEMES))
    ap.add_argument("--formats", default="square,round,phone")
    ap.add_argument("--model", default="gemini-2.5-flash-image")
    a = ap.parse_args()

    if not os.environ.get("GEMINI_API_KEY"):
        sys.exit("set GEMINI_API_KEY (aistudio.google.com/apikey)")
    client = genai.Client()
    os.makedirs(a.out, exist_ok=True)

    todo = [(t, f) for t in a.themes.split(",")
            for f in a.formats.split(",")]
    failed = []
    for i, (theme, fmt) in enumerate(todo, 1):
        src = os.path.join(os.path.expanduser(a.src), f"{theme}.jpeg")
        dst = os.path.join(a.out, f"{theme}-{fmt}.jpeg")
        if os.path.exists(dst):
            print(f"[{i}/{len(todo)}] {theme}-{fmt}: exists, skipping")
            continue
        subject = THEMES[theme].get(fmt) or THEMES[theme]["*"]
        prompt = BASE[fmt].format(theme=subject)
        print(f"[{i}/{len(todo)}] {theme}-{fmt} ...")
        if generate(client, a.model, src, prompt, ASPECT[fmt], dst):
            print(f"  -> {dst}")
        else:
            failed.append(f"{theme}-{fmt}")
        time.sleep(5)                    # be gentle with the rate limit

    if failed:
        print("\nFAILED:", ", ".join(failed))
        sys.exit(1)
    print("\nall done")


if __name__ == "__main__":
    main()
