#!/usr/bin/env python3
"""Prueft ein FAT12/16/32- oder exFAT-Image auf Konsistenz (Ersatz fuer fsck.fat/fsck.exfat, die hier fehlen).

Aufruf: fatcheck.py <image> [--part N]     (ohne --part: MBR-Partition 1, falls das Image einen MBR hat, sonst ganzes Image)

Geprueft wird: Bootsektor, FAT-Kopien, Verzeichnisbaum (Kurz- und LFN-Eintraege samt Pruefsummen, '.'/'..'),
Clusterketten (Schleifen, Ueberschneidungen, Groesse passt zur Kettenlaenge), verlorene Cluster, FSInfo (FAT32);
bei exFAT: Pruefsumme des Boot-Bereichs, Eintragssaetze (Pruefsumme, Namenshash, Namenslaenge), NoFatChain,
Belegungs-Bitmap gegen die tatsaechlich benutzten Cluster.
Ausgabe: Zusammenfassung; Exit-Code 1 bei Fehlern.
"""
import struct
import sys

SECTOR = 512
errors = []


def err(msg):
    errors.append(msg)


class Img:
    def __init__(self, path, offset):
        self.f = open(path, "rb")
        self.offset = offset

    def read(self, lba, count=1):
        self.f.seek((self.offset + lba) * SECTOR)
        return self.f.read(count * SECTOR)


def le16(b, o): return struct.unpack_from("<H", b, o)[0]
def le32(b, o): return struct.unpack_from("<I", b, o)[0]
def le64(b, o): return struct.unpack_from("<Q", b, o)[0]


# ------------------------------------------------------------------ FAT12/16/32

