#!/usr/bin/env python3
"""Schreibt eine VMware-Konfiguration (.vmx) fuer den Kernel.

Aufruf: mkvmx.py <ausgabe.vmx> [--bus sata|nvme]

Boot-Platte (boot.vmdk) und Datenplatte (data.vmdk) liegen am gewaehlten Controller (Standard: SATA = AHCI).
UEFI-Firmware ohne Secure Boot, 1 vCPU, 1 GiB RAM, serieller Port COM1 -> serial.log (dorthin schreibt der Kernel mit),
Netzwerkkarte e1000e an NAT.
"""
import sys


def main():
    args = sys.argv[1:]
    bus = "sata"
    if "--bus" in args:
        i = args.index("--bus")
        bus = args[i + 1]
        del args[i:i + 2]
    if not args or bus not in ("sata", "nvme"):
        sys.exit(__doc__)

    lines = [
        '.encoding = "UTF-8"',
        'config.version = "8"',
        'virtualHW.version = "16"',
        'displayName = "MiniKernel"',
        'guestOS = "other-64"',
        'firmware = "efi"',
        'uefi.secureBoot.enabled = "FALSE"',   # unser Bootloader ist nicht signiert
        'memsize = "1024"',
        'numvcpus = "1"',                       # Kernel nutzt nur den Boot-CPU
        'hpet0.present = "TRUE"',
        'pciBridge0.present = "TRUE"',
    ]
    # PCIe-Root-Ports: NVMe-Controller haengen an einem PCIe-Slot (sonst "No PCIe slot available for nvme0").
    # So schreibt VMware die Bruecken selbst in generierte Konfigurationen.
    for n in (4, 5, 6, 7):
        lines += [
            'pciBridge%d.present = "TRUE"' % n,
            'pciBridge%d.virtualDev = "pcieRootPort"' % n,
            'pciBridge%d.functions = "8"' % n,
        ]
    lines += [
        '%s0.present = "TRUE"' % bus,
        '%s0:0.present = "TRUE"' % bus,
        '%s0:0.fileName = "boot.vmdk"' % bus,
        '%s0:1.present = "TRUE"' % bus,
        '%s0:1.fileName = "data.vmdk"' % bus,
        'serial0.present = "TRUE"',
        'serial0.fileType = "file"',
        'serial0.fileName = "serial.log"',
        'floppy0.present = "FALSE"',
        'usb.present = "FALSE"',              # kein UHCI/EHCI: Geraete sollen am xHCI (USB 3.x) haengen
        'usb_xhci.present = "TRUE"',
        'ethernet0.present = "TRUE"',         # Netzwerkkarte Intel 82574L (e1000e), per NAT am Host
        'ethernet0.virtualDev = "e1000e"',
        'ethernet0.connectionType = "nat"',
        'ethernet0.addressType = "generated"',
    ]
    if bus == "sata":
        lines[lines.index('sata0:0.present = "TRUE"')] = 'sata0:0.present = "TRUE"\nsata0:0.deviceType = "disk"'
        lines[lines.index('sata0:1.present = "TRUE"')] = 'sata0:1.present = "TRUE"\nsata0:1.deviceType = "disk"'

    with open(args[0], "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s geschrieben (Controller: %s)" % (args[0], bus))


if __name__ == "__main__":
    main()
