#!/usr/bin/env python3
"""Build and run the CYD host-preview harness, then convert every
<name>.raw (RGB565 LE, 320x240) in tests/preview/out to a PNG.

Usage: python3 tests/preview/run_preview.py [--nopng]
Exit code: the harness's (0 = no problems)."""
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
ROOT = HERE.parents[2]          # ESP32-CYD-SP500/
TESTS = ROOT / "tests"
MOCK = HERE.parent / "mock"
LIB = ROOT / ".pio/libdeps/cyd/TFT_eSPI"
OUT = HERE.parent / "out"

SRCS = [
    HERE.parent / "preview.cpp",
    ROOT / "src/DisplayUI.cpp",
    ROOT / "src/core/AppState.cpp",
    ROOT / "src/core/FlatJson.cpp",
    ROOT / "src/core/Format.cpp",
    ROOT / "src/core/JsonWriter.cpp",
    ROOT / "src/core/MarketClock.cpp",
    ROOT / "src/finance/ChartData.cpp",
    ROOT / "src/finance/FinancialCalculator.cpp",
    ROOT / "src/finance/JsonStream.cpp",
    ROOT / "src/finance/Portfolio.cpp",
    ROOT / "src/finance/SaleEngine.cpp",
    ROOT / "src/finance/YahooParser.cpp",
]

def main():
    if not LIB.exists():
        sys.exit(f"TFT_eSPI library not found at {LIB} (run `pio run` once first)")
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = ["g++", "-std=c++17", "-O1", "-g", "-w",
           f"-I{MOCK}", f"-I{ROOT}/src", f"-I{ROOT}/include", f"-I{LIB}"]
    cmd += [str(s) for s in SRCS] + ["-o", str(HERE.parent / "preview"), "-lm"]
    print("compiling...")
    subprocess.run(cmd, check=True)
    print("running...")
    rc = subprocess.run([str(HERE.parent / "preview"), str(TESTS)]).returncode
    if "--nopng" not in sys.argv:
        try:
            from PIL import Image
        except ImportError:
            print("PIL missing: .raw files only (pip install pillow for PNGs)")
            return rc
        for raw in sorted(OUT.glob("*.raw")):
            data = raw.read_bytes()
            if len(data) != 320 * 240 * 2:
                continue
            img = Image.new("RGB", (320, 240))
            px = img.load()
            for i in range(320 * 240):
                v = struct.unpack_from("<H", data, i * 2)[0]
                r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
                px[i % 320, i // 320] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
            img.save(raw.with_suffix(".png"))
        print(f"{len(list(OUT.glob('*.png')))} PNGs in {OUT}")
    return rc

if __name__ == "__main__":
    sys.exit(main())
