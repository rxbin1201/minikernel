#!/usr/bin/env python3
"""Erzeugt ein bootfaehiges UEFI-Diskimage (GPT + EFI-Systempartition mit FAT32).

Aufruf: mkesp.py <ausgabe.img> <bootx64.efi> <kernel.elf> <initrd.tar> [groesse_in_MiB=64] [--cmdline "text"]

Inhalt der Partition:  /EFI/BOOT/BOOTX64.EFI   (Fallback-Pfad: jede UEFI-Firmware startet ihn von allein)
                       /KERNEL.ELF, /INITRD.TAR (vom Bootloader geladen)
Das Volume heisst KERNELBOOT, der Kernel benutzt als Datenplatte nur Volumes namens MINIKERNEL, fasst dieses also nicht an.
"""
import os
import sys

from mkdisk import (EFI_SYSTEM_GUID, GPT_ENTRIES_SECTORS, PART_START, SECTOR, SPC, Fat32, write_gpt)


def main():
    args = sys.argv[1:]
    cmdline = None
    if "--cmdline" in args:
        i = args.index("--cmdline")
        cmdline = args[i + 1]
        del args[i:i + 2]
    if len(args) < 4:
        sys.exit(__doc__)
    out, efi, kernel, initrd = args[:4]
    size = int(args[4]) if len(args) > 4 else 64

    disk_sectors = size * 1024 * 1024 // SECTOR
    part_sectors = disk_sectors - PART_START - GPT_ENTRIES_SECTORS - 1
    fs = Fat32(out, disk_sectors, PART_START, part_sectors, label="KERNELBOOT")
    assert fs.alloc(1) == [2]  # Cluster 2 = Wurzelverzeichnis

    def read(path):
        with open(path, "rb") as f:
            return f.read()

    efi_data, kernel_data, initrd_data = read(efi), read(kernel), read(initrd)
    efi_cl = fs.add_file(efi_data)
    kernel_cl = fs.add_file(kernel_data)
    initrd_cl = fs.add_file(initrd_data)
    cmd_data = (cmdline + "\n").encode("ascii") if cmdline else b""
    cmd_cl = fs.add_file(cmd_data) if cmdline else 0

    def dir_cluster(parent_cluster, entries):
        c = fs.alloc(1)[0]
        dots = (fs.dirent(b".          ", 0x10, c, 0) + fs.dirent(b"..         ", 0x10, parent_cluster, 0))
        assert 2 + len(entries) <= SECTOR // 32 * SPC
        fs.write_cluster(c, dots + b"".join(entries))
        return c

    # BOOT und EFI brauchen den Elterncluster fuer ".."; EFI liegt in der Wurzel (Cluster 0 bedeutet Wurzel)
    efi_dir_cluster = fs.next_free
    boot_cluster = fs.next_free + 1
    # Reihenfolge: erst EFI (Cluster n), dann BOOT (Cluster n+1)
    dir_cluster(0, [fs.dirent(b"BOOT       ", 0x10, boot_cluster, 0)])
    dir_cluster(efi_dir_cluster, [fs.dirent(b"BOOTX64 EFI", 0x20, efi_cl, len(efi_data))])

    root = [
        fs.dirent(b"KERNELBOOT ", 0x08, 0, 0),
        fs.dirent(b"EFI        ", 0x10, efi_dir_cluster, 0),
        fs.dirent(b"KERNEL  ELF", 0x20, kernel_cl, len(kernel_data)),
        fs.dirent(b"INITRD  TAR", 0x20, initrd_cl, len(initrd_data)),
    ]
    if cmdline:  # optionale Kernel-Kommandozeile (\\cmdline.txt, siehe Kernel/core/cmdline.h)
        root.append(fs.dirent(b"CMDLINE TXT", 0x20, cmd_cl, len(cmd_data)))
    fs.finish(root)
    write_gpt(fs, disk_sectors, part_sectors, EFI_SYSTEM_GUID, "EFI System")
    fs.f.close()
    print("%s: %d MiB, GPT + EFI-Systempartition (%d Cluster), %d Bytes Kernel, %d Bytes initrd" %
          (out, size, fs.clusters, len(kernel_data), len(initrd_data)))


if __name__ == "__main__":
    main()
