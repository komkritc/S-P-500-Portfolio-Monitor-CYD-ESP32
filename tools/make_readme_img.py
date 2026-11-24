#!/usr/bin/env python3
"""Build the README screenshots from the LCD preview harness output.

    python3 tests/preview/run_preview.py     # writes tests/preview/out/*.png
    python3 tools/make_readme_img.py         # writes docs/img/*.png

Every image here is the real firmware drawing code run on the host against a
mock TFT_eSPI (tests/preview/mock/TFT_eSPI.h) with sample portfolio data - no
camera, no real holdings, nothing staged. Nearest-neighbour 2x keeps the
pixel grid honest while making the 6 px text readable in a README.
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "tests", "preview", "out")
DST = os.path.join(HERE, "..", "docs", "img")
SCALE = 2
GAP = 8
BG = (24, 26, 32)          # the app background, so the gutters match the UI

# name in docs/img -> frame produced by the preview harness
SINGLES = {
    "lcd-dashboard.png": "normal_p1_dashboard.png",
    "lcd-chart-down.png": "normal_p2_chart_1d.png",
    "lcd-chart-up.png": "up_p2_chart_5d.png",
    "lcd-positions.png": "many_p3_positions_w0.png",
    "lcd-realised.png": "realised_p4_realised.png",
    "lcd-empty.png": "empty_p1_dashboard.png",
    "lcd-offline.png": "nodata_p1_dashboard.png",
    "boot-banner.png": "boot_banner.png",
}

# 2x2 contact sheet of the four rotating screens, reading order = nav order
GRID = [
    "normal_p1_dashboard.png",
    "normal_p2_chart_1d.png",
    "many_p3_positions_w0.png",
    "realised_p4_realised.png",
]


def load(name):
    p = os.path.join(SRC, name)
    if not os.path.exists(p):
        sys.exit(f"missing {p} - run tests/preview/run_preview.py first")
    im = Image.open(p).convert("RGB")
    return im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)


def main():
    os.makedirs(DST, exist_ok=True)
    for out, src in SINGLES.items():
        im = load(src)
        im.save(os.path.join(DST, out), optimize=True)
        print(f"{out:24} {im.width}x{im.height}")

    cell = [load(n) for n in GRID]
    w, h = cell[0].size
    sheet = Image.new("RGB", (w * 2 + GAP * 3, h * 2 + GAP * 3), BG)
    for i, im in enumerate(cell):
        sheet.paste(im, (GAP + (i % 2) * (w + GAP), GAP + (i // 2) * (h + GAP)))
    sheet.save(os.path.join(DST, "lcd-pages.png"), optimize=True)
    print(f"{'lcd-pages.png':24} {sheet.width}x{sheet.height}")


if __name__ == "__main__":
    main()
