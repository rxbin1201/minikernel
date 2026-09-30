#!/usr/bin/env python3
"""Erzeugt Kernel/font_ext.c: die Zeichen ueber ASCII (Latin-1, Latin Erweitert-A, Griechisch, Kyrillisch, Satzzeichen,
Euro, Pfeile, Rahmenzeichen) aus einer PSF1-Konsolenschrift mit Unicode-Tabelle (8x16).

Aufruf: python3 gen_font_ext.py [/usr/share/consolefonts/Uni3-Terminus16.psf.gz]

Quelle: Terminus (Uni3-Terminus16.psf), SIL Open Font License 1.1, (c) Dimitar Toshkov Zhekov.
Zeichen, die die Schrift nicht kennt, bekommen das Glyph von '?'.
"""
import gzip
import struct
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/consolefonts/Uni3-Terminus16.psf.gz"
data = gzip.open(src).read()
assert data[0:2] == b"\x36\x04", "keine PSF1-Datei"
mode, height = data[2], data[3]
assert height == 16 and mode & 2, "erwarte 8x16 mit Unicode-Tabelle"
n_glyphs = 512 if mode & 1 else 256
glyphs = [data[4 + i * 16:4 + (i + 1) * 16] for i in range(n_glyphs)]

# Unicode-Tabelle: je Glyph eine Folge von 16-Bit-Codepunkten, 0xFFFF beendet, 0xFFFE leitet eine Kombination ein
table = {}
pos = 4 + n_glyphs * 16
for gi in range(n_glyphs):
    combining = False
    while pos + 2 <= len(data):
        (v,) = struct.unpack_from("<H", data, pos)
        pos += 2
        if v == 0xFFFF:
            break
        if v == 0xFFFE:
            combining = True
            continue
        if not combining and v not in table:
            table[v] = gi

RANGES = [
    (0x00A0, 0x0180),  # Latin-1 (Umlaute, Akzente, Symbole) und Latin Erweitert-A
    (0x0391, 0x03CA),  # Griechisch
    (0x0401, 0x0460),  # Kyrillisch
    (0x2010, 0x2028),  # Striche, Anfuehrungszeichen, Punkte
    (0x20AC, 0x20AD),  # Euro
    (0x2190, 0x2196),  # Pfeile
    (0x2500, 0x25A0),  # Rahmenzeichen, Bloecke
]
fallback = glyphs[table[ord("?")]]

out = ['#include "font.h"', "",
       "/* Erzeugt mit gen_font_ext.py aus Uni3-Terminus16.psf (SIL Open Font License 1.1, (c) Dimitar Toshkov Zhekov). */",
       "const FontRange font_ext_ranges[] = {"]
offset = 0
blob = []
missing = 0
for lo, hi in RANGES:
    out.append("    {0x%04X, 0x%04X, %d}," % (lo, hi, offset))
    for cp in range(lo, hi):
        if cp in table:
            blob.append(glyphs[table[cp]])
        elif cp == 0x00A0:
            blob.append(bytes(16))
        else:
            blob.append(fallback)
            missing += 1
        offset += 1
out.append("};")
out.append("const int font_ext_range_count = %d;" % len(RANGES))
out.append("const unsigned char font_ext_data[%d * 16] = {" % offset)
for i, g in enumerate(blob):
    out.append("    " + ", ".join("0x%02X" % b for b in g) + ",")
out.append("};")

with open("Kernel/font_ext.c", "w") as f:
    f.write("\n".join(out) + "\n")
print("Kernel/font_ext.c geschrieben: %d Zeichen (%d ohne Glyph -> '?')" % (offset, missing))
