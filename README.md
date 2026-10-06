# MiniKernel

Ein eigenes 64-Bit-Betriebssystem fuer x86_64-PCs: UEFI-Bootloader, Kernel mit mehreren CPUs, Prozessen und
Threads, Dateisystemen, USB, Netzwerk (Kabel und WLAN), Bluetooth-Audio, Ton und Intel-Grafik mit GPU-Beschleunigung -
dazu ein Userland mit Shell, rund 80 Programmen und einer grafischen Oberflaeche im Stil von macOS.

Laeuft in QEMU (`make run`) und auf echter Hardware (getestet: Intel i5-8400T mit UHD Graphics 630, Intel AX200 fuer
WLAN/Bluetooth, Monitor 3440x1440 ueber HDMI/DisplayPort).

**Inhalt**

1. [Stand auf einen Blick](#stand-auf-einen-blick)
2. [Schnellstart](#schnellstart)
3. [Auf echter Hardware](#auf-echter-hardware)
4. [Bedienung](#bedienung): Shell, Programme, Desktop, Einstellungen
5. [Hardware im Detail](#hardware-im-detail): Netzwerk, WLAN, Bluetooth, Intel-Grafik, Ton
6. [Kernel im Detail](#kernel-im-detail): CPUs, Threads, Speicher, Grenzen
7. [Fehlersuche und Tests](#fehlersuche-und-tests)
8. [Kernel-Kommandozeile](#kernel-kommandozeile)
9. [Aufbau des Projekts](#aufbau-des-projekts)
10. [Was noch fehlt](#was-noch-fehlt)

---

## Stand auf einen Blick

✅ funktioniert · 🟡 teilweise / Grundfunktion · ❌ fehlt noch

| Bereich | | Was geht | Was fehlt |
|---|---|---|---|
| **Start** | ✅ | Eigener UEFI-Bootloader, Startlogo der Firmware mit Ladebalken, Kommandozeile aus `cmdline.txt` | Secure Boot (Bootloader nicht signiert), Legacy-BIOS |
| **CPUs** | ✅ | Alle CPUs (SMP), Programme rechnen echt parallel, Gleitkomma/SSE in Programmen | Feinere Sperren im Kernel (noch ein Big Kernel Lock), Energiesparen |
| **Prozesse** | ✅ | `fork` (Copy-on-Write), `exec`, Pipes, Prozessgruppen, Strg+C, `kill`, bis 1024 Threads je Programm, Futex-Sperren | Signale, Benutzer und Rechte, Umgebungsvariablen fuer Programme |
| **Speicher** | ✅ | Paging, wachsender Stack, `mmap` (auch auf Dateien, Seiten erst bei Bedarf), geteilter Speicher | Auslagern auf die Platte |
| **Datentraeger** | ✅ | NVMe, AHCI (SATA), virtio-blk, USB-Sticks; MBR und GPT | Installation auf die interne Platte, Schreib-Cache |
| **Dateisysteme** | 🟡 | FAT12/16/32 mit langen Namen und exFAT, lesen und schreiben; initrd (tar) | ext4, NTFS, Dateirechte |
| **USB** | 🟡 | xHCI (USB 1-3), Hubs, Tastatur, Maus, Massenspeicher, Bluetooth; Transfers per Interrupt | Ohne xHCI (EHCI/OHCI), USB-Audio, Gamepads, Webcams |
| **Netzwerk** | 🟡 | Intel e1000/e1000e/I217-I219, IPv4, DHCP, DNS, NTP, UDP, TCP nach aussen, `wget` (HTTP) | HTTPS/TLS, TCP-Server (listen/accept), IPv6, andere Netzwerkkarten (Realtek, ...) |
| **WLAN** | 🟡 | Intel AX200: suchen, verbinden mit offenen und WPA2-PSK-Netzen, DHCP, im Desktop | Schnelle Raten (802.11n/ac/ax, hoechstens 54 Mbit/s), WPA3-only, Enterprise, andere Karten |
| **Bluetooth** | 🟡 | Intel AX200: Firmware laden, suchen, koppeln, Musik an Lautsprecher/Kopfhoerer (A2DP, SBC), Fernbedienung (AVRCP) | Tastaturen/Maeuse (HID), Freisprechen, Bluetooth LE, AAC |
| **Grafik** | ✅ | GOP-Framebuffer ueberall; Intel Gen9 (Skylake bis Comet Lake): Moduswechsel, HDMI/DP, Hotplug, Hardware-Zeiger, Doppelpufferung | AMD- und NVIDIA-Treiber, mehrere Monitore gleichzeitig |
| **3D** | 🟡 | Intel Gen9: eigene Shader, kleines OpenGL 1.x/1.5 (Texturen, Licht, Tiefentest, Mischen, Vertex-Buffer); Desktop setzt auf der GPU zusammen | Mip-Maps, programmierbare Shader fuer Programme, Mesa |
| **Ton** | ✅ | Intel HD Audio, 8 Programme gleichzeitig, WAV und MP3, Ausgabe waehlbar (Lautsprecher, Kopfhoerer, Bluetooth) | Mikrofon/Aufnahme, Lautstaerke je Programm im Menue, HDMI-Ton |
| **Desktop** | ✅ | Fenster mit Animationen, Taskleiste, Startmenue mit Suche, Andocken, Menues fuer Netzwerk, WLAN, Bluetooth und Ton, Task-Manager | Sperrbildschirm, Benachrichtigungen, Drag & Drop zwischen Programmen |
| **Programme** | ✅ | Terminal, Dateien, Texteditor, Musik, Bilder (BMP), Malen, Rechner, Uhr, Spiele, Einstellungen, Task-Manager | Webbrowser, PNG/JPEG, Compiler im System |
| **Shell** | ✅ | Pipes, Umleitungen, `&&`/`\|\|`, Hintergrund-Jobs, Variablen, `if`/`while`/`for`, Funktionen, Skripte, Vervollstaendigen | Jobsteuerung mit Strg+Z, Rueckwaertssuche im Verlauf |

Ausfuehrlich in [Was noch fehlt](#was-noch-fehlt).

---

## Schnellstart

```sh
make          # Bootloader, Kernel und initrd bauen (Ergebnisse in Image/)
make run      # in QEMU starten
make test     # Selbsttests in QEMU, danach Zusammenfassung "N OK, M FEHLER"
make help     # alle Ziele und Optionen
```

In der Shell zeigt `help` die Befehle, `desktop` startet die grafische Oberflaeche, `poweroff` beendet das System.
Alles, was der Kernel ausgibt, steht auch in `Build/Out.log` (serielle Schnittstelle).
Direkt in den Desktop starten: `make run CMDLINE="init=/bin/desktop"`.

### Voraussetzungen (Ubuntu/WSL)

- `gcc`, `binutils`, `make`, `python3`
- `qemu-system-x86` und `ovmf` (UEFI-Firmware, erwartet unter `/usr/share/ovmf/OVMF.fd`, sonst `OVMF=...` angeben)
- `qemu-utils` nur fuer `make vmware`
- gnu-efi liegt fertig gebaut im Projekt (`gnu-efi/`)

**KVM:** Ist `/dev/kvm` fuer den Benutzer beschreibbar, startet QEMU mit Hardware-Virtualisierung (deutlich schneller),
sonst in reiner Emulation. Unter WSL einmalig `sudo usermod -aG kvm $USER` und WSL neu starten. `KVM=0` schaltet es ab.

### Optionen fuer `make run` / `make test`

| Option | Bedeutung |
|---|---|
| `DISK=virtio\|nvme\|ahci\|usb` | Controller der Datenplatte (Standard `virtio`) |
| `DISK_LAYOUT=none\|mbr\|gpt` | Partitionierung beim Neuanlegen der Datenplatte |
| `NET=e1000\|e1000e\|none` | Netzwerkkarte im QEMU-User-Netz (Gast 10.0.2.15, Router 10.0.2.2) |
| `SOUND=none\|wav\|pa\|off` | Soundkarte: Ton ins Leere, nach `Build/sound.wav`, ueber PulseAudio (unter WSLg die Windows-Lautsprecher) oder ohne Karte |
| `MOUSE=0` | ohne USB-Maus. Standard: `usb-tablet` (der Zeiger folgt der Maus des Rechners, ohne sie einzufangen) |
| `SMP=N` | Zahl der CPUs (Standard 4) |
| `STICK=12\|16\|exfat` | zusaetzlichen Test-Stick (FAT12/FAT16/exFAT) am USB anschliessen |
| `TESTS=1` / `TESTS=disk,user` | Selbsttests beim Start (alle bzw. nur diese Gruppen); `KEEP=1` bleibt danach im System |
| `CMDLINE="..."` | weitere Kernel-Kommandozeile, siehe [Kernel-Kommandozeile](#kernel-kommandozeile) |
| `HEADLESS=1` | ohne Fenster, Ausgabe nur in `Build/Out.log` (z.B. `make test HEADLESS=1`) |
| `QEMU_EXTRA="..."` | weitere QEMU-Argumente |

Beispiele: `make run CMDLINE="mode=1600x900 kbd=de"`, `make test TESTS=foreign STICK=exfat`, `make run DISK=nvme`.

Die Datenplatte `Image/disk.img` (FAT32, Label `MINIKERNEL`, im System unter `/disk`) bleibt bei `make clean` erhalten;
`make cleandisk` legt sie neu an, `make fatcheck` prueft sie.

QEMU hat keine Intel-GPU, keine AX200 und kein Bluetooth: diese Treiber lassen sich nur auf echter Hardware testen.

---

## Auf echter Hardware

```sh
make usb        # Image/usb/minikernel-usb.img: mit Rufus (DD-Modus) oder balenaEtcher auf einen Stick schreiben
make usbfiles   # Image/usbfiles/: Inhalt auf einen FAT32-Stick kopieren
make vmware     # Image/vmware/: VM fuer VMware (VMWARE_BUS=sata|nvme)
```

- **Secure Boot** muss aus sein (der Bootloader ist nicht signiert).
- **Daten:** der Kernel schreibt auf das FAT32-Volume mit dem Label `MINIKERNEL` (`/disk`), z.B. einen USB-Stick, der
  unter Windows so benannt wurde. Weitere Datentraeger erscheinen unter `/mnt` (`usb0`, `usb0p1`, ...).
- **Firmware fuer WLAN und Bluetooth** kommt aus *linux-firmware* und liegt lokal in `firmware/` (nicht im
  Repository): `iwlwifi-cc-a0-77.ucode` (WLAN), `ibt-20-1-3.sfi` und optional `ibt-20-1-3.ddc` (Bluetooth). `make`
  packt sie nach `/firmware` in die initrd.
- **Kernel-Log:** `dmesg` zeigt alle Meldungen seit dem Start, `dmesg > /disk/log.txt` speichert sie.

---

## Bedienung

### Shell

Die Shell (`/bin/sh`) ist an die POSIX-Shell angelehnt:

| | |
|---|---|
| Zeileneditor | Pfeiltasten, Pos1/Ende, Verlauf (hoch/runter), Tab vervollstaendigt, Strg+C bricht ab, Strg+D beendet |
| Syntax | `a \| b`, `< > >> 2> 2>&1`, `;`, `&&`, `\|\|`, `&` (Hintergrund), `!`, `# Kommentar`, `'...'`, `"..."` |
| Bloecke | `if/elif/else/fi`, `while`/`until ... do ... done`, `for x in ...; do ... done`, `name() { ...; }`, `( ... )` |
| Ersetzungen | `$NAME`, `${NAME:-vorgabe}`, `${#NAME}`, `$?`, `$$`, `$1..$9`, `$@`, `~`, `$(befehl)`, `$((rechnung))`, Platzhalter `* ? [abc]` |
| Eingebaut | `cd pwd exit echo test [ read set export unset shift return break continue type source history clear jobs fg wait help` |
| Skripte | `sh datei`, `./datei` bzw. Dateien mit `#!` oder auf `.sh`; beim Start laeuft `/etc/profile` |

Jede Befehlszeile ist eine Prozessgruppe: Strg+C beendet die laufende Gruppe, nicht die Shell. Variablen gelten nur in
der Shell (Programme bekommen keine Umgebung).

### Programme

Grafische Programme laufen unter dem Desktop im Fenster, ohne Desktop im Vollbild. Auch aus dem Terminal gestartet
bekommen sie ein Fenster (`snake &`, `view bild.bmp`).

| Art | Programme |
|---|---|
| **Desktop-Apps** | `term` (Terminal), `files` (Dateien), `textedit` (Texteditor), `textview` (Text ansehen), `music` (Musik), `view` (Bilder, BMP), `paint` (Malen), `calc` (Rechner), `clock` (Uhr), `settings` (Einstellungen), `taskmgr` (Task-Manager), `about` (Ueber MiniKernel), `snake`, `tetris`, `gldemo` (3D-Demo) |
| **Dateien und Text** | `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `find`, `tree`, `du`, `df`, `mount`, `grep`, `head`, `tail`, `less`, `wc`, `sort`, `uniq`, `diff`, `hexdump`, `edit` (Editor in der Konsole), `echo`, `seq`, `yes` |
| **System** | `ps`, `kill`, `cpus` (Auslastung je CPU), `uptime`, `date`, `cal`, `dmesg`, `lspci`, `lsusb`, `mouse`, `keymap`, `resolution`, `sleep`, `reboot`, `poweroff` |
| **Netzwerk** | `ifconfig`, `ping`, `nslookup`, `wget`, `ntp`, `netstat`, `udp`, `wlan` |
| **Ton und Bluetooth** | `play` (WAV/MP3 abspielen, Testton, Lautstaerke), `bt` (Bluetooth-Geraete) |
| **Test und Diagnose** | `ramtest` (Arbeitsspeicher), `burn` (CPU-Last), `igdtest` (Intel-Grafik), `anim` (Bildtakt), dazu die Selbsttests `threadtest`, `limittest`, `cowtest`, `mmaptest`, `shmtest`, `gltest`, `fputest`, `memtest`, ... |

Einige im Einzelnen:

- **Dateien** (`files [ordner]`): Seitenleiste mit Schnellzugriff, Orten (`/disk`, `/`, angesteckte Datentraeger) und
  belegtem Speicher; Tabs (Strg+T/W/Tab), Zurueck/Vor/Hoch, klickbare Pfadleiste, Suche (Strg+F; im Schnellzugriff auf
  der ganzen Platte). Liste mit Name, Geaendert, Groesse (sortierbar). Auswahl mit Klick, Strg/Shift+Klick, Pfeilen,
  Strg+A; Strg+C/X/V (auch zwischen zwei Fenstern), Strg+D duplizieren, Strg+N neuer Ordner, Entf loeschen (mit
  Rueckfrage), Rechtsklick-Menue, Ziehen auf einen Ordner verschiebt (mit Strg: kopiert). Kopieren laeuft im
  Hintergrund mit Fortschritt. Doppelklick oeffnet Ordner, Bilder in `view`, Musik in `music`, sonst den Texteditor.
- **Musik** (`music [ordner|datei]`): MP3 und WAV eines Ordners, Titel und Interpret aus ID3, Fortschritt zum
  Anklicken/Ziehen, Zufall, Wiederholen, Titelliste mit Laengen, eigene Lautstaerke. Leertaste Pause, Pfeile 10 s
  springen. Die Tasten einer Bluetooth-Soundbar (Play/Pause/vor/zurueck) wirken auch, wenn ein anderes Fenster vorne ist.
- **Texteditor** (`textedit [datei]`): Zeilennummern, Markieren mit Maus und Shift+Pfeil, Strg+Pfeil wortweise,
  Strg+A/C/X/V, Strg+Z/Y, Strg+S; fragt beim Schliessen nach ungesicherten Aenderungen.
- **Task-Manager** (`taskmgr`, Strg+Shift+Esc): Prozesse mit % CPU (100 % = ein Kern), CPU-Zeit, Threads und Speicher;
  Klick auf eine Spalte sortiert, "Beenden"/Entf beendet nach Rueckfrage. Unten CPU-Last (Benutzer/System) und
  Arbeitsspeicher, im Tab "Leistung" der Verlauf jeder CPU.
- **Ueber MiniKernel** (`about`): wie "Ueber diesen Mac" - Prozessor, Kerne, Arbeitsspeicher, Grafik, Bildschirm,
  Festplatte, virtuelle Maschine, Build-Datum, Laufzeit.
- **ramtest** `[MB] [Runden]`: belegt freien Arbeitsspeicher (128 MB bleiben dem Kernel), schreibt sechs Muster
  (Nullen, Einsen, Schachbrett, laufende Eins, Adresse, Zufall), prueft sie, meldet Fehler mit Adresse und misst die
  Geschwindigkeit. Mehrere gleichzeitig belasten mehrere Kerne.
- **wget**: `wget http://example.com/` speichert `index.html` im aktuellen Verzeichnis, `wget -O - url | less` zeigt
  die Seite, `-S` die Kopfzeilen. Folgt Weiterleitungen, versteht "chunked"; nur `http://`.
- **play**: `play datei.wav` (8-32 Bit, Gleitkomma, jede Abtastrate), `play lied.mp3` (MPEG-1/2/2.5 Layer I-III),
  `play -w lied.wav lied.mp3` wandelt um, `play -t` Testton, `play -v 0-100` Lautstaerke.

### Desktop

`desktop` startet die grafische Oberflaeche (oder `init=/bin/desktop` in der Kommandozeile). Unten sitzt die
Taskleiste aus vier Segmenten aus Milchglas:

| Segment | Inhalt |
|---|---|
| **Programme** | Klick startet bzw. holt das Fenster nach vorn; Strich darunter = laeuft, blau = aktiv. Rechts die minimierten Fenster |
| **Suche, Start, Fenster** | Startmenue mit allen Programmen und Suchfeld (tippen, Enter startet); Fenstermenue mit allen Fenstern und den Befehlen fuers aktive |
| **Uhrzeit und Datum** | Klick oeffnet die Uhr |
| **System** | Netzwerk, Bluetooth, Ton und ^ (Einstellungen, Task-Manager, Ueber MiniKernel, Neu starten, Ausschalten, Zur Konsole) |

- **Netzwerk:** ein Knopf fuer Kabel und WLAN. Zeigt Zustand, Adresse, Gateway, DNS, Geschwindigkeit und Datenmengen;
  "Adresse neu anfragen". Im WLAN-Teil die gefundenen Netze mit Signal und Verschluesselung; Klick verbindet (bei WPA2
  mit Passwortabfrage). Das zuletzt verbundene Netz steht in `wlan.cfg`, der Desktop verbindet sich beim Start.
- **Bluetooth:** Zustand, Geraet, Audio und die gefundenen Geraete (Audio zuerst). Klick verbindet, danach geht aller
  Ton an das Geraet. Das zuletzt verbundene Geraet (`bt_keys.cfg`) wird beim Start wieder verbunden.
- **Ton:** Mausrad ueber dem Symbol regelt die Lautstaerke; das Menue hat einen Regler, "Stumm schalten" und die
  Ausgabe: "Automatisch" (Bluetooth, wenn bereit; sonst Soundkarte mit Auto-Mute) oder fest Lautsprecher, Kopfhoerer,
  Line-Out bzw. Bluetooth. Die Wahl wird gespeichert.
- **Fenster:** runde Ecken, weiche Schatten, Titelleiste mit Minimieren, Maximieren, Schliessen; Ziehen an den
  linken/rechten Rand dockt an die Haelfte an, an den oberen Rand maximiert. Weiche Animationen beim Oeffnen,
  Schliessen, Minimieren und Zoomen. Ab 1300 Pixel Bildschirmhoehe wird alles 25 % groesser.
- **Neu starten / Ausschalten:** alle Programme werden gebeten, sich zu beenden; bleibt eins offen (z.B. ungespeichertes
  Bild), fragt der Desktop nach.

**Tastenkuerzel** (linke Alt-Taste):

| Kuerzel | Wirkung |
|---|---|
| Alt+Tab (mit Shift: zurueck) | naechstes Fenster |
| Alt+W / Alt+Q | Fenster schliessen / Programm beenden |
| Alt+M / Alt+N / Alt+F | minimieren / neues Fenster / zoomen |
| Alt+Pfeil links/rechts | an die Bildschirmhaelfte andocken |
| Alt+Pfeil hoch/runter | maximieren / zurueck |
| Strg+Shift+Esc | Task-Manager |
| Esc | Menue schliessen |

### Einstellungen

`settings` (Startmenue oder ^ → "Einstellungen ..."), gespeichert in `settings.cfg` auf dem Boot-Volume (sonst `/disk`):

- **Anzeige:** Aufloesung aus den Modi des Monitors; mit Intel-Treiber sofort (mit "beibehalten?" und Ruecksprung nach
  15 s), sonst ab dem naechsten Start. Groesse der Oberflaeche (automatisch, 100, 125, 150 %).
- **Zeiger:** Groesse 100-250 %, gilt sofort.
- **Taskleiste:** klein/normal/gross, Uhr mit Sekunden, Datum unter der Uhrzeit.
- **Hintergrund:** fuenf Farbthemen (Abendrot, Ozean, Wald, Lavendel, Graphit).
- **Tastatur:** Layout de/us/uk, sofort und dauerhaft.
- **Info:** Bildschirm, Grafik, Groesse der Oberflaeche, Speicherort der Einstellungen.

### Technik des Desktops

- **Fensterprotokoll** (`Userland/include/winproto.h`): jedes Fenster gehoert einem eigenen Prozess. Der Desktop
  startet ihn mit zwei Pipes (Nachrichten zu 64 Byte: Tasten, Maus, Fokus, Schliessen, Bildtakt bzw. Fenster anlegen,
  geaenderter Bereich, Titel, Datei oeffnen); den Inhalt zeichnet das Programm in geteilten Speicher. Der Desktop
  wartet nie auf ein Programm; wer auf "Schliessen" nicht reagiert, wird beim dritten Klick beendet. Stuerzt ein
  Programm ab, verschwindet nur sein Fenster.
- **Grafikbibliothek** (`gfx.c`): unter dem Desktop liefert `gfx_open()` ein Fenster statt des Bildschirms,
  `gfx_open_window_ex(w, h, titel, GFX_RESIZABLE)` waehlt Groesse und Titel, Ereignisse `EV_CLOSE`, `EV_FOCUS`,
  `EV_RESIZE`. Die Titelleiste zeichnet die Bibliothek selbst; Programme mit eigener Kopfleiste nehmen
  `GFX_FRAMELESS` und `gfx_window_cmd`. Gemeinsames Aussehen: `ui.h`.
- **Dienste** (`SYS_SERVICE`): benannte Verbindungen wie ein einfacher Unix-Socket; der Desktop meldet sich als
  "desktop" an, aus dem Terminal gestartete Grafikprogramme verbinden sich dort.
- **Tasten:** Alt-Kombinationen kommen als `KEY_ALT` + Taste, Shift/Strg mit Sondertasten als `KEY_MODS` + Umschalttasten
  + Taste (`KEY_MOD_*` in `Event.key`) - nur bei Grafikprogrammen, die Konsole verwirft sie.
- **Schriften:** Inter und JetBrains Mono (SIL Open Font License, `/share/fonts`), gerastert mit stb_truetype;
  Zeichnen mit Kantenglaettung und Transparenz in `Userland/lib/draw.c`.

---

## Hardware im Detail

### Netzwerk (Kabel)

- **Treiber** (`Kernel/drivers/net/e1000.c`): Intel 82540EM/82545EM, 82574L, I217-I219. Karten mit MSI melden Pakete
  per Interrupt, die anderen fragt der Thread `net` ab.
- **IPv4-Stack** (`Kernel/net`): ARP, ICMP, DHCP, DNS mit Cache, NTP (stellt beim Start die Uhr), UDP-Sockets und
  TCP-Verbindungen nach aussen.
- **TCP:** Wiederholung nach gemessener Laufzeit, schnelle Wiederholung nach drei doppelten ACKs, 64 KB
  Empfangsfenster, geordneter Abbau im Hintergrund. Programme bekommen eine Verbindung als Datei-Deskriptor
  (`sys_tcp_connect`, dann `read`/`write`).

### WLAN (Intel AX200)

```sh
wlan scan
wlan connect "Mein Netz" "geheimes Passwort"
wlan status          # Zustand, Kanal, Rate, Zaehler; bei Fehlern Schritt und Grund
ifconfig             # wlan0 mit Adresse
wlan disconnect
```

- **Erkennen und Firmware** (`Kernel/drivers/net/iwl.c`): PCI 8086:2723; die Firmware wird zerlegt (TLV wie bei
  Linux: LMAC/UMAC, Paging, Faehigkeiten), geladen und gestartet. `wlan` zeigt alles.
- **Verbinden** (`iwl_sta.c`, `Kernel/net/wpa.c`): offene und WPA2-PSK-Netze (auch WPA2/WPA3-gemischt), danach
  `wlan0` im Netzwerk-Stack mit DHCP. Ablauf wie iwlmvm in Linux: PHY-, MAC-Kontext, Bindung, Station, Sende-
  warteschlangen, Authentifizierung, Assoziierung, 4-Wege-Handshake (PBKDF2, PRF, AES Key Wrap in
  `Kernel/lib/crypto.c`); die Firmware ver- und entschluesselt CCMP selbst. Gruppenschluessel-Wechsel beantwortet der
  Thread `wlan`. Befehlsversionen der Firmware `cc-a0-77`.
- **Grenzen:** 802.11a/g-Station ohne HT/VHT/HE, QoS und Aggregation; feste Rate nach Signalstaerke (hoechstens
  54 Mbit/s); Empfang wird abgefragt (kein Interrupt). Nicht unterstuetzt: WEP, WPA1/TKIP, WPA3-only (SAE), Enterprise,
  Pflicht-MFP.
- **Test:** Selbsttest `crypto` (17 Pruefungen: RFC- und 802.11-Testvektoren, kompletter Handshake gegen einen
  simulierten AP, auch mit falschem Passwort). Das Verbinden selbst nur auf echter Hardware.

### Bluetooth (Intel AX200 am USB)

```sh
bt                   # Geraet, Firmware-Version
bt scan [s]          # Firmware laden (falls noetig), Geraete suchen
bt connect NAME      # koppeln, verbinden, A2DP einrichten
bt status
bt disconnect
```

| Stufe | Was passiert |
|---|---|
| USB (`btusb.c`) | Geraet 8087:0029: Interrupt-IN fuer HCI-Ereignisse, Bulk fuer Daten, Befehle ueber Endpunkt 0; Intel-Version und Boot-Parameter |
| Firmware | `ibt-20-1-3.sfi` per Secure Send, Start per Intel Reset; DDC-Einstellungen aus `.ddc` |
| Suche | klassisch (Inquiry mit RSSI/Namen) und LE gleichzeitig |
| Verbindung (`btconn.c`) | ACL mit Flusskontrolle, Secure Simple Pairing ("Just Works") bzw. PIN 0000, Verschluesselung, L2CAP, AVDTP-Endpunkte; Schluessel in `bt_keys.cfg` |
| A2DP (`a2dp.c`, `sbc.c`) | SBC 48 kHz Stereo, Bitpool 53, RTP; der Mischer (`hda.c`) spielt alle Programme ueber Bluetooth statt ueber die Soundkarte; nach 2 s Stille Suspend |
| AVRCP | Tasten der Soundbar: lauter/leiser/stumm direkt, Play/Pause/Stop/vor/zurueck an `music` (`SYS_BT 10`) |

Der SBC-Encoder rechnet nur mit Ganzzahlen und ist gegen den Decoder von BlueZ (libsbc) geprueft (Selbsttest `sbc`,
bitgenau gegen den Host).

### Intel-Grafik (Gen9: Skylake bis Comet Lake, z.B. UHD Graphics 630)

`Kernel/drivers/gpu/igd*.c` setzt auf der Anzeige der UEFI-Firmware auf. Ohne Intel-GPU (QEMU, VMware, andere
Grafikkarten) bleibt der Framebuffer der Firmware; `noigd` schaltet den Treiber ab.

**Anzeige**

- **Hardware-Mauszeiger** als eigene Ebene der Pipe.
- **Doppelpufferung:** ganze Bilder werden beim Bildwechsel umgeschaltet (kein Tearing).
- **Moduswechsel im Betrieb** (`igd_mode.c`, `igd_dp.c`): Modi aus der EDID; HDMI bis 300 MHz Pixeltakt, DisplayPort
  mit der eingemessenen Verbindung (z.B. 3440x1440 mit 100 Hz). `resolution` listet und schaltet
  (`resolution 2560x1440@60`) und speichert fuer den naechsten Start.
- **Blitter** (`igd_blt.c`): kopiert Teil-Updates im Hintergrund in den Bildspeicher (`bltmode=N`, `noblt`).
- **Bildwechsel-Interrupt** (`igd_irq.c`): `gfx_vsync()` laeuft genau im Takt des Monitors (in QEMU 10-ms-Pause).
- **Anschluss wechseln:** wird der Monitor abgezogen, wechselt das Bild auf einen anderen Anschluss (mit
  DP-Link-Training); `nohotplug` schaltet das ab.

**Beschleunigung**

- **Render-Engine und EUs** (`igd_rcs.c`): Befehlsprozessor im Ring-Modus, eigene Programme auf den Recheneinheiten
  (eigener kleiner Assembler fuer die EU-Maschinensprache).
- **3D-Pipeline:** Vertex- und Pixel-Shader, Tiefenpuffer (D32_FLOAT, Y-Kacheln), Texturen mit Sampler (naechster
  Texel oder bilinear), Rueckseiten weglassen, Mischen (`BLEND_STATE`). `igdtest 3d` zeigt einen texturierten,
  beleuchteten Wuerfel.
- **OpenGL fuer Programme** (`Userland/lib/gl.c`, `gl.h`, Demo `gldemo`): `glBegin`/`glEnd`, Matrix-Stapel,
  `gluPerspective`/`glFrustum`/`glOrtho`, Texturen, Licht (`GL_LIGHT0`), Tiefentest, `GL_CULL_FACE`, `GL_BLEND`,
  Puffer und Vertex-Arrays (OpenGL 1.5: `glBufferData`, `glDrawArrays`, `glDrawElements`). Die GPU zeichnet direkt in
  das Fensterbild; `gl.c` schneidet Dreiecke an naher/ferner Ebene und am Bildrand ab. Ohne Intel-GPU rechnet `gl.c`
  dasselbe mit der CPU (ein Thread je CPU; `gldemo -cpu` zum Vergleich). Selbsttest `gltest` (28 Pruefungen).
- **Desktop auf der GPU** (`igd_comp.c`, Desktop `gpu.c`): Hintergrund, Fenster, Schatten, runde Ecken und Animationen
  setzt die GPU zusammen; Taskleiste und Menues zeichnet die CPU als eigene Ebene. Beim Start prueft ein Selbsttest die
  GPU gegen die CPU, sonst setzt die CPU zusammen. Auf dem Test-PC etwa doppelt so schnell wie mit der CPU.
  `gpucomp=off` / `gpucomp=soft` (dieselben Auftraege rechnet die CPU - zum Testen in QEMU).

**Testen** (nur auf echter Hardware): `igdtest` (Page-Flipping), `igdtest cursor|blit|info|edid|scale|mode|dp|dpmode|
output|vblank|render|gpgpu|3d|comp`; Messwerte stehen in `dmesg`.

### Ton (Intel High Definition Audio)

- **Treiber** (`Kernel/drivers/sound/hda.c`): HDA-Controller (PCI 04.03), Codecs ueber CORB/RIRB; Wege von jedem
  analogen Ausgang (Kopfhoerer, Line-Out, Lautsprecher) zu einem DAC; Auto-Mute, wenn an einer Buchse etwas steckt.
- **Mischer:** bis zu 8 Programme gleichzeitig, jedes mit eigener Stimme (auf 48 kHz umgerechnet); ein Thread haelt
  den DMA-Ring etwa 60 ms voraus gefuellt, ohne Ton schlaeft er. Ausgabe waehlbar (`SYS_AUDIO` 7-9), auch Bluetooth.
- **Programme:** `#include "sound.h"`, `snd_open()`, `snd_tone(hz, ms, lautstaerke)` (Tetris, Snake); beliebige
  16-Bit-Daten ueber `SYS_AUDIO`. MP3 dekodiert [minimp3](https://github.com/lieff/minimp3) (CC0).
- **In QEMU:** `make run SOUND=pa` (hoerbar) oder `SOUND=wav` (Aufnahme in `Build/sound.wav`).

### USB und Datentraeger

- **USB** (`Kernel/drivers/usb`): xHCI mit Hubs, Tastatur und Maus (HID), Massenspeicher (Bulk-Only), Bluetooth.
  Transfers warten mit MSI schlafend - ein lesender Stick haelt Maus, Tastatur und Ton nicht an.
- **Platten** (`Kernel/drivers/block`): NVMe, AHCI, virtio-blk, Partitionen MBR und GPT.
- **Dateisysteme** (`Kernel/fs`): FAT12/16/32 mit langen Namen (VFAT) und exFAT, lesen und schreiben; `fsro` bindet
  fremde Volumes nur lesbar ein. Die initrd (tar) ist nur lesbar und enthaelt `/bin`, `/etc`, `/share`, `/firmware`.

---

## Kernel im Detail

### Mehrere CPUs (SMP)

Der Kernel startet alle CPUs aus der ACPI-MADT. Threads und Prozesse kommen aus einer gemeinsamen Run-Queue und
laufen auf jeder CPU. Kernel-Code ist durch einen Big Kernel Lock geschuetzt (immer nur eine CPU im Kernel), einige
Syscalls (`brk`, `mmap` mit einem Thread, Futex, ...) laufen ohne ihn; User-Programme rechnen echt parallel. Details:
`Kernel/arch/x86_64/smp.h`. Probieren: `burn 5000 & burn 5000 & cpus` oder der Task-Manager.

Jeder Timer-Tick (10 ms) zaehlt fuer die CPU (Benutzer, Kernel, Leerlauf) und fuer den Prozess des laufenden Threads
(`tick_sink`); daraus rechnen `cpus`, `ps` und der Task-Manager die Auslastung.

### Threads in Programmen

Ein Programm kann bis zu 1024 Threads haben (`Userland/include/thread.h`). Sie teilen Speicher, Deskriptoren und
Arbeitsverzeichnis:

```c
#include "thread.h"

static Mutex lock = MUTEX_INIT;
static void *arbeit(void *arg) { mutex_lock(&lock); /* ... */ mutex_unlock(&lock); return arg; }

int t = thread_create(arbeit, &daten);   // laeuft parallel, eigener Stack (64 KiB)
void *ergebnis;
thread_join(t, &ergebnis);               // wartet aufs Ende und gibt den Stack frei
```

- **Sperren** (`Mutex`) warten ueber einen Futex: wer nicht drankommt, schlaeft. `malloc`/`free` sperren, sobald ein
  zweiter Thread laeuft.
- **Ende:** `sys_exit`, ein Absturz oder Strg+C/`kill` beenden alle Threads; `thread_exit` nur den eigenen.
- **fork** uebernimmt nur den aufrufenden Thread, **exec** geht nur mit einem Thread.
- Ausgeblendete Seiten werden erst frei, wenn jede betroffene CPU ihren TLB geleert hat (IPI).
- Genutzt von `gl.c` (CPU-Rasterer mit einem Thread je CPU) und `files` (Kopieren im Hintergrund). Selbsttest
  `threadtest` (22 Pruefungen).

### Speicher

- **fork mit Copy-on-Write:** das Kind bekommt dieselben Frames, beschreibbare Seiten werden schreibgeschuetzt
  (`PAGE_COW`); erst wer schreibt, bekommt eine Kopie. `fork` + `exec` kopiert so gar nichts (bei 32 MiB etwa 7 ms
  statt 780 ms ohne KVM). Selbsttest `cowtest`.
- **Dateien einblenden:** `sys_mmap_file(fd, laenge, offset, schreibbar)`; gelesen wird erst beim Zugriff
  (Seitenfehler, bis 64 KiB am Stueck), privat wie `MAP_PRIVATE`. Genutzt von `play`, `music`, den Schriften und
  `bmp_load`. Selbsttest `mmaptest`.

  ```c
  int fd = sys_open("/disk/musik.wav", O_RDONLY);
  const unsigned char *d = (const unsigned char *)sys_mmap_file(fd, groesse, 0, 0);
  sys_close(fd);                 // die Einblendung bleibt
  ... d[i] ...                   // liest beim ersten Zugriff die passenden 4 KiB
  sys_munmap((void *)d, groesse);
  ```
- **Geteilter Speicher** (`SYS_SHM`): eigenes PTE-Bit, damit `fork` ihn nicht kopiert und er nicht doppelt
  freigegeben wird (Selbsttest `shmtest`).
- **Stack** waechst bei Bedarf bis 8 MiB.
- **Gleitkomma:** Programme duerfen `float`/`double` und SSE nutzen; der Kernel selbst nicht. Beim Threadwechsel
  sichert er die Register (`fxsave`/`fxrstor`). Selbsttest `fputest`.

### Grenzen

Die Tabellen wachsen bei Bedarf; feste Grenzen gibt es nur gegen Ausreisser (Selbsttest `limittest`, 25 Pruefungen):

| | Grenze |
|---|---|
| Prozesse gleichzeitig | 4096 |
| offene Deskriptoren je Prozess | 1024 |
| Threads je Prozess | 1024 |
| Datei-Einblendungen / geteilter Speicher je Prozess | 1024 / 1024 |
| geteilte Speicherobjekte im System | 4096 |
| Kernel-Threads im System | 65536 |
| Stack | waechst bis 8 MiB |
| Kommandozeile | 256 Woerter, 4 KiB (Shell-Eingabezeile 2048 Zeichen) |
| Pipe in der Shell / Hintergrund-Jobs | 64 / 64 |
| Pfadlaenge | 1023 Zeichen (laenger: `ERR_NAMETOOLONG`; initrd 256) |
| Zwischenablage | 4 MiB |
| benannte Dienste / wartende Verbindungen / Name | 256 / 256 / 63 Zeichen |
| Fenster im Desktop / gerade startende Programme | 128 / 32 |
| GPU-Flaechen zum Zusammensetzen | 640 |

Prozess-Eintraege und Thread-Bloecke werden nie freigegeben, sondern wiederverwendet: so koennen Interrupts (Strg+C)
die Listen ohne BKL durchgehen.

---

## Fehlersuche und Tests

- **Selbsttests:** `make test` (alle Gruppen, derzeit 288 Pruefungen) oder `make test TESTS=disk,user`; nach jeder
  Gruppe wird der Kernel-Heap geprueft. `make test DISK=usb` testet zusaetzlich USB-Massenspeicher.
- **Backtraces:** eine Exception im Kernel gibt die Aufrufkette mit Funktionsnamen aus:
  ```
  *** EXCEPTION 14: Page Fault ***
    Aufrufkette:
      0x10427d test_paging+0x17d
      0x10a62c run_selftests+0x86c
      0x12bf17 kmain+0x257
  ```
  Ist der Heap beschaedigt, nennt `heap_check` die Funktion, die den Block davor angelegt hat.
- **gdb:** `make debug` startet QEMU angehalten mit gdb-Server, `make gdb` im zweiten Terminal verbindet sich (Symbole
  aus `Build/kernel.debug.elf`, Breakpoint auf `kmain`). Adressen von Hand: `addr2line -f -e Build/kernel.debug.elf 0x...`.
- **Haenger finden:** stockt das System (Mauszeiger, Ton), stehen die Gruende im Log:
  `dmesg | grep -e wartete -e stockte -e dauerte -e Rueckstand`
  - `smp: CPU n wartete X ms auf den BKL - gehalten von Thread '...'`
  - `usb: vvvv:pppp Bulk/Control ... dauerte X ms`
  - `sched: System stockte etwa X ms` (Thread "wachhund")
  - `hda: Bluetooth-Mischer ... im Rueckstand`
- **Auf echter Hardware:** `dmesg > /disk/log.txt`, Arbeitsspeicher mit `ramtest`, Auslastung im Task-Manager.

---

## Kernel-Kommandozeile

Der Bootloader liest `\cmdline.txt` von der EFI-Systempartition (bei QEMU aus `CMDLINE`/`TESTS` erzeugt).

| Option | Bedeutung |
|---|---|
| `mode=1600x900`, `mode=max` | Grafikmodus (naechstliegende Aufloesung bzw. groesste); ohne: Modus der Firmware |
| `igdmode=2560x1440@60` | Intel-Grafik: diesen Modus des Monitors beim Start setzen (`resolution` traegt ihn ein) |
| `scale=1..4` | Schriftvergroesserung der Konsole |
| `kbd=us\|de\|uk` | Tastaturlayout |
| `tz=eu\|uk\|utc\|+2\|+5:30` | Zeitzone der Uhr |
| `init=/bin/...` | erstes Programm statt `/bin/sh` (z.B. `init=/bin/desktop`) |
| `verbose` | Meldungen von Bootloader und Kernel auf dem Bildschirm statt der Startanimation |
| `ip=192.168.1.50/24,192.168.1.1[,dns]` | feste Adresse fuer eth0 statt DHCP; `nodhcp`, `nonet`, `nontp` schalten ab |
| `fsro` | fremde Volumes nur lesbar einbinden |
| `nosmp`, `cpus=N` | nur die Boot-CPU bzw. hoechstens N CPUs benutzen |
| `noigd`, `nohotplug`, `noblt`, `bltmode=N`, `gpucomp=off\|soft` | Intel-Grafik abschalten bzw. einzelne Teile |
| `selftest`, `selftest=gruppe,...`, `keep` | Selbsttests (siehe `Kernel/tests/selftest.c`) |

**Startbild:** ohne `verbose` zeigt der Bootloader das Logo der Firmware (ACPI-Tabelle BGRT, wie Windows und Linux),
der Kernel darunter einen Ladebalken wie bei macOS (`Kernel/console/splash.c`). Das Bild bleibt, bis das erste
Programm etwas zeigt - so geht es direkt in den Desktop ueber. Meldungen des Kernels kommen danach nur noch seriell und
in `dmesg` (wie `quiet` bei Linux); bei einer Exception und nach 60 s ohne Programm erscheinen alle.

---

## Aufbau des Projekts

```
Sources/main.c        UEFI-Bootloader: laedt kernel.elf, initrd.tar, cmdline.txt; Grafikmodus; Startlogo
Includes/boot_info.h  Uebergabe Bootloader -> Kernel

Kernel/
  arch/x86_64/        Einstieg, GDT/IDT, Interrupts, APIC/IOAPIC, ACPI, Ausschalten, Syscall-Einstieg, SMP
  mm/                 physischer Speicher, Paging (Copy-on-Write), Heap, Kernel-Stacks
  core/               kmain (kernel.c), Prozesse, Scheduler, Syscalls, TTY, Kommandozeile, Dienste
  console/            Textkonsole im Framebuffer, Schriften, Startbild
  drivers/            PCI, serielle Schnittstelle, Uhr, Tastatur, Maus
    block/            AHCI, NVMe, virtio-blk, Partitionen (MBR/GPT)
    usb/              xHCI, Tastatur/Maus (HID), Massenspeicher
    net/              Intel e1000/e1000e, WLAN Intel AX200 (iwl.c, iwl_sta.c)
    bt/               Bluetooth: USB-Transport, Verbindung (L2CAP, AVDTP, AVRCP), A2DP, SBC-Encoder
    gpu/              Intel-Grafik Gen9: Anzeige, Modi, DisplayPort, Blitter, Render-Engine, Zusammensetzen
    sound/            Intel HD Audio mit Mischer
  fs/                 VFS, Dateisystem-Schicht; fat/: FAT12/16/32 und exFAT
  net/                IPv4-Stack: ARP, ICMP, DHCP, UDP, TCP, DNS, NTP; WPA2-Handshake (wpa.c)
  lib/                string, kprintf, UTF-8, Kryptografie (SHA-1, HMAC, PBKDF2, AES, Key Wrap)
  tests/              Selbsttests, je Gruppe eine Datei

Userland/
  include/            Syscalls (user.h), libc, malloc, gfx (Grafik), ui (Aussehen), gl (OpenGL), sound, thread,
                      settings, winproto (Fensterprotokoll), util
  lib/                libuser.a
  bin/                je Programm eine Datei (NAME.c) oder ein Ordner (sh/, desktop/, music/, play/) -> /bin/NAME

Initrd/               wird 1:1 in die initrd kopiert (/etc/profile, /etc/motd, Test-Skripte)
firmware/             Firmware fuer WLAN und Bluetooth (lokal, nicht im Repository)
tools/                Images erzeugen (mkdisk, mkesp, mkstick, mkvmx), FAT pruefen (fatcheck), Schriften, Symbole
```

Build-Ergebnisse landen in `Build/` (Objektdateien, `kernel.debug.elf`, `esp.img`, `Out.log`) und `Image/`
(`bootx64.efi`, `kernel.elf`, `initrd.tar`, Datenplatte).

---

## Was noch fehlt

Grob nach Nutzen sortiert. ✳ = laesst sich komplett in QEMU entwickeln und testen.

**Netzwerk und Internet**

- [ ] HTTPS/TLS (fuer `wget` und spaeter einen Browser) ✳
- [ ] TCP-Server (`listen`/`accept`) - Voraussetzung fuer Webserver, Fernzugriff (VNC), Dateiuebertragung ✳
- [ ] IPv6 ✳
- [ ] Weitere Netzwerkkarten (Realtek RTL8111, virtio-net) ✳
- [ ] WLAN schneller: Ratenanpassung der Firmware (TLC), 802.11n/ac, Empfang per Interrupt
- [ ] WLAN: WPA3 (SAE), Enterprise

**Hardware**

- [ ] Installation auf die interne SSD (Bootloader und System ohne Stick)
- [ ] Bluetooth-Tastatur und -Maus (HID), Freisprechen (HFP), Bluetooth LE
- [ ] USB-Audio, Gamepads ✳
- [ ] Energiesparen (CPU-Takt, Ruhezustand), Akku-Anzeige
- [ ] Mehrere Monitore gleichzeitig
- [ ] Grafik fuer AMD/NVIDIA (bisher nur Framebuffer der Firmware)
- [ ] Mikrofon/Aufnahme, Ton ueber HDMI

**Kernel**

- [ ] Feinere Sperren statt des Big Kernel Lock ✳
- [ ] Signale (SIGINT, SIGTERM, SIGCHLD, ...) und Umgebungsvariablen fuer Programme ✳
- [ ] Benutzer, Rechte, Dateirechte ✳
- [ ] Schreib-Cache fuer Datentraeger ✳
- [ ] Weitere Dateisysteme: ext4, NTFS (z.B. Windows-Partitionen lesen) ✳
- [ ] Absturzberichte von Programmen als Datei ✳

**Programme und Oberflaeche**

- [ ] PNG und JPEG (Bildansicht, Hintergrundbilder) ✳
- [ ] Webbrowser (einfaches HTML, braucht TLS) ✳
- [ ] Skriptsprache (Lua oder MicroPython) und ein Compiler im System (TCC) ✳
- [ ] DOOM-Port ✳
- [ ] Sperrbildschirm, Benachrichtigungen, Drag & Drop zwischen Programmen ✳
- [ ] Lautstaerke je Programm im Ton-Menue ✳
- [ ] Shell: Jobsteuerung mit Strg+Z, Rueckwaertssuche (Strg+R), Aliase ✳
- [ ] Screenshots (Druck-Taste) ✳

**3D**

- [ ] Mip-Maps, Auftraege ohne Warten
- [ ] Shader fuer Programme, langfristig Mesa
