#!/usr/bin/env python3
"""Erzeugt ein FAT32-Diskimage fuer den Kernel-Treiber.

Aufruf: mkdisk.py <datei> [groesse_in_MiB=64] [--layout none|mbr|gpt]

  --layout none  (Standard) das Volume belegt das ganze Image ("Superfloppy", ohne Partitionstabelle)
  --layout mbr   MBR mit einer FAT32-Partition (Typ 0x0C) ab Sektor 2048
  --layout gpt   GPT (mit Schutz-MBR, primaerer und Sicherungstabelle) mit einer Basic-Data-Partition ab Sektor 2048

Das Volume heisst MINIKERNEL (nur Volumes mit diesem Label benutzt der Kernel). Es enthaelt SEED.TXT (mehrere Cluster,
bekanntes Muster) und DOCS/NOTE.TXT, damit der Kernel das Lesen von Dateien pruefen kann, die er nicht selbst geschrieben
hat. Aufbau nach Microsofts FAT-Spezifikation (1 Sektor pro Cluster, 2 FATs, FSInfo, Backup-Bootsektor); geprueft mit
dosfstools' fsck.fat.
"""
import struct
import sys
import uuid
import zlib

SECTOR = 512
SPC = 1           # Sektoren pro Cluster
RESERVED = 32
NFATS = 2
DATE = (2026 - 1980) << 9 | 1 << 5 | 1  # 2026-01-01
EOC = 0x0FFFFFFF
PART_START = 2048
GPT_ENTRIES_SECTORS = 32                # 128 Eintraege a 128 Byte


def seed_pattern(n):
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


class Fat32:
    def __init__(self, path, disk_sectors, offset, part_sectors, label="MINIKERNEL"):
        self.label = label.ljust(11)[:11].encode("ascii")
        self.offset = offset
        self.total = part_sectors
        fatsz = 1
        while True:
            data = self.total - RESERVED - NFATS * fatsz
            clusters = data // SPC
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fatsz:
                break
            fatsz = need
        self.fatsz = fatsz
        self.clusters = clusters
        self.fat_lba = RESERVED
        self.data_lba = RESERVED + NFATS * fatsz
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = EOC
        self.f = open(path, "w+b")
        self.f.truncate(disk_sectors * SECTOR)
        self.next_free = 2

    def alloc(self, count):
        cl = list(range(self.next_free, self.next_free + count))
        self.next_free += count
        for a, b in zip(cl, cl[1:]):
            self.fat[a] = b
        self.fat[cl[-1]] = EOC
        return cl

    def write_abs(self, lba, data):
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def write_sector(self, lba, data):
        self.write_abs(self.offset + lba, data.ljust(SECTOR, b"\0"))

    def write_cluster(self, c, data):
        for i in range(SPC):
            self.write_sector(self.data_lba + (c - 2) * SPC + i, data[i * SECTOR:(i + 1) * SECTOR])

    @staticmethod
    def dirent(name11, attr, cluster, size):
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, 0, DATE, DATE, cluster >> 16, 0, DATE,
                           cluster & 0xFFFF, size)

    def add_file(self, content):
        count = max(1, (len(content) + SPC * SECTOR - 1) // (SPC * SECTOR))
        cl = self.alloc(count)
        for i, c in enumerate(cl):
            self.write_cluster(c, content[i * SPC * SECTOR:(i + 1) * SPC * SECTOR])
        return cl[0]

    def finish(self, root_entries):
        # Wurzelverzeichnis: Cluster 2 (in main() vorab belegt)
        self.write_cluster(2, b"".join(root_entries))
        free = self.clusters - (self.next_free - 2)

        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x58\x90"
        boot[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, SPC, RESERVED, NFATS, 0, 0, 0xF8, 0, 63, 255, self.offset,
                         self.total)  # ..., Sektoren/Spur, Koepfe, versteckte Sektoren (= Partitionsbeginn), Gesamtsektoren
        struct.pack_into("<IHHIHH", boot, 36, self.fatsz, 0, 0, 2, 1, 6)  # fatsz32, flags, version, root, fsinfo, backup
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into("<I", boot, 67, 0x4D494E49)
        boot[71:82] = self.label
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xAA"

        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<I", fsinfo, 484, 0x61417272)
        struct.pack_into("<II", fsinfo, 488, free, self.next_free)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)

        self.write_sector(0, bytes(boot))
        self.write_sector(1, bytes(fsinfo))
        self.write_sector(6, bytes(boot))
        self.write_sector(7, bytes(fsinfo))

        fat_bytes = struct.pack("<%dI" % len(self.fat), *self.fat).ljust(self.fatsz * SECTOR, b"\0")
        for i in range(NFATS):
            self.write_abs(self.offset + self.fat_lba + i * self.fatsz, fat_bytes)


