#!/usr/bin/env python3
"""Erzeugt Kernel/console/font.c aus einer PSF1-Konsolenschrift (8x16, ASCII 0-127).

Aufruf: python3 gen_font.py [/usr/share/consolefonts/Lat15-Terminus16.psf.gz]
"""
import gzip
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/consolefonts/Lat15-Terminus16.psf.gz"
data = gzip.open(src).read()
assert data[0:2] == b"\x36\x04", "keine PSF1-Datei"
assert data[3] == 16, "erwarte 8x16"
glyphs = data[4:4 + 128 * 16]

lines = [
    '#include "font.h"',
    "",
    "/* 8x16-Bitmapfont, ASCII 0-127. Quelle: Terminus (Lat15-Terminus16.psf), SIL Open Font License 1.1,",
    " * (c) Dimitar Toshkov Zhekov. Pro Zeichen 16 Zeilen, Bit 7 = linkestes Pixel. Erzeugt mit gen_font.py. */",
    "const unsigned char font8x16[128 * 16] = {",
]
for i in range(128):
    row = glyphs[i * 16:(i + 1) * 16]
    lines.append("    " + ", ".join("0x%02X" % b for b in row) + ",  /* %d */" % i)
lines.append("};")

with open("Kernel/console/font.c", "w") as f:
    f.write("\n".join(lines) + "\n")
print("Kernel/console/font.c geschrieben")
