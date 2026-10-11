#!/usr/bin/env python3
"""App icon and splash image, drawn from the pod's own N badge.

    python3 scripts/make-icons.py        # from mobile/, writes assets/images/

The badge is firmware/assets/hyundai_n_logo.png - the one the pod's boot
screen shows (shadow lifted to grey for a dark background) - so the app and
the gauge look like one thing. Around it, a tachometer arc like the pod's
gauges: a dim 270-degree track, the app's amber accent over most of it, a
redline at the end.

Android wants, besides a plain square icon:
  - an adaptive icon: a foreground layer on a background layer. Launchers
    mask it to their own shape and may zoom it, so everything that matters
    sits inside the central safe circle (66 of 108 dp: r = 313 px of 1024).
  - a monochrome layer for Android 13+ themed icons: one colour, shape only.
Drawn at 4x and scaled down, for smooth edges.
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent.parent
OUT = HERE / "assets" / "images"
BADGE = HERE.parent / "firmware" / "assets" / "hyundai_n_logo.png"

# mobile/src/theme.ts
BG = (13, 17, 22, 255)          # C.bg  #0d1116
TRACK = (38, 49, 60, 255)       # C.border #26313C
AMBER = (232, 163, 61, 255)     # C.accent #E8A33D
RED = (229, 84, 75, 255)        # C.bad #E5544B

SIZE = 1024
SS = 4                          # supersampling factor

START, SWEEP = 135, 270         # degrees, clockwise from 3 o'clock: a gauge's sweep
VALUE = 0.72                    # how much of it the amber covers
REDLINE = 0.86                  # where the redline starts


def arc(d: ImageDraw.ImageDraw, c: float, r: float, w: float, a0: float, a1: float, fill):
    """An arc of centreline radius r and width w, with round ends."""
    d.arc([c - r - w / 2, c - r - w / 2, c + r + w / 2, c + r + w / 2], a0, a1, fill=fill, width=round(w))
    for a in (a0, a1):
        x = c + r * math.cos(math.radians(a))
        y = c + r * math.sin(math.radians(a))
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=fill)


def gauge(r: float, w: float, badge_w: float, background: bool) -> Image.Image:
    """The icon at SIZE: arc of radius r (fraction of SIZE), badge badge_w wide."""
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), BG if background else (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c, rr, ww = n / 2, r * n, w * n
    end = START + SWEEP
    arc(d, c, rr, ww, START, end, TRACK)
    arc(d, c, rr, ww, START + SWEEP * REDLINE, end, RED)
    arc(d, c, rr, ww, START, START + SWEEP * VALUE, AMBER)

    badge = Image.open(BADGE).convert("RGBA")
    bw = round(badge_w * n)
    bh = round(badge.height * bw / badge.width)
    badge = badge.resize((bw, bh), Image.LANCZOS)
    # A touch above centre: the arc's open bottom leaves more room below.
    img.alpha_composite(badge, (round(c - bw / 2), round(c - bh / 2 - 0.02 * n)))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def monochrome(img: Image.Image) -> Image.Image:
    """Shape only: the launcher tints it. Opaque wherever the art is."""
    alpha = img.getchannel("A")
    out = Image.new("RGBA", img.size, (255, 255, 255, 0))
    out.putalpha(alpha)
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    # Adaptive foreground: outer edge of the arc at r = 0.297 (304 px), inside
    # the 313 px safe circle with a little room for launchers that zoom.
    fg = gauge(r=0.27, w=0.055, badge_w=0.40, background=False)
    fg.save(OUT / "android-icon-foreground.png")
    Image.new("RGBA", (SIZE, SIZE), BG).save(OUT / "android-icon-background.png")
    monochrome(fg).save(OUT / "android-icon-monochrome.png")

    # Plain square (older launchers, the store, web): no mask, so larger.
    gauge(r=0.38, w=0.07, badge_w=0.56, background=True).save(OUT / "icon.png")

    # Splash: the badge alone, on the app's background (app.json sets the
    # colour and the size it is shown at).
    badge = Image.open(BADGE).convert("RGBA")
    pad = Image.new("RGBA", (badge.width, badge.width), (0, 0, 0, 0))
    pad.alpha_composite(badge, (0, (badge.width - badge.height) // 2))
    pad.resize((SIZE, SIZE), Image.LANCZOS).save(OUT / "splash-icon.png")

    gauge(r=0.38, w=0.07, badge_w=0.56, background=True).resize((48, 48), Image.LANCZOS).save(OUT / "favicon.png")
    print("wrote", ", ".join(sorted(p.name for p in OUT.glob("*.png"))))


if __name__ == "__main__":
    main()