def check_fat(img, bs):
    spc, reserved, nfats = bs[13], le16(bs, 14), bs[16]
    root_entries = le16(bs, 17)
    fatsz = le16(bs, 22) or le32(bs, 36)
    total = le16(bs, 19) or le32(bs, 32)
    root_sectors = (root_entries * 32 + 511) // 512
    data_lba = reserved + nfats * fatsz + root_sectors
    clusters = (total - data_lba) // spc
    bits = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    cb = spc * SECTOR
    print(f"FAT{bits}: {clusters} Cluster a {cb} Bytes, {nfats} FAT(s) a {fatsz} Sektoren")

    fats = [img.read(reserved + i * fatsz, fatsz) for i in range(nfats)]
    for i in range(1, nfats):
        if fats[i] != fats[0]:
            err(f"FAT {i} weicht von FAT 0 ab")
    fat = fats[0]

    def get(c):
        if bits == 32:
            return le32(fat, c * 4) & 0x0FFFFFFF
        if bits == 16:
            return le16(fat, c * 2)
        o = c + c // 2
        v = fat[o] | (fat[o + 1] << 8)
        return v >> 4 if c & 1 else v & 0xFFF

    eoc = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[bits]
    owner = {}

    def chain(first, what):
        out = []
        c = first
        while True:
            if c < 2 or c >= clusters + 2:
                err(f"{what}: ungueltiger Cluster {c}")
                return out
            if c in owner:
                err(f"{what}: Cluster {c} gehoert auch zu {owner[c]}")
                return out
            owner[c] = what
            out.append(c)
            n = get(c)
            if n >= eoc:
                return out
            if n == 0:
                err(f"{what}: Kette endet in freiem Cluster (nach {c})")
                return out
            c = n
            if len(out) > clusters:
                err(f"{what}: Schleife")
                return out

    def cluster_data(c):
        return img.read(data_lba + (c - 2) * spc, spc)

    def dir_bytes(first):
        if first == 0 and bits != 32:
            return img.read(reserved + nfats * fatsz, root_sectors)
        return b"".join(cluster_data(c) for c in chain(first if first else le32(bs, 44), f"Verz.{first}"))

    def lfn_sum(name11):
        s = 0
        for ch in name11:
            s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
        return s

    stats = {"files": 0, "dirs": 0}

    def walk(first, path, parent):
        raw = dir_bytes(first) if first != 0 or bits != 32 else dir_bytes(le32(bs, 44))
        lfn = []
        seen_end = False
        names = set()
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                seen_end = True
                continue
            if seen_end:
                err(f"{path}: Eintrag hinter dem Ende-Marker")
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr == 0x0F:
                lfn.append(e)
                continue
            if attr & 0x08:
                lfn = []
                continue
            name11 = e[:11]
            first_c = (le16(e, 20) << 16) | le16(e, 26)
            size = le32(e, 28)
            disp = name11[:8].decode("latin1").rstrip() + ("." + name11[8:].decode("latin1").rstrip() if name11[8:].strip() else "")
            if lfn:
                if lfn[0][0] & 0x40 == 0:
                    err(f"{path}/{disp}: LFN ohne Start-Flag")
                n = len(lfn)
                for k, le in enumerate(lfn):
                    want = (n - k) | (0x40 if k == 0 else 0)
                    if le[0] != want:
                        err(f"{path}/{disp}: LFN-Folge falsch")
                    if le[13] != lfn_sum(name11):
                        err(f"{path}/{disp}: LFN-Pruefsumme falsch")
                chars = b""
                for le in reversed(lfn):
                    chars += le[1:11] + le[14:26] + le[28:32]
                text = chars.decode("utf-16-le").split("\x00")[0]
                shown = text
            else:
                shown = disp
            lfn = []
            if disp in (".", ".."):
                if disp == ".":
                    if first_c != first and not (first == 0):
                        err(f"{path}: '.' zeigt auf {first_c}, erwartet {first}")
                else:
                    if first_c != parent:
                        err(f"{path}: '..' zeigt auf {first_c}, erwartet {parent}")
                continue
            if shown.lower() in names:
                err(f"{path}: doppelter Name {shown}")
            names.add(shown.lower())
            full = f"{path}/{shown}"
            if attr & 0x10:
                stats["dirs"] += 1
                if first_c == 0:
                    err(f"{full}: Verzeichnis ohne Cluster")
                    continue
                walk(first_c, full, 0 if path == "" else first)
            else:
                stats["files"] += 1
                if size == 0:
                    if first_c != 0:
                        err(f"{full}: Groesse 0, aber Startcluster {first_c}")
                    continue
                ch = chain(first_c, full)
                need = (size + cb - 1) // cb
                if len(ch) != need:
                    err(f"{full}: Groesse {size} braucht {need} Cluster, Kette hat {len(ch)}")

    # Wurzel
    if bits == 32:
        root_first = le32(bs, 44)
        root_chain = chain(root_first, "Wurzel")
        raw_root_chain = root_chain
        # walk() ruft chain() erneut auf -> Besitzer der Wurzel vorher freigeben
        for c in raw_root_chain:
            del owner[c]
        walk(root_first, "", 0)
    else:
        walk(0, "", 0)

    used = set(owner)
    lost = [c for c in range(2, clusters + 2) if get(c) != 0 and c not in used and get(c) != 0xFF7 and get(c) != 0xFFF7]
    if lost:
        err(f"{len(lost)} verlorene Cluster (belegt, aber von keiner Datei benutzt), z.B. {lost[:5]}")
    free = sum(1 for c in range(2, clusters + 2) if get(c) == 0)
    print(f"{stats['files']} Dateien, {stats['dirs']} Verzeichnisse, {len(used)} Cluster belegt, {free} frei")

    if bits == 32:
        fsinfo = le16(bs, 48)
        if fsinfo:
            fi = img.read(fsinfo)
            if le32(fi, 0) == 0x41615252 and le32(fi, 484) == 0x61417272:
                fc = le32(fi, 488)
                if fc != 0xFFFFFFFF and fc != free:
                    err(f"FSInfo: {fc} freie Cluster, tatsaechlich {free}")
        v1 = get(1)
        if not v1 & 0x08000000:
            print("Hinweis: Dirty-Flag gesetzt")


# ------------------------------------------------------------------ exFAT

