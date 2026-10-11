#!/usr/bin/env python3
"""Convert a picture into an LVGL 9 image compiled into the firmware.

    scripts/png2lvgl.py IN OUT.c NAME [--a8] [--trim] [--max W H] [--bold PX]

Writes OUT.c defining `const lv_image_dsc_t NAME`. Use it from C++ with
`extern "C" const lv_image_dsc_t NAME;` (the .c file is compiled as C) and
`lv_image_set_src(img, &NAME);`.

Two formats:

- default, RGB565A8: the panel's own 16-bit colour, then one alpha byte per
  pixel in a separate plane - 3 bytes/pixel. For pictures (the tyre screen's
  car).
- --a8: alpha only, 1 byte/pixel. LVGL draws it as a mask in the object's
  image_recolor colour (lv_draw_sw_img.c: `blend_dsc.color = recolor`), so ONE
  icon can be shown blue, orange or red to follow a state. For symbols.
  The shape comes from the alpha channel if the file has real transparency;
  otherwise from how far each pixel's brightness is from the background
  colour at the top-left corner, in either direction - so red-on-black,
  white-on-grey and a black outline on white all give the same kind of mask.

Either way the image is copied row by row when drawn, with no decoder and no
RAM copy.

--trim crops to the visible shape first, so pictures with different padding
come out the same size for the same --max. --max fits the result inside
W x H, keeping its aspect ratio. Resize here, not on the device: LVGL can
scale at draw time, but that turns a row copy into per-pixel filtering on
every redraw.

The assets in the firmware were made with:
    scripts/png2lvgl.py firmware/assets/fastback.png \\
        firmware/car_gauge_v1/car_img.c car_top --trim --max 200 280
    scripts/png2lvgl.py firmware/assets/oil_temp.png \\
        firmware/car_gauge_v1/icon_oil.c icon_oil --a8 --trim --max 60 46
    scripts/png2lvgl.py firmware/assets/coolant_temp.webp \\
        firmware/car_gauge_v1/icon_coolant.c icon_coolant --a8 --trim --max 46 40
    scripts/png2lvgl.py firmware/assets/turbo.jpg \\
        firmware/car_gauge_v1/icon_turbo.c icon_turbo --a8 --trim --max 50 50
    scripts/png2lvgl.py firmware/assets/hyundai_n_logo.png \\
        firmware/car_gauge_v1/n_logo_img.c n_logo --trim --max 280 130
  (hyundai_n_logo.png is hyundai_n_logo_orig.png - Wikipedia's Hyundai N
  badge - with every opaque pixel darker than luminance 90 set to #6A7480:
  the near-black outline and shadow would vanish on the dark panel.)
    scripts/png2lvgl.py firmware/assets/satellite.jpg \\
        firmware/car_gauge_v1/icon_sat.c icon_sat --a8 --trim --max 50 50 --bold 6
    rsvg-convert -w 480 -h 480 firmware/assets/flag_sports_score.svg -o /tmp/flag480.png
    scripts/png2lvgl.py /tmp/flag480.png \\
        firmware/car_gauge_v1/icon_flag.c icon_flag --a8 --trim --max 60 60

Where each source picture came from, and its licence: firmware/assets/README.md.

The output is generated - edit the source picture and re-run, never the .c.
"""
import argparse
import os
import sys

from PIL import Image

TRIM_THRESHOLD = 16   # alpha below this counts as background when trimming


def shape_mask(img):
    """Alpha mask (mode L) for --a8: real transparency if present, else
    brightness relative to the background colour."""
    rgba = img.convert("RGBA")
    a = rgba.getchannel("A")
    if a.getextrema()[0] < 255:
        return a
    # The shape is whatever differs from the background, in either direction:
    # a bright symbol on black and a black outline on white both work.
    bg = max(rgba.getpixel((0, 0))[:3])
    span = max(1, bg, 255 - bg)
    r, g, b, _ = rgba.split()
    bright = Image.merge("RGB", (r, g, b)).convert("RGB")
    px = bright.load()
    w, h = bright.size
    out = Image.new("L", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            m = max(px[x, y])
            op[x, y] = min(255, abs(m - bg) * 255 // span)
    return out


def trim_box(alpha, threshold):
    return alpha.point(lambda v: 255 if v >= threshold else 0).getbbox()


def c_array(data):
    return "\n".join("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ","
                     for i in range(0, len(data), 16))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("name")
    ap.add_argument("--a8", action="store_true",
                    help="alpha-only mask, coloured on the device")
    ap.add_argument("--max", nargs=2, type=int, metavar=("W", "H"))
    ap.add_argument("--trim", action="store_true",
                    help="crop to the visible shape first")
    ap.add_argument("--bold", type=int, default=0, metavar="PX",
                    help="--a8 only: thicken lines by PX source pixels before "
                         "shrinking, for thin-outline icons that would "
                         "otherwise end up under a pixel wide")
    a = ap.parse_args()

    if not a.name.isidentifier():
        sys.exit(f"name must be a C identifier: {a.name!r}")

    src = Image.open(a.src)
    img = shape_mask(src) if a.a8 else src.convert("RGBA")
    if a.bold:
        if not a.a8:
            sys.exit("--bold only applies to --a8 masks")
        # A max filter grows every stroke by `bold` px on each side. Done at
        # source resolution, before the resize, so the result is a thicker
        # line rather than a blurrier one.
        from PIL import ImageFilter
        img = img.filter(ImageFilter.MaxFilter(2 * a.bold + 1))
    if a.trim:
        # A mask derived from brightness carries compression noise (the webp
        # coolant icon), so it trims at TRIM_THRESHOLD. A picture's own alpha
        # is trimmed at anything non-zero, as it always was: the car's faint
        # outer shading counts, and changing that would rescale the car the
        # tyre layout was measured against.
        box = trim_box(img, TRIM_THRESHOLD) if a.a8 else trim_box(img.getchannel("A"), 1)
        if box:
            img = img.crop(box)
    if a.max:
        img.thumbnail((a.max[0], a.max[1]), Image.LANCZOS)
    w, h = img.size

    if a.a8:
        data = img.tobytes()                    # mode L: one byte per pixel
        cf, stride, kind = "LV_COLOR_FORMAT_A8", w, "A8"
    else:
        px = img.load()
        rgb = bytearray()
        alpha = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b, al = px[x, y]
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                rgb += bytes((v & 0xFF, v >> 8))   # little-endian, as the ESP32 stores it
                alpha.append(al)
        data = bytes(rgb + alpha)
        cf, stride, kind = "LV_COLOR_FORMAT_RGB565A8", w * 2, "RGB565A8"

    src_name = os.path.basename(a.src)
    with open(a.out, "w") as f:
        f.write(f"""// GENERATED by scripts/png2lvgl.py from {src_name} - do not edit.
// {w}x{h} {kind} ({len(data)} bytes). Re-run the script after changing the source.
#include "lvgl.h"

static const uint8_t {a.name}_map[] = {{
{c_array(data)}
}};

const lv_image_dsc_t {a.name} = {{
  .header = {{
    .magic  = LV_IMAGE_HEADER_MAGIC,
    .cf     = {cf},
    .flags  = 0,
    .w      = {w},
    .h      = {h},
    .stride = {stride},
  }},
  .data_size = sizeof({a.name}_map),
  .data      = {a.name}_map,
}};
""")
    print(f"{a.out}: {w}x{h} {kind}, {len(data)} bytes")


if __name__ == "__main__":
    main()
