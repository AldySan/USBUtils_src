#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the USBUtils application icon set.

Design: a PS2-memory-card-styled USB stick on a deep-blue rounded square
(the PS2 'Emotion Blue' palette), with a small disc + play accent.
Outputs (inside the output directory, default <repo>/res):
  usbutils.ico            (16,24,32,48,64,128,256 multi-size ICO)
  icons/usbutils_256.png  (hicolor source PNG)
  icons/usbutils.svg      (vector fallback for Linux packaging)

Drawn procedurally with PIL so the repo carries no binary blob
(unlike the data/ files which are verbatim originals).

Usage: gen_icon.py [output_dir]   (default: the repository's res/)
"""
from PIL import Image, ImageDraw
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 \
    else os.path.join(REPO_ROOT, "res")
os.makedirs(OUT, exist_ok=True)
os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)

# Emotion-Engine palette
BG_TOP = (16, 28, 58)      # deep navy
BG_BOT = (36, 64, 128)     # PS2 blue
BODY = (70, 116, 196)      # stick body
BODY_DK = (44, 84, 158)
METAL = (208, 214, 226)
METAL_DK = (150, 158, 175)
ACCENT = (255, 196, 48)    # PS2 gold/amber
WHITE = (255, 255, 255)
TXT = (232, 240, 252)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def vgrad(size, top, bot):
    img = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        d.line([(0, y), (size, y)], fill=lerp(top, bot, y / max(1, size - 1)))
    return img


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def draw_icon(size: int) -> Image.Image:
    S = 512  # draw large, downscale for crisp AA
    k = S / 256.0
    img = vgrad(S, BG_TOP, BG_BOT)
    d = ImageDraw.Draw(img)

    # subtle glow circle behind the stick
    d.ellipse([S * 0.16, S * 0.10, S * 0.84, S * 0.90],
              fill=(52, 92, 172, 90))

    # --- USB connector (top) ---
    cx = S / 2
    conn_w, conn_h = 84 * k, 86 * k
    conn_top = 52 * k
    d.rounded_rectangle([cx - conn_w / 2, conn_top,
                         cx + conn_w / 2, conn_top + conn_h],
                        radius=8 * k, fill=METAL)
    # connector shading + contacts
    d.rectangle([cx - conn_w / 2 + 6 * k, conn_top + conn_h - 10 * k,
                 cx + conn_w / 2 - 6 * k, conn_top + conn_h - 2 * k],
                fill=METAL_DK)
    for i, xoff in enumerate((-24, -8, 8, 24)):
        d.rectangle([cx + xoff * k - 5 * k, conn_top + 12 * k,
                     cx + xoff * k + 5 * k, conn_top + 34 * k],
                    fill=(120, 128, 146))

    # --- stick body (bottom) ---
    body_w, body_h = 150 * k, 190 * k
    body_top = conn_top + conn_h - 6 * k
    d.rounded_rectangle([cx - body_w / 2, body_top,
                         cx + body_w / 2, body_top + body_h],
                        radius=22 * k, fill=BODY)
    # side shade (left edge darker, right lighter)
    d.rounded_rectangle([cx - body_w / 2, body_top,
                         cx - body_w / 2 + 16 * k, body_top + body_h],
                        radius=22 * k, fill=BODY_DK)
    d.rounded_rectangle([cx + body_w / 2 - 16 * k, body_top,
                         cx + body_w / 2, body_top + body_h],
                        radius=22 * k, fill=(96, 140, 220))

    # --- PS2 mark: stylized disc + play ---
    dcx, dcy, r = cx, body_top + 74 * k, 42 * k
    d.ellipse([dcx - r, dcy - r, dcx + r, dcy + r],
              outline=WHITE, width=int(9 * k))
    d.ellipse([dcx - 8 * k, dcy - 8 * k, dcx + 8 * k, dcy + 8 * k],
              fill=ACCENT)
    # play triangle inside the ring
    tri = [(dcx - 10 * k, dcy - 17 * k), (dcx - 10 * k, dcy + 17 * k),
           (dcx + 18 * k, dcy)]
    d.polygon(tri, fill=WHITE)

    # --- label strip at the bottom of the body ---
    strip_top = body_top + body_h - 46 * k
    d.rounded_rectangle([cx - body_w / 2 + 14 * k, strip_top,
                         cx + body_w / 2 - 14 * k, strip_top + 28 * k],
                        radius=8 * k, fill=(24, 44, 92))
    if size >= 48:
        # 'PS2' glyph text drawn only in big render (S=512)
        try:
            from PIL import ImageFont
            f = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                int(26 * k))
            d.text((cx, strip_top + 13 * k), "PS2", font=f,
                   fill=TXT, anchor="mm")
        except Exception:
            pass

    # rounded-square crop + downscale
    radius = int(S * 0.22)
    mask = rounded_mask(S, radius)
    img.putalpha(mask)
    img = img.resize((size, size), Image.LANCZOS)
    return img


# ---- render sizes ----------------------------------------------------------
sizes = [16, 24, 32, 48, 64, 128, 256]
base = draw_icon(512)  # full-quality base

frames = {}
for s in sizes:
    frames[s] = base.resize((s, s), Image.LANCZOS)

# multi-resolution ICO
ico_path = os.path.join(OUT, "usbutils.ico")
frames[256].save(ico_path, format="ICO",
                 sizes=[(s, s) for s in sizes],
                 append_images=[frames[s] for s in sizes[:-1]])
print("wrote", ico_path)

# PNG set for Linux hicolor
for s in sizes:
    p = os.path.join(OUT, "icons", f"usbutils_{s}.png")
    frames[s].save(p)
print("wrote PNG set")

# simple SVG companion (scales anywhere, used by .desktop / theme installs)
svg = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#101c3a"/>
      <stop offset="1" stop-color="#244080"/>
    </linearGradient>
    <linearGradient id="body" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#2c549e"/>
      <stop offset="0.5" stop-color="#4674c4"/>
      <stop offset="1" stop-color="#608cdc"/>
    </linearGradient>
  </defs>
  <rect x="8" y="8" width="240" height="240" rx="52" fill="url(#bg)"/>
  <rect x="86" y="52" width="84" height="86" rx="8" fill="#d0d6e2"/>
  <rect x="92" y="128" width="72" height="10" fill="#969eaf"/>
  <g fill="#788092">
    <rect x="103" y="64" width="10" height="22"/><rect x="119" y="64" width="10" height="22"/>
    <rect x="135" y="64" width="10" height="22"/><rect x="151" y="64" width="10" height="22"/>
  </g>
  <rect x="53" y="132" width="150" height="190" rx="22" fill="url(#body)"/>
  <circle cx="128" cy="206" r="42" fill="none" stroke="#ffffff" stroke-width="9"/>
  <circle cx="128" cy="206" r="8" fill="#ffc430"/>
  <path d="M 118 189 L 118 223 L 146 206 Z" fill="#ffffff"/>
  <rect x="67" y="276" width="122" height="28" rx="8" fill="#182c5c"/>
  <text x="128" y="298" font-family="DejaVu Sans, sans-serif" font-size="24"
        font-weight="bold" fill="#e8f0fc" text-anchor="middle">PS2</text>
</svg>
"""
svg_path = os.path.join(OUT, "icons", "usbutils.svg")
with open(svg_path, "w") as f:
    f.write(svg)
print("wrote", svg_path)
