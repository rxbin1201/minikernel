#!/usr/bin/env python3
"""Erzeugt die Symboltabelle des Kernels (C-Quelltext) fuer lib/ksyms.c, damit Backtraces Funktionsnamen zeigen.

Aufruf: mksyms.py <kernel.elf> <ausgabe.c>     Funktionen aus nm (nach Adresse sortiert)
        mksyms.py --empty <ausgabe.c>         leere Tabelle (erster Link-Durchgang)

Die Tabelle liegt nur in .rodata, die hinter .text gelinkt wird: die Funktionsadressen aendern sich durch sie nicht.
"""
import subprocess
import sys


def main():
    args = sys.argv[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    syms = []
    if args[0] != "--empty":
        out = subprocess.check_output(["nm", "-n", "--defined-only", args[0]], text=True)
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "Tt" and not parts[2].startswith("."):
                syms.append((int(parts[0], 16), parts[2]))

    names, offsets, pos = [], [], 0
    for _, name in syms:
        offsets.append(pos)
        names.append(name)
        pos += len(name) + 1

    lines = ['#include "lib/ksyms.h"', "", "/* Erzeugt von tools/mksyms.py, nicht von Hand aendern */", ""]
    lines.append("const unsigned ksym_count = %d;" % len(syms))
    lines.append("const KSym ksym_table[] = {")
    for (addr, _), off in zip(syms, offsets):
        lines.append("    {%#x, %d}," % (addr, off))
    if not syms:
        lines.append("    {0, 0},")
    lines.append("};")
    lines.append("const char ksym_names[] =")
    for n in names:
        lines.append('    "%s\\0"' % n)
    lines.append('    "";')
    with open(args[1], "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
