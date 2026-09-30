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
| `HEADLESS=1` | ohne Fenster, Ausgabe nur in `Build/Out.log` (z.B. `make test HEADLESS=1`) |
| `QEMU_EXTRA="..."` | weitere QEMU-Argumente |

Beispiele: `make run CMDLINE="mode=1600x900 kbd=de"`, `make test TESTS=foreign STICK=exfat`, `make run DISK=nvme`.

Die Datenplatte `Image/disk.img` (FAT32, Label `MINIKERNEL`, im System unter `/disk`) bleibt bei `make clean` erhalten;
`make cleandisk` legt sie neu an, `make fatcheck` prueft sie.

## Mehrere CPUs (SMP)

Der Kernel startet alle CPUs aus der ACPI-MADT (QEMU: `make run SMP=N`, Standard 4). Threads und Prozesse kommen aus
einer gemeinsamen Run-Queue und laufen auf jeder CPU. Kernel-Code ist durch einen Big Kernel Lock geschuetzt
(immer nur eine CPU im Kernel), User-Programme rechnen echt parallel. Details: `Kernel/arch/x86_64/smp.h`.

In der Shell: `burn 5000 & burn 5000 & cpus` zeigt zwei ausgelastete CPUs.

## Intel-Grafik (Gen9: Skylake bis Comet Lake, z.B. UHD Graphics 630)

`Kernel/drivers/gpu/igd.c` setzt auf der Anzeige auf, die die UEFI-Firmware eingerichtet hat, und ergaenzt:

- **Hardware-Mauszeiger:** eigene Ebene der Pipe; Konsole und Grafikprogramme verschieben ihn nur noch
- **Doppelpufferung:** ganze Bilder von Grafikprogrammen (`gfx_present_all`) kommen in einen verdeckten Puffer und
  werden beim Bildwechsel umgeschaltet (kein Tearing). Der Puffer liegt im RAM und wird mit Non-Temporal-Stores
  beschrieben (am CPU-Cache vorbei, den die Display-Engine nicht sieht); bei 3440x1440 ca. 4 ms je Bild
- **Moduswechsel im Betrieb** (`igd_mode.c`, `igd_dp.c`): die Modi aus den Monitordaten (EDID), die ueber den
  Anschluss gehen und in den Framebuffer der Firmware passen. HDMI: hoechstens 300 MHz Pixeltakt (Gen9), Pipe und
  Port werden mit neu berechnetem DPLL neu gestartet. DisplayPort: die von der Firmware eingemessene Verbindung
  bleibt (z.B. 4 Lanes x 5,4 GBit/s), neu gesetzt werden Zeitablauf, M/N und Watermarks; so gehen z.B.
  3440x1440 mit 100 Hz. `resolution` listet sie mit
  Bildrate und schaltet sofort um (`resolution 2560x1440@60`), die Konsole passt sich an. Fuer den naechsten Start
  speichert es `igdmode=2560x1440@60` und `mode=max` (der Framebuffer der Firmware muss gross genug sein)

`noigd` in der Kommandozeile schaltet alles ab. `igdtest`, `igdtest cursor` und `igdtest blit` pruefen
Page-Flipping, Mauszeiger und Blitter einzeln und schreiben Messwerte ins Kernel-Log (`dmesg`). `igdtest info`
zeigt, wie viele Bild-Updates es seit dem Start gab und was sie gekostet haben, und vergleicht die Kopierwege fuer
ganze Bilder. `igdtest edid`, `igdtest scale` und `igdtest mode` pruefen Monitordaten, Skalierer und
Moduswechsel (HDMI), `igdtest dp` und `igdtest dpmode` dasselbe per DisplayPort. QEMU emuliert keine Intel-GPU: getestet wird
auf echter Hardware (bisher i5-8400T, UHD 630, 3440x1440 ueber HDMI).

## Fehlersuche

- **Selbsttests:** `make test` (alle Gruppen) oder `make test TESTS=disk,user`; jede Gruppe laeuft auch einzeln.
  Nach jeder Gruppe wird der Kernel-Heap geprueft.
- **Backtraces:** Eine Exception im Kernel gibt die Aufrufkette mit Funktionsnamen aus (Framepointer-Kette,
  Symboltabelle aus `tools/mksyms.py`), z.B.
  ```
  *** EXCEPTION 14: Page Fault ***
    Aufrufkette:
      0x10427d test_paging+0x17d
      0x10a62c run_selftests+0x86c
      0x12bf17 kmain+0x257
  ```
  Ist der Heap beschaedigt, nennt `heap_check` die Funktion, die den Block davor angelegt hat.
- **gdb:** `make debug` startet QEMU angehalten mit gdb-Server (ohne KVM), `make gdb` im zweiten Terminal verbindet
  sich, laedt die Symbole aus `Build/kernel.debug.elf` und setzt einen Breakpoint auf `kmain` (`tools/gdbinit`).
  Braucht `sudo apt install gdb`.
- **Adressen von Hand:** `addr2line -f -e Build/kernel.debug.elf 0x10427d`
- **Kernel-Log auf echter Hardware:** `dmesg` zeigt alle Kernel-Meldungen seit dem Start (letzte 256 KiB);
  `dmesg > /disk/log.txt` speichert sie. `/disk` ist das FAT32-Volume mit dem Label `MINIKERNEL` (nur dieses
  beschreibt der Kernel), z.B. ein USB-Stick, der unter Windows so benannt wurde.

## Kernel-Kommandozeile

Der Bootloader liest `\cmdline.txt` von der EFI-Systempartition (bei QEMU aus `CMDLINE`/`TESTS` erzeugt).

| Option | Bedeutung |
|---|---|
| `mode=1600x900`, `mode=max` | Grafikmodus (naechstliegende Aufloesung bzw. groesste); ohne: Modus der Firmware |
| `igdmode=2560x1440@60` | Intel-Grafik: diesen Modus des Monitors beim Start setzen (`resolution` traegt ihn ein) |
| `scale=1..4` | Schriftvergroesserung der Konsole |
| `kbd=us\|de\|uk` | Tastaturlayout |
| `tz=eu\|uk\|utc\|+2\|+5:30` | Zeitzone der Uhr |
| `init=/bin/...` | erstes Programm statt `/bin/sh` |
| `ip=192.168.1.50/24,192.168.1.1[,dns]` | feste Adresse fuer eth0 statt DHCP; `nodhcp`, `nonet`, `nontp` schalten ab |
| `fsro` | fremde Volumes nur lesbar einbinden |
| `nosmp`, `cpus=N` | nur die Boot-CPU bzw. hoechstens N CPUs benutzen |
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
  arch/x86_64/        Einstieg (entry.S), GDT/IDT, Interrupts, APIC/IOAPIC, ACPI, Ausschalten, Syscall-Einstieg,
                      SMP (smp.c, trampoline.S)
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
