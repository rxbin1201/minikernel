# MiniKernel

Ein kleines x86_64-Betriebssystem: eigener UEFI-Bootloader, Kernel mit Prozessen, Dateisystemen, USB und Netzwerk,
dazu ein Userland mit Shell, Werkzeugen, Editor, Spielen und einer grafischen Oberflaeche.

## Schnellstart

```sh
make          # Bootloader, Kernel und initrd bauen (Ergebnisse in Image/)
make run      # in QEMU starten
make test     # Selbsttests in QEMU, danach Zusammenfassung "N OK, M FEHLER"
make help     # alle Ziele und Optionen
```

In der Shell zeigt `help` die Befehle, `desktop` startet die grafische Oberflaeche, `poweroff` beendet das System.
Alles, was der Kernel ausgibt, steht auch in `Build/Out.log` (serielle Schnittstelle).

### Voraussetzungen (Ubuntu/WSL)

- `gcc`, `binutils`, `make`, `python3`
- `qemu-system-x86` und `ovmf` (UEFI-Firmware, erwartet unter `/usr/share/ovmf/OVMF.fd`, sonst `OVMF=...` angeben)
- `qemu-utils` nur fuer `make vmware`
- gnu-efi liegt fertig gebaut im Projekt (`gnu-efi/`)

**KVM:** Ist `/dev/kvm` fuer den Benutzer beschreibbar, startet QEMU mit Hardware-Virtualisierung (deutlich schneller),
sonst in reiner Emulation. Unter WSL einmalig `sudo usermod -aG kvm $USER` und WSL neu starten. `KVM=0` schaltet es ab.

## Optionen fuer `make run` / `make test`

| Option | Bedeutung |
|---|---|
| `DISK=virtio\|nvme\|ahci\|usb` | Controller der Datenplatte (Standard `virtio`) |
| `DISK_LAYOUT=none\|mbr\|gpt` | Partitionierung beim Neuanlegen der Datenplatte |
| `NET=e1000\|e1000e\|none` | Netzwerkkarte im QEMU-User-Netz (Gast 10.0.2.15, Router 10.0.2.2) |
| `STICK=12\|16\|exfat` | zusaetzlichen Test-Stick (FAT12/FAT16/exFAT) am USB anschliessen |
| `TESTS=1` / `TESTS=disk,user` | Selbsttests beim Start (alle bzw. nur diese Gruppen); `KEEP=1` bleibt danach im System |
| `CMDLINE="..."` | weitere Kernel-Kommandozeile, siehe unten |
| `QEMU_EXTRA="..."` | weitere QEMU-Argumente |

Beispiele: `make run CMDLINE="mode=1600x900 kbd=de"`, `make test TESTS=foreign STICK=exfat`, `make run DISK=nvme`.

Die Datenplatte `Image/disk.img` (FAT32, Label `MINIKERNEL`, im System unter `/disk`) bleibt bei `make clean` erhalten;
`make cleandisk` legt sie neu an, `make fatcheck` prueft sie.

## Kernel-Kommandozeile

Der Bootloader liest `\cmdline.txt` von der EFI-Systempartition (bei QEMU aus `CMDLINE`/`TESTS` erzeugt).

| Option | Bedeutung |
|---|---|
| `mode=1600x900`, `mode=max` | Grafikmodus (naechstliegende Aufloesung bzw. groesste); ohne: Modus der Firmware |
| `scale=1..4` | Schriftvergroesserung der Konsole |
| `kbd=us\|de\|uk` | Tastaturlayout |
| `tz=eu\|uk\|utc\|+2\|+5:30` | Zeitzone der Uhr |
| `init=/bin/...` | erstes Programm statt `/bin/sh` |
| `ip=192.168.1.50/24,192.168.1.1[,dns]` | feste Adresse fuer eth0 statt DHCP; `nodhcp`, `nonet`, `nontp` schalten ab |
| `fsro` | fremde Volumes nur lesbar einbinden |
| `selftest`, `selftest=gruppe,...`, `keep` | Selbsttests (siehe `Kernel/tests/selftest.c`) |

Im laufenden System schreibt `resolution` Grafikmodus und Schriftgroesse in die `cmdline.txt` der Boot-Partition.

## Echte Hardware und VMware

```sh
make usb        # Image/usb/minikernel-usb.img: mit Rufus (DD-Modus) oder balenaEtcher auf einen Stick schreiben
make usbfiles   # Image/usbfiles/: Inhalt auf einen FAT32-Stick kopieren
make vmware     # Image/vmware/: VM fuer VMware (VMWARE_BUS=sata|nvme)
```

Secure Boot muss aus sein (der Bootloader ist nicht signiert).

## Aufbau

```
Sources/main.c        UEFI-Bootloader: laedt kernel.elf, initrd.tar, cmdline.txt; Grafikmodus; springt in den Kernel
Includes/boot_info.h  Uebergabe Bootloader -> Kernel

Kernel/
  arch/x86_64/        Einstieg (entry.S), GDT/IDT, Interrupts, APIC/IOAPIC, ACPI, Ausschalten, Syscall-Einstieg
  mm/                 physischer Speicher, Paging, Heap, Kernel-Stacks
  core/               kmain (kernel.c), Prozesse, Scheduler, Syscalls, TTY, Kommandozeile
  console/            Textkonsole im Framebuffer, Schriften
  drivers/            PCI, serielle Schnittstelle, Uhr, Tastatur, Maus, Grafik
    block/            AHCI, NVMe, virtio-blk, Partitionen (MBR/GPT)
    usb/              xHCI, Tastatur/Maus (HID), Massenspeicher
    net/              Intel e1000/e1000e
  fs/                 VFS, Dateisystem-Schicht; fat/: FAT12/16/32 und exFAT
  net/                IPv4-Stack: ARP, ICMP, DHCP, UDP, DNS, NTP
  lib/                string, kprintf, UTF-8
  tests/              Selbsttests, je Gruppe eine Datei

Userland/
  include/            Syscalls (user.h), libc, malloc, gfx (Grafik), util (Helfer fuer Werkzeuge)
  lib/                libuser.a
  bin/                je Programm eine Datei (NAME.c) oder ein Ordner (sh/, desktop/) -> /bin/NAME

Initrd/               wird 1:1 in die initrd kopiert (/etc/profile, /etc/motd, Test-Skripte)
tools/                Images erzeugen (mkdisk, mkesp, mkstick, mkvmx), FAT pruefen (fatcheck), Schriften (gen_font*)
```

Build-Ergebnisse landen in `Build/` (Objektdateien, `kernel.debug.elf` mit Debug-Infos, `esp.img`, `Out.log`) und
`Image/` (`bootx64.efi`, `kernel.elf`, `initrd.tar`, Datenplatte).