def chs_dummy():
    return b"\xFE\xFF\xFF"


def write_mbr(fs, disk_sectors, part_sectors, protective=False):
    mbr = bytearray(SECTOR)
    if protective:  # GPT-Schutz-MBR: eine Partition vom Typ 0xEE ueber die ganze Platte
        entry = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, chs_dummy(), 1, min(disk_sectors - 1, 0xFFFFFFFF))
    else:
        entry = struct.pack("<B3sB3sII", 0, b"\x00\x21\x00", 0x0C, chs_dummy(), PART_START, part_sectors)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    fs.write_abs(0, bytes(mbr))


BASIC_DATA_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
EFI_SYSTEM_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"


def write_gpt(fs, disk_sectors, part_sectors, type_guid_str=BASIC_DATA_GUID, name="MINIKERNEL"):
    write_mbr(fs, disk_sectors, part_sectors, protective=True)

    type_guid = uuid.UUID(type_guid_str).bytes_le  # GUIDs sind in gemischter Byte-Reihenfolge gespeichert
    entry = bytearray(128)
    entry[0:16] = type_guid
    entry[16:32] = uuid.UUID(int=0x4D494E494B45524E454C00000000BEEF).bytes_le
    struct.pack_into("<QQQ", entry, 32, PART_START, PART_START + part_sectors - 1, 0)
    entry[56:56 + 2 * len(name)] = name.encode("utf-16-le")
    entries = bytes(entry).ljust(GPT_ENTRIES_SECTORS * SECTOR, b"\0")
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    first_usable = 2 + GPT_ENTRIES_SECTORS
    last_usable = disk_sectors - 1 - GPT_ENTRIES_SECTORS - 1
    disk_guid = uuid.UUID(int=0x1234567890ABCDEF1234567890ABCDEF).bytes_le

    def header(current, backup, entries_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x00010000, 92, 0, 0, current, backup, first_usable, last_usable)
        h[56:72] = disk_guid
        struct.pack_into("<QIII", h, 72, entries_lba, 128, 128, entries_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
        return bytes(h)

    fs.write_abs(1, header(1, disk_sectors - 1, 2))
    fs.write_abs(2, entries)
    fs.write_abs(disk_sectors - 1 - GPT_ENTRIES_SECTORS, entries)
    fs.write_abs(disk_sectors - 1, header(disk_sectors - 1, 1, disk_sectors - 1 - GPT_ENTRIES_SECTORS))


def main():
    args = [a for a in sys.argv[1:]]
    layout = "none"
    if "--layout" in args:
        i = args.index("--layout")
        layout = args[i + 1]
        del args[i:i + 2]
    if not args or layout not in ("none", "mbr", "gpt"):
        sys.exit(__doc__)
    path = args[0]
    size = int(args[1]) if len(args) > 1 else 64

    disk_sectors = size * 1024 * 1024 // SECTOR
    if layout == "none":
        offset, part_sectors = 0, disk_sectors
    elif layout == "mbr":
        offset, part_sectors = PART_START, disk_sectors - PART_START
    else:
        offset, part_sectors = PART_START, disk_sectors - PART_START - GPT_ENTRIES_SECTORS - 1

    fs = Fat32(path, disk_sectors, offset, part_sectors)
    assert fs.alloc(1) == [2]  # Cluster 2 = Wurzelverzeichnis

    seed = fs.add_file(seed_pattern(5000))
    note_text = b"Diese Datei liegt in einem Unterverzeichnis.\n"
    note = fs.add_file(note_text)

    docs = fs.alloc(1)[0]
    dot = fs.dirent(b".          ", 0x10, docs, 0)
    dotdot = fs.dirent(b"..         ", 0x10, 0, 0)
    fs.write_cluster(docs, dot + dotdot + fs.dirent(b"NOTE    TXT", 0x20, note, len(note_text)))

    entries = [
        fs.dirent(b"MINIKERNEL ", 0x08, 0, 0),  # Volume-Label (passend zum Bootsektor)
        fs.dirent(b"SEED    TXT", 0x20, seed, 5000),
        fs.dirent(b"DOCS       ", 0x10, docs, 0),
    ]
    fs.finish(entries)

    if layout == "mbr":
        write_mbr(fs, disk_sectors, part_sectors)
    elif layout == "gpt":
        write_gpt(fs, disk_sectors, part_sectors)
    fs.f.close()
    print("%s: %d MiB, Layout %s, %d Cluster, %d Sektoren pro FAT" % (path, size, layout, fs.clusters, fs.fatsz))


if __name__ == "__main__":
    main()
