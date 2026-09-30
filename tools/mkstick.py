#!/usr/bin/env python3
"""Erzeugt ein FAT12- oder FAT16-Image wie von einem gewoehnlichen USB-Stick, zum Testen des Lesezugriffs.

Aufruf: mkstick.py <datei> [--fat 12|16|exfat] [--mbr] [--dirty]

  --fat 12  (Standard) 2 MiB, 1 Sektor pro Cluster
  --fat 16  32 MiB, 4 Sektoren pro Cluster
  --fat exfat  16 MiB exFAT (zusammenhaengende und fragmentierte Dateien, gueltige Laenge < Groesse, Umlaut-Name)
  --mbr     MBR mit einer Partition ab Sektor 2048 statt eines Volumes ueber das ganze Image

Inhalt (lange Dateinamen, Unterverzeichnis, Kleinschreibung per NT-Flag, geloeschter Eintrag mit LFN-Resten):
  HELLO.TXT, readme.txt (als README.TXT mit Klein-Flags), "Lange Datei mit Leerzeichen.txt",
  "Bilder und Notizen/Notiz Nummer eins.txt", BIG.BIN (20000 Bytes Muster (i*13+5)&255).
Das Label ist USBSTICK (im Wurzelverzeichnis, im BPB steht "NO NAME", wie bei Windows). Aufbau nach Microsofts
FAT-Spezifikation.
"""
import struct
import sys

SECTOR = 512
DATE = (2026 - 1980) << 9 | 1 << 5 | 1
PART_START = 2048
DIRTY = False  # --dirty: Volume als nicht sauber getrennt markieren (FAT16/exFAT) und DIRTY.MRK anlegen


def big_pattern(n):
    return bytes((i * 13 + 5) & 0xFF for i in range(n))


def lfn_checksum(name11):
    s = 0
    for c in name11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(long_name, name11):
    """LFN-Eintraege in Verzeichnisreihenfolge (hoechster Teil zuerst)."""
    chars = [ord(c) for c in long_name]
    chars = chars + [0] if len(chars) % 13 else chars
    while len(chars) % 13:
        chars.append(0xFFFF)
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    cs = lfn_checksum(name11)
    for idx, p in reversed(list(enumerate(parts, 1))):
        order = idx | (0x40 if idx == len(parts) else 0)
        e = struct.pack("<B5HBBB6HH2H", order, *p[0:5], 0x0F, 0, cs, *p[5:11], 0, *p[11:13])
        out.append(e)
    return out


def dirent(name11, attr, cluster, size, ntres=0):
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, ntres, 0, 0, DATE, DATE, cluster >> 16, 0, DATE,
                       cluster & 0xFFFF, size)