def x_checksum16(entries):
    chk = 0
    for idx, e in enumerate(entries):
        for j, b in enumerate(e):
            if idx == 0 and j in (2, 3):
                continue
            chk = ((((chk & 1) << 15) | (chk >> 1)) + b) & 0xFFFF
    return chk


def x_hash(name):
    h = 0
    up = "".join(c.upper() if len(c.upper()) == 1 else c for c in name)  # wie die Upcase-Tabelle: 1 Zeichen -> 1 Zeichen
    for ch in up.encode("utf-16-le"):
        h = ((((h & 1) << 15) | (h >> 1)) + ch) & 0xFFFF
    return h


def check_exfat(img, bs):
    fat_off, fat_len, heap, ccount, root = le32(bs, 80), le32(bs, 84), le32(bs, 88), le32(bs, 92), le32(bs, 96)
    spc = 1 << bs[109]
    cb = spc * SECTOR
    flags = le16(bs, 106)
    print(f"exFAT: {ccount} Cluster a {cb} Bytes, Flags {flags:#x}")

    # Boot-Bereich: Pruefsumme
    region = img.read(0, 11)
    chk = 0
    for i, b in enumerate(region):
        if i in (106, 107, 112):
            continue
        chk = ((((chk & 1) << 31) | (chk >> 1)) + b) & 0xFFFFFFFF
    stored = img.read(11)
    if any(le32(stored, k) != chk for k in range(0, 512, 4)):
        err("Boot-Bereich: Pruefsumme falsch")
    if img.read(12, 12) != img.read(0, 12):
        err("Boot-Bereich: Sicherung weicht ab")

    fat = img.read(fat_off, fat_len)

    def fget(c):
        return le32(fat, c * 4)

    owner = {}

    def use(c, what):
        if c < 2 or c >= ccount + 2:
            err(f"{what}: ungueltiger Cluster {c}")
            return False
        if c in owner:
            err(f"{what}: Cluster {c} gehoert auch zu {owner[c]}")
            return False
        owner[c] = what
        return True

    def clusters_of(first, size, contig, what, is_root=False):
        out = []
        if contig:
            n = (size + cb - 1) // cb
            for k in range(n):
                if use(first + k, what):
                    out.append(first + k)
            return out
        c = first
        while True:
            if not use(c, what):
                return out
            out.append(c)
            n = fget(c)
            if n >= 0xFFFFFFF8:
                return out
            if n == 0 or n == 0xFFFFFFF7:
                err(f"{what}: Kette endet ungueltig nach {c}")
                return out
            c = n

    def data(cl):
        return b"".join(img.read(heap + (c - 2) * spc, spc) for c in cl)

    stats = {"files": 0, "dirs": 0}
    bitmap_info = {}

    def walk(cl, path, is_root):
        raw = data(cl)
        i = 0
        names = set()
        while i + 32 <= len(raw):
            t = raw[i]
            if t == 0:
                break
            if t & 0x80 == 0:  # geloeschter Eintrag: ganzer Satz uebersprungen
                i += 32
                continue
            if t == 0x83:
                i += 32
                continue
            if t == 0x81 and is_root:
                bitmap_info["first"] = le32(raw, i + 20)
                bitmap_info["bytes"] = le64(raw, i + 24)
                i += 32
                continue
            if t == 0x82 and is_root:
                up_first, up_len = le32(raw, i + 20), le64(raw, i + 24)
                clusters_of(up_first, up_len, True, "Upcase-Tabelle")
                i += 32
                continue
            if t != 0x85:
                err(f"{path}: unbekannter Eintragstyp {t:#x} bei {i}")
                i += 32
                continue
            sec = raw[i + 1]
            entries = [raw[i + k * 32:i + k * 32 + 32] for k in range(sec + 1)]
            i += 32 * (sec + 1)
            if len(entries) != sec + 1 or entries[1][0] != 0xC0:
                err(f"{path}: Eintragssatz ohne Stream-Eintrag")
                continue
            if le16(entries[0], 2) != x_checksum16(entries):
                err(f"{path}: Pruefsumme des Eintragssatzes falsch")
            e1 = entries[1]
            nlen = e1[3]
            name = ""
            for ne in entries[2:]:
                if ne[0] != 0xC1:
                    err(f"{path}: Namenseintrag hat Typ {ne[0]:#x}")
                    continue
                name += ne[2:32].decode("utf-16-le")
            name = name[:nlen]
            if len(name) != nlen or (len(entries) - 2) != (nlen + 14) // 15:
                err(f"{path}/{name}: Namenslaenge/Zahl der Namenseintraege passt nicht")
            if le16(e1, 4) != x_hash(name):
                err(f"{path}/{name}: Namenshash falsch")
            if name.lower() in names:
                err(f"{path}: doppelter Name {name}")
            names.add(name.lower())
            attr = le16(entries[0], 4)
            contig = bool(e1[1] & 2)
            valid, first, size = le64(e1, 8), le32(e1, 20), le64(e1, 24)
            full = f"{path}/{name}"
            if valid > size:
                err(f"{full}: gueltige Laenge {valid} > Groesse {size}")
            if size == 0:
                if first != 0:
                    err(f"{full}: Groesse 0, aber Startcluster {first}")
                if attr & 0x10:
                    err(f"{full}: Verzeichnis mit Groesse 0")
                stats["files"] += 1
                continue
            cl = clusters_of(first, size, contig, full)
            if not contig and len(cl) != (size + cb - 1) // cb:
                err(f"{full}: Groesse {size} braucht {(size + cb - 1) // cb} Cluster, Kette hat {len(cl)}")
            if attr & 0x10:
                stats["dirs"] += 1
                walk(cl, full, False)
            else:
                stats["files"] += 1

    root_cl = clusters_of(root, 0, False, "Wurzel")
    walk(root_cl, "", True)

    # Bitmap
    if "first" not in bitmap_info:
        err("Keine Allocation-Bitmap gefunden")
    else:
        bm_first, bm_bytes = bitmap_info["first"], bitmap_info["bytes"]
        n_bm = (bm_bytes + cb - 1) // cb
        for k in range(n_bm):
            use(bm_first + k, "Bitmap")
        bm = data([bm_first + k for k in range(n_bm)])
        marked = {c for c in range(2, ccount + 2) if bm[(c - 2) // 8] >> ((c - 2) % 8) & 1}
        used = set(owner)
        if marked - used:
            err(f"Bitmap: {len(marked - used)} Cluster als belegt markiert, aber ungenutzt, z.B. {sorted(marked - used)[:5]}")
        if used - marked:
            err(f"Bitmap: {len(used - marked)} benutzte Cluster nicht markiert, z.B. {sorted(used - marked)[:5]}")
        print(f"{stats['files']} Dateien, {stats['dirs']} Verzeichnisse, {len(used)} Cluster belegt, {ccount - len(marked)} frei")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    part = None
    if "--part" in args:
        part = int(args[args.index("--part") + 1])
    img = Img(path, 0)
    first = img.read(0)
    offset = 0
    if part is not None or (first[510:512] == b"\x55\xAA" and first[3:11] not in (b"EXFAT   ",) and
                            first[0] not in (0xEB, 0xE9) and any(first[446 + 16 * i + 4] for i in range(4))):
        p = (part or 1) - 1
        offset = struct.unpack_from("<I", first, 446 + 16 * p + 8)[0]
        img = Img(path, offset)
    bs = img.read(0)
    if bs[510:512] != b"\x55\xAA":
        sys.exit("kein gueltiger Bootsektor (55AA fehlt)")
    if bs[3:11] == b"EXFAT   ":
        check_exfat(img, bs)
    else:
        check_fat(img, bs)
    if errors:
        print(f"{len(errors)} FEHLER:")
        for e in errors[:40]:
            print("  -", e)
        sys.exit(1)
    print("OK: keine Fehler gefunden")


if __name__ == "__main__":
    main()