class Fat:
    def __init__(self, bits, sectors):
        self.bits = bits
        self.total = sectors
        self.spc = 1 if bits == 12 else 4
        self.reserved = 1
        self.nfats = 2
        self.root_entries = 224 if bits == 12 else 512
        self.root_sectors = self.root_entries * 32 // SECTOR
        fatsz = 1
        while True:
            data = self.total - self.reserved - self.nfats * fatsz - self.root_sectors
            self.clusters = data // self.spc
            need = ((self.clusters + 2) * bits // 8 + 1 + SECTOR - 1) // SECTOR
            if need <= fatsz:
                break
            fatsz = need
        self.fatsz = fatsz
        want = (4085 if bits == 12 else 65525)
        assert (self.clusters < want) if bits == 12 else (4085 <= self.clusters < want), self.clusters
        self.fat_lba = self.reserved
        self.root_lba = self.reserved + self.nfats * fatsz
        self.data_lba = self.root_lba + self.root_sectors
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFF8 if bits == 12 else 0xFFF8
        self.fat[1] = 0xFFF if bits == 12 else 0xFFFF
        self.eoc = 0xFFF if bits == 12 else 0xFFFF
        self.image = bytearray(sectors * SECTOR)
        self.next_free = 2

    def alloc(self, count):
        cl = list(range(self.next_free, self.next_free + count))
        self.next_free += count
        assert self.next_free <= self.clusters + 2
        for a, b in zip(cl, cl[1:]):
            self.fat[a] = b
        self.fat[cl[-1]] = self.eoc
        return cl

    def write_data(self, clusters, data):
        cb = self.spc * SECTOR
        for i, c in enumerate(clusters):
            off = (self.data_lba + (c - 2) * self.spc) * SECTOR
            chunk = data[i * cb:(i + 1) * cb]
            self.image[off:off + len(chunk)] = chunk

    def add_file(self, data):
        if not data:
            return 0
        cb = self.spc * SECTOR
        cl = self.alloc((len(data) + cb - 1) // cb)
        self.write_data(cl, data)
        return cl[0]

    def make_dir(self, entries, parent_cluster):
        """entries: Liste von Roh-Eintraegen (ohne . und ..). Liefert den Startcluster."""
        cb = self.spc * SECTOR
        n = (len(entries) + 2) * 32
        cl = self.alloc((n + cb - 1) // cb)
        dots = dirent(b".          ", 0x10, cl[0], 0) + dirent(b"..         ", 0x10, parent_cluster, 0)
        self.write_data(cl, dots + b"".join(entries))
        return cl[0]

    def finish(self, root_entries, label, bpb_label=b"NO NAME    "):
        assert len(root_entries) + 1 <= self.root_entries
        raw = dirent(label.ljust(11).encode(), 0x08, 0, 0) + b"".join(root_entries)
        off = self.root_lba * SECTOR
        self.image[off:off + len(raw)] = raw

        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"MSDOS5.0"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, self.spc, self.reserved, self.nfats, self.root_entries,
                         self.total if self.total < 65536 else 0, 0xF8, self.fatsz, 32, 64, PART_START if self.mbr else 0,
                         0 if self.total < 65536 else self.total)
        struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, 0x1234ABCD, bpb_label, b"FAT12   " if self.bits == 12 else b"FAT16   ")
        bs[510:512] = b"\x55\xAA"
        self.image[0:SECTOR] = bs

        fat = bytearray(self.fatsz * SECTOR)
        if self.bits == 16:
            for i, v in enumerate(self.fat):
                struct.pack_into("<H", fat, i * 2, v)
        else:
            for i in range(0, len(self.fat), 2):
                a = self.fat[i]
                b = self.fat[i + 1] if i + 1 < len(self.fat) else 0
                o = i * 3 // 2
                fat[o] = a & 0xFF
                fat[o + 1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4)
                fat[o + 2] = b >> 4
        for k in range(self.nfats):
            o = (self.fat_lba + k * self.fatsz) * SECTOR
            self.image[o:o + len(fat)] = fat


def long_entry(fat, long_name, name11, attr, cluster, size, ntres=0):
    return lfn_entries(long_name, name11) + [dirent(name11, attr, cluster, size, ntres)]


def build(bits, mbr):
    sectors = 4096 - 1200 if bits == 12 else 65536
    if bits == 12:
        sectors = 4000  # ~2 MiB, 3990 Cluster (<4085)
    f = Fat(bits, sectors)
    f.mbr = mbr

    hello = b"Hallo vom USB-Stick!\n"
    lang = b"Inhalt der langen Datei.\nZweite Zeile.\n"
    notiz = b"Notiz im Unterverzeichnis mit langem Namen.\n"
    readme = b"Kleinbuchstaben ueber das NT-Flag.\n"
    big = big_pattern(20000)

    root = []
    root.append(dirent(b"HELLO   TXT", 0x20, f.add_file(hello), len(hello)))
    root.append(dirent(b"README  TXT", 0x20, f.add_file(readme), len(readme), ntres=0x18))
    root += long_entry(f, "Lange Datei mit Leerzeichen.txt", b"LANGED~1TXT", 0x20, f.add_file(lang), len(lang))

    # geloeschter Eintrag samt LFN-Resten (0xE5 im ersten Byte): darf nicht auftauchen
    dead = lfn_entries("Geloeschte Datei.txt", b"GELOES~1TXT") + [dirent(b"GELOES~1TXT", 0x20, 0, 0)]
    for e in dead:
        root.append(b"\xE5" + e[1:])

    sub = long_entry(f, "Notiz Nummer eins.txt", b"NOTIZN~1TXT", 0x20, f.add_file(notiz), len(notiz))
    sub_cluster = f.make_dir(sub, 0)
    root += long_entry(f, "Bilder und Notizen", b"BILDER~1   ", 0x10, sub_cluster, 0)
    root.append(dirent(b"BIG     BIN", 0x20, f.add_file(big), len(big)))

    if DIRTY:
        assert bits == 16, "--dirty gibt es nur bei FAT16 und exFAT"
        f.fat[1] = 0x7FFF
        root.append(dirent(b"DIRTY   MRK", 0x20, 0, 0))
    f.finish(root, "USBSTICK")
    return f


class ExFat:
    """exFAT (16 MiB, 512-Byte-Sektoren, 4 Sektoren pro Cluster) mit zusammenhaengenden und fragmentierten Dateien."""

    def __init__(self, part_offset):
        self.total = 32768
        self.spc_shift = 2
        self.spc = 4
        self.cb = self.spc * SECTOR
        self.fat_off = 32
        fat_len = 1
        while True:
            heap = self.fat_off + fat_len
            ccount = (self.total - heap) // self.spc
            need = ((ccount + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_len:
                break
            fat_len = need
        self.fat_len, self.heap, self.ccount = fat_len, heap, ccount
        self.image = bytearray(self.total * SECTOR)
        self.fat = [0] * (ccount + 2)
        self.fat[0] = 0xFFFFFFF8
        self.fat[1] = 0xFFFFFFFF
        self.next_free = 2
        self.gaps = set()  # absichtlich freigelassene Cluster zwischen den Teilen fragmentierter Dateien
        self.part_offset = part_offset

    def alloc(self, n):
        cl = list(range(self.next_free, self.next_free + n))
        self.next_free += n
        assert self.next_free <= self.ccount + 2
        return cl

    def write(self, clusters, data):
        for i, c in enumerate(clusters):
            off = (self.heap + (c - 2) * self.spc) * SECTOR
            chunk = data[i * self.cb:(i + 1) * self.cb]
            self.image[off:off + len(chunk)] = chunk

    def chain(self, cl):
        for a, b in zip(cl, cl[1:]):
            self.fat[a] = b
        self.fat[cl[-1]] = 0xFFFFFFFF

    def contiguous_file(self, data):
        if not data:
            return 0
        cl = self.alloc((len(data) + self.cb - 1) // self.cb)
        self.write(cl, data)
        return cl[0]

    def fragmented_file(self, data):
        """Cluster mit Luecken dazwischen, verkettet ueber die FAT."""
        n = (len(data) + self.cb - 1) // self.cb
        cl = []
        for _ in range(n):
            cl.append(self.alloc(1)[0])
            self.gaps.add(self.alloc(1)[0])  # Luecke: bleibt frei
        self.write(cl, data)
        self.chain(cl)
        return cl[0]


def x_hash(name):
    h = 0
    for ch in name.upper().encode("utf-16-le"):
        h = ((((h & 1) << 15) | (h >> 1)) + ch) & 0xFFFF
    return h


def x_entry_set(name, is_dir, first, size, valid, contig, deleted=False):
    units = name.encode("utf-16-le")
    chars = [struct.unpack_from("<H", units, i)[0] for i in range(0, len(units), 2)]
    names = [chars[i:i + 15] for i in range(0, len(chars), 15)]
    e0 = bytearray(32)
    e0[0] = 0x85
    e0[1] = 1 + len(names)
    struct.pack_into("<H", e0, 4, 0x10 if is_dir else 0x20)
    e1 = bytearray(32)
    e1[0] = 0xC0
    e1[1] = 0x01 | (0x02 if contig else 0)
    e1[3] = len(chars)
    struct.pack_into("<HxxQxxxxIQ", e1, 4, x_hash(name), valid, first, size)
    sets = [e0, e1]
    for part in names:
        e = bytearray(32)
        e[0] = 0xC1
        for i, c in enumerate(part):
            struct.pack_into("<H", e, 2 + i * 2, c)
        sets.append(e)
    chk = 0
    for idx, e in enumerate(sets):
        for j, b in enumerate(e):
            if idx == 0 and j in (2, 3):
                continue
            chk = ((((chk & 1) << 15) | (chk >> 1)) + b) & 0xFFFF
    struct.pack_into("<H", e0, 2, chk)
    if deleted:  # InUse-Bit loeschen
        for e in sets:
            e[0] &= 0x7F
    return b"".join(bytes(e) for e in sets)


def build_exfat(mbr):
    f = ExFat(PART_START if mbr else 0)

    bitmap_bytes = (f.ccount + 7) // 8
    bitmap_cl = f.alloc((bitmap_bytes + f.cb - 1) // f.cb)
    upcase = b"".join(struct.pack("<H", (c - 32) if ord("a") <= c <= ord("z") else c) for c in range(128))
    upcase_cl = f.alloc(1)
    f.write(upcase_cl, upcase)
    upsum = 0
    for b in upcase:
        upsum = ((((upsum & 1) << 31) | (upsum >> 1)) + b) & 0xFFFFFFFF
    root_cl = f.alloc(1)
    f.chain(root_cl)

    hello = b"Hallo vom USB-Stick!\n"
    lang = b"Inhalt der langen Datei.\nZweite Zeile.\n"
    notiz = b"Notiz im Unterverzeichnis mit langem Namen.\n"
    readme = b"Kleinbuchstaben bleiben erhalten.\n"
    big = big_pattern(20000)
    frag = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    sparse_disk = bytes((i * 5 + 1) & 0xFF for i in range(1000)) + b"\xAA" * 2000  # hinter 1000 Bytes: Altdaten

    sub_entries = x_entry_set("Notiz Nummer eins.txt", False, f.contiguous_file(notiz), len(notiz), len(notiz), True)
    sub_cl = f.alloc(1)
    f.write(sub_cl, sub_entries)

    root = bytearray()
    label = "USBSTICK".encode("utf-16-le")
    root += bytes([0x83, 8]) + label.ljust(22, b"\0") + bytes(8)
    root += bytes([0x81, 0]) + bytes(18) + struct.pack("<IQ", bitmap_cl[0], bitmap_bytes)
    root += bytes([0x82]) + bytes(3) + struct.pack("<I", upsum) + bytes(12) + struct.pack("<IQ", upcase_cl[0], len(upcase))
    root += x_entry_set("HELLO.TXT", False, f.contiguous_file(hello), len(hello), len(hello), True)
    root += x_entry_set("readme.txt", False, f.contiguous_file(readme), len(readme), len(readme), True)
    root += x_entry_set("Lange Datei mit Leerzeichen.txt", False, f.contiguous_file(lang), len(lang), len(lang), True)
    root += x_entry_set("Geloeschte Datei.txt", False, 0, 0, 0, True, deleted=True)
    root += x_entry_set("Bilder und Notizen", True, sub_cl[0], f.cb, f.cb, True)
    root += x_entry_set("BIG.BIN", False, f.contiguous_file(big), len(big), len(big), True)
    root += x_entry_set("FRAG.BIN", False, f.fragmented_file(frag), len(frag), len(frag), False)
    sparse_first = f.contiguous_file(sparse_disk)
    root += x_entry_set("SPARSE.BIN", False, sparse_first, 3000, 1000, True)
    root += x_entry_set("Bär.txt", False, f.contiguous_file(b"Umlaut\n"), 7, 7, True)
    root += x_entry_set("EXFAT.MRK", False, 0, 0, 0, True)
    if DIRTY:
        root += x_entry_set("DIRTY.MRK", False, 0, 0, 0, True)
    assert len(root) + 32 <= f.cb
    f.write(root_cl, bytes(root))

    # Belegungs-Bitmap: alle vergebenen Cluster
    bm = bytearray(bitmap_bytes)
    for c in range(2, f.next_free):
        if c in f.gaps:
            continue
        bm[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    f.write(bitmap_cl, bytes(bm))

    # FAT
    fat = b"".join(struct.pack("<I", v) for v in f.fat)
    o = f.fat_off * SECTOR
    f.image[o:o + len(fat)] = fat

    # Boot-Bereich: Sektor 0 (VBR), 1-8 erweiterte Bootsektoren, 9-10 leer, 11 Pruefsumme; Sicherung ab Sektor 12
    region = bytearray(12 * SECTOR)
    bs = region[0:SECTOR]
    bs[0:3] = b"\xEB\x76\x90"
    bs[3:11] = b"EXFAT   "
    struct.pack_into("<QQIIIIIIHH", bs, 64, f.part_offset, f.total, f.fat_off, f.fat_len, f.heap, f.ccount,
                     root_cl[0], 0x2468ACE0, 0x0100, 2 if DIRTY else 0)
    bs[108] = 9
    bs[109] = f.spc_shift
    bs[110] = 1
    bs[111] = 0x80
    bs[112] = 0xFF
    bs[510:512] = b"\x55\xAA"
    region[0:SECTOR] = bs
    for k in range(1, 9):
        region[k * SECTOR + 508:k * SECTOR + 512] = b"\x00\x00\x55\xAA"
    chk = 0
    for i in range(11 * SECTOR):
        if i in (106, 107, 112):
            continue
        chk = ((((chk & 1) << 31) | (chk >> 1)) + region[i]) & 0xFFFFFFFF
    region[11 * SECTOR:12 * SECTOR] = struct.pack("<I", chk) * (SECTOR // 4)
    f.image[0:12 * SECTOR] = region
    f.image[12 * SECTOR:24 * SECTOR] = region
    return f


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    bits = 12
    mbr = False
    i = 1
    while i < len(args):
        if args[i] == "--fat":
            bits = 0 if args[i + 1] == "exfat" else int(args[i + 1])
            i += 2
        elif args[i] == "--mbr":
            mbr = True
            i += 1
        elif args[i] == "--dirty":
            global DIRTY
            DIRTY = True
            i += 1
        else:
            sys.exit(__doc__)
    assert bits in (0, 12, 16)

    f = build_exfat(mbr) if bits == 0 else build(bits, mbr)
    if not mbr:
        data = bytes(f.image)
    else:
        total = PART_START + f.total + 2048
        disk = bytearray(total * SECTOR)
        disk[PART_START * SECTOR:PART_START * SECTOR + len(f.image)] = f.image
        ptype = 0x07 if bits == 0 else 0x01 if bits == 12 else 0x06
        disk[446:462] = struct.pack("<B3sB3sII", 0x80, b"\x00\x21\x00", ptype, b"\xFE\xFF\xFF", PART_START, f.total)
        disk[510:512] = b"\x55\xAA"
        data = bytes(disk)
    with open(path, "wb") as out:
        out.write(data)
    print(f"{path}: {'exFAT' if bits == 0 else 'FAT%d' % bits}, {len(data) // 1024} KiB{', MBR' if mbr else ''}")


if __name__ == "__main__":
    main()
