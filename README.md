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
| `SOUND=none\|wav\|pa\|off` | Soundkarte (Intel HD Audio): Ton ins Leere, nach `Build/sound.wav`, ueber PulseAudio (unter WSLg die Windows-Lautsprecher) oder ohne Karte |
| `STICK=12\|16\|exfat` | zusaetzlichen Test-Stick (FAT12/FAT16/exFAT) am USB anschliessen |
| `TESTS=1` / `TESTS=disk,user` | Selbsttests beim Start (alle bzw. nur diese Gruppen); `KEEP=1` bleibt danach im System |
| `CMDLINE="..."` | weitere Kernel-Kommandozeile, siehe unten |
| `HEADLESS=1` | ohne Fenster, Ausgabe nur in `Build/Out.log` (z.B. `make test HEADLESS=1`) |
| `QEMU_EXTRA="..."` | weitere QEMU-Argumente |

Beispiele: `make run CMDLINE="mode=1600x900 kbd=de"`, `make test TESTS=foreign STICK=exfat`, `make run DISK=nvme`.

Die Datenplatte `Image/disk.img` (FAT32, Label `MINIKERNEL`, im System unter `/disk`) bleibt bei `make clean` erhalten;
`make cleandisk` legt sie neu an, `make fatcheck` prueft sie.

## Netzwerk

Treiber fuer Intel-Netzwerkkarten (82540EM/82545EM, 82574L, I217-I219); Karten mit MSI melden Pakete per Interrupt,
die anderen fragt der Thread `net` ab. Darueber ein IPv4-Stack (`Kernel/net`): ARP, ICMP, DHCP, DNS mit Cache, NTP
(stellt beim Start die Uhr), UDP-Sockets und TCP-Verbindungen nach aussen. TCP: Wiederholung nach gemessener
Laufzeit, schnelle Wiederholung nach drei doppelten ACKs, 64 KB Empfangsfenster, geordneter Abbau im Hintergrund nach
`close`. Programme bekommen eine Verbindung als Datei-Deskriptor (`sys_tcp_connect`, dann `read`/`write`).

Programme: `ifconfig`, `ping`, `nslookup`, `ntp`, `udp`, `netstat` (TCP-Verbindungen) und `wget`:
`wget http://example.com/` speichert `index.html` im aktuellen Verzeichnis (also z.B. erst `cd /disk`),
`wget -O - url | less` zeigt die Seite, `-S` die Kopfzeilen der Antwort, `-q` keine Meldungen. Folgt Weiterleitungen,
versteht Content-Length und "chunked"; nur `http://` (fuer `https://` fehlt TLS).

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
- **Blitter fuer die Bild-Updates** (`igd_blt.c`): die Blitter-Engine kopiert die Teil-Updates der Grafikprogramme
  (z.B. des Desktops) im Hintergrund in den Bildspeicher, statt der CPU; das Programmbild wird dafuer in die GGTT
  eingeblendet (nur wenn es sich aendert). Ganze Bilder kopiert die CPU (sie ist dabei schneller). Entscheidend ist die
  Cache-Steuerung der Engine (MOCS): mit den Werten der Firmware blieben Schreibzugriffe im Cache haengen (Striche mit
  altem Inhalt), mit "uncached" kommen sie gleich im RAM an, aus dem die Anzeige liest. `igdtest bltmode N` schaltet
  im Betrieb um (0 aus, 1-4 Varianten mit Strichen, 5 Standard, 6 wie 5 mit Zurueckschreiben aus dem CPU-Cache);
  `bltmode=N` bzw. `noblt` in der Kommandozeile. Bei jedem Umschalten prueft ein Selbsttest, ob die GPU die frisch
  geschriebenen Daten der CPU sieht
- **Render-Engine** (`igd_rcs.c`, erster Schritt Richtung 3D-Beschleunigung, nur auf Befehl): `igdtest render`
  startet den Befehlsprozessor der Render-Engine im Ring-Modus und prueft Leerauftrag, `PIPE_CONTROL` (Schreiben nach
  getaner Arbeit), Batch-Buffer und den Zeitstempel der GPU (1000 Befehle). Haengt sie, wird sie zurueckgesetzt.
  `igdtest gpgpu` laesst das erste Programm auf den Recheneinheiten (EUs) laufen: GPGPU-Pipeline (wie der Fuelltest von
  IGT), Kernel in EU-Maschinensprache (eigener kleiner Assembler) fuellen, kopieren und mischen Flaechen (gegen die
  CPU geprueft, an der Konsole zusaetzlich ein halbtransparentes Farbfeld). Ring, Status- und Befehlsseiten bleiben
  nach dem ersten Test fest in der GGTT; vor jedem Test wird die Engine zurueckgesetzt und vor jedem Auftrag verwirft
  ein `PIPE_CONTROL` TLB und Caches - so laufen die Tests beliebig oft hintereinander
- **3D-Pipeline** (`igd_rcs.c`, `igdtest 3d`, Stufe 7): erstes Rechteck ueber die 3D-Pipeline, aufgebaut wie IGTs
  rendercopy fuer Gen9 - Vertex-Buffer mit drei Eckpunkten (RECTLIST in Bildschirmkoordinaten), Vertex-Shader aus,
  Clipper durchlassen, Rasterizer ohne Culling, Pixel-Shader (SIMD16, eigener Assembler), der eine feste Farbe per
  Render-Target-Write schreibt. Die CPU prueft die Testflaeche, die Pipeline-Statistik (Eckpunkte, Clipper,
  Pixel-Shader) zeigt, wie weit die GPU kam. Danach ein Dreieck mit Vertex-Shader (SIMD8: Position mal 2x3-Matrix,
  Ausgabe per URB-Write: Kopf, Position, Farbe) und Pixel-Shader, der die Farbe der Ecken interpoliert (`pln`); die
  CPU prueft Flaeche, Mitte und Ecken. An der Konsole dreht sich danach ein Dreieck mit Farbverlauf (128 Bilder;
  Sinus aus einer Tabelle, Festkomma in float-Bitmuster umgerechnet, der Kernel hat keine FPU)
- **Zusammensetzen auf der GPU** (`igd_comp.c`, `SYS_GPUCOMP`; Desktop: `gpu.c`): Bildschirmbild, Hintergrund,
  Fensterbilder und Schatten liegen in geteiltem Speicher, den der Kernel fest in die GGTT einblendet (eigener Bereich,
  Referenz auf das shm-Objekt, solange angemeldet). Je Bild schickt der Desktop alle geaenderten Rechtecke als eine
  Liste von Auftraegen: Hintergrund kopieren, je Fenster Schatten (vier vorberechnete Streifen) und die Zeilen mit den
  runden Ecken mischen (Deckung in Byte 3 des Fensterbildes), den Rest kopieren. Gemessen auf echter Hardware: die GPU
  rundet die Basisadresse einer Flaeche auf 32 Byte und die Zeilenlaenge auf 64 Byte ab und schneidet Bloecke am Rand
  nicht ab. Deshalb sind es immer ganze Flaechen ab ihrem Anfang (Ecke des Rechtecks in den Konstanten des Kernels),
  nur mit Breiten in Vielfachen von 16 Pixeln (der Desktop rundet Fensterbilder und Schatten auf), und jeder Auftrag
  wird in Stuecke zerlegt, in denen die Bloecke der Threads genau aufgehen (innen 8 x 8 Pixel, Raender 1 x 8, 8 x 1,
  1 x 1 - je ein eigener Kernel). Animationen (oeffnen, schliessen, minimieren, maximieren) laufen ebenfalls auf der
  GPU: zwei weitere Kernel skalieren in zwei Durchgaengen (senkrecht: Zeilen 8 x 1 lesen, als 8 x 8 schreiben;
  waagerecht: Spalten 1 x 8), Schrittweite 8.8 aus den Konstanten; danach mischt der Mischen-Kernel mit der Deckung der
  Animation (r1.4, 256 = unveraendert). Die Deckung der runden Ecken steht in Byte 3 des Fensterbildes (sonst 255) und
  wird mitskaliert; die Schattenstreifen werden nur entlang der Kante gestreckt, ihre Ecken bleiben unveraendert.
  Taskleiste, Menues, Dialog und Andock-Vorschau liegen in einer eigenen Ebene: die CPU zeichnet sie nur, wenn sie sich
  aendern, und zwar zweimal (auf Schwarz und auf Weiss) - aus dem Unterschied ergeben sich Deckung und Farbe je Pixel;
  die GPU mischt die Ebene bei jedem Bild ueber die Fenster - nur in den Zonen, wo gerade etwas liegt (Taskleiste,
  offenes Menue, Dialogkasten, Andock-Vorschau), und nur dort zeichnet die CPU sie auch. Beim Darueberfahren der
  Taskleiste werden nur das alte und das neue Feld samt Namen neu gezeichnet; das Abdunkeln beim Dialog macht die GPU
  mit einer festen halbdurchsichtigen Flaeche (Reihenfolge wie bei der CPU). Am Ende jeder Liste steht "anzeigen" (die GPU kopiert die
  Rechtecke uncached in den angezeigten Puffer), der Desktop wartet nicht: abgewartet wird erst vor dem naechsten Bild
  bzw. bevor CPU oder Blitter in den Bildspeicher schreiben oder eine Flaeche abgemeldet wird. Die GPU-Zeit misst der Zeitstempel der Render-Engine. Cache: die Render-Engine hat eigene Eintraege fuer "uncached" und "write-back im
  LLC" (L3 der GPU fuer beide aus, damit sie keine alten Fensterinhalte sieht). Standard ist "alles im Cache" (auf
  dem Test-PC 8,2/6,1 GB/s kopieren/mischen gegen 4,7/3,4 uncached, Desktop doppelt so schnell wie mit der CPU) -
  aber nur, wenn ein zweiter Selbsttest zeigt, dass der Blitter das von der GPU in den Cache geschriebene Bild beim
  Anzeigen richtig liest (sonst "Quellen im Cache", sonst uncached). Beim Start misst der Kernel alle drei Modi
  (2048 x 1024); `igdtest comp cache N` (0 uncached, 1 Quellen im Cache, 2 alles) wechselt im Betrieb, vorher laufen
  die Selbsttests.
  Taskleiste, Menues und Animationen zeichnet weiter die CPU. Beim Start prueft ein Selbsttest die GPU gegen die CPU
  (ungerade, ueberlappende Rechtecke: jeder Auftrag allein, dann alle als Liste, sonst einzeln nacheinander; ohne und
  mit Zurueckschreiben der CPU-Caches); geht nichts davon, setzt die CPU zusammen wie bisher. `igdtest comp` zeigt Zustand und Messwerte (GPU gegen CPU je Mpx),
  `igdtest comp off/on` schaltet im Betrieb um; Kommandozeile `gpucomp=off` bzw. `gpucomp=soft` (dieselben Auftraege
  rechnet die CPU im Kernel - zum Testen in QEMU)
- **Bildwechsel-Interrupt** (`igd_irq.c`): die Pipe meldet jeden Bildwechsel per MSI. Grafikprogramme warten mit
  `gfx_vsync()` darauf und laufen so genau im Takt des Monitors (Tetris, Snake, Desktop; `anim` zeigt es), die
  Doppelpufferung wartet darauf statt nachzusehen. Ohne Interrupt (QEMU) ersetzt eine 10-ms-Pause den Takt
- **Anschluss wechseln im Betrieb:** ein Thread prueft jede Sekunde den aktiven Anschluss. Wird der Monitor
  abgezogen, legt der Treiber das Bild auf einen anderen angeschlossenen Monitor (DisplayPort bevorzugt) und schaltet
  diesen Anschluss ganz ohne Firmware ein (Strom, Pegel, DPLL, bei DP Link-Training); wird DP neu eingesteckt, misst
  er die Verbindung neu ein. `nohotplug` in der Kommandozeile schaltet das ab, `igdtest output [b|c|d]` testet es

`noigd` in der Kommandozeile schaltet alles ab. `igdtest`, `igdtest cursor` und `igdtest blit` pruefen
Page-Flipping, Mauszeiger und Blitter einzeln und schreiben Messwerte ins Kernel-Log (`dmesg`). `igdtest info`
zeigt, wie viele Bild-Updates es seit dem Start gab und was sie gekostet haben, und vergleicht die Kopierwege fuer
ganze Bilder. `igdtest edid`, `igdtest scale` und `igdtest mode` pruefen Monitordaten, Skalierer und
Moduswechsel (HDMI), `igdtest dp` und `igdtest dpmode` dasselbe per DisplayPort. QEMU emuliert keine Intel-GPU: getestet wird
auf echter Hardware (bisher i5-8400T, UHD 630, 3440x1440 ueber HDMI).

## Desktop

`desktop` startet die grafische Oberflaeche (hell). Unten sitzt die Taskleiste aus vier freistehenden Segmenten aus
Milchglas ueber einem berechneten Farbverlauf:

- **Programme:** ein Klick startet bzw. holt das Fenster nach vorn; ein Strich darunter = laeuft, lang und blau = aktives
  Fenster. Rechts hinter einem Trennstrich die minimierten Fenster
- **Suche, Start, Fenster:** Start zeigt alle Programme mit Suchfeld (einfach tippen, Treffer am Anfang zuerst, Enter
  startet, Pfeile waehlen); die Lupe oeffnet dasselbe. Das Fenstermenue listet alle Fenster und die Befehle fuer das
  aktive (minimieren, zoomen, anordnen, schliessen)
- **Uhrzeit und Datum:** ein Klick oeffnet die Uhr
- **System:** Netzwerk, Lautstaerke (Klick: stumm/zurueck, Mausrad: lauter/leiser) und ^ (Ueber MiniKernel, Neu
  starten, Ausschalten, Zur Konsole)

Menues oeffnen sich nach oben ueber ihrem Knopf, Esc schliesst sie. Fenster haben runde Ecken, weiche Schatten und
eine helle Titelleiste mit dem Titel links und rechts Minimieren, Maximieren/Wiederherstellen, Schliessen (wie bei
Windows; Ziehen an der Leiste verschiebt, Doppelklick maximiert). Die Leiste zeichnet die Grafikbibliothek im Fenster
selbst (`gfx.c`), die Programme merken davon nichts: ihr `gfx_screen` beginnt darunter. Programme mit eigener
Kopfleiste oeffnen ihr Fenster mit `GFX_FRAMELESS` (z.B. Dateien mit seinen Tabs): sie zeichnen die Knoepfe dann selbst
und bitten den Desktop mit `gfx_window_cmd` ums Verschieben, Minimieren, Maximieren und Schliessen. Ab 1300 Pixel Hoehe
wird alles um 25 % groesser.

Tastenkuerzel (linke Alt-Taste): Alt+Tab naechstes Fenster (mit Shift zurueck), Alt+W Fenster schliessen, Alt+Q
Programm beenden, Alt+M minimieren, Alt+N neues Fenster, Alt+F zoomen, Alt+Pfeil links/rechts an die Bildschirmhaelfte
andocken, Alt+Pfeil hoch/runter maximieren bzw. zurueck. Fenster, die man an den linken/rechten Rand zieht, docken an
die Haelfte an, am oberen Rand fuellen sie den Bildschirm (mit Vorschau); weggezogen bekommen sie ihre alte Groesse.
Im Systemmenue: Neu starten und Ausschalten mit Rueckfrage - alle Programme werden gebeten, sich zu beenden; bleibt eins
offen (z.B. Malen mit ungespeichertem Bild), fragt der Desktop, ob trotzdem.
Das Netzwerk-Symbol zeigt den Zustand (kraeftig: verbunden, blass: ohne Adresse, durchgestrichen: kein Kabel); ein
Klick oeffnet Karte, Adresse, Gateway, DNS, Geschwindigkeit und die Datenmengen mit aktueller Rate (jede Sekunde neu)
sowie "Adresse neu anfragen (DHCP)".
Alt-Kombinationen kommen als zwei Bytes (`KEY_ALT`/`KEY_ALT_SHIFT`, dann die Taste) und nur bei Grafikprogrammen an
(`KEY_MOD_ALT` in `Event.key`); die Konsole verwirft sie.

Animationen (nach der Uhr, nicht nach Bildern): Fenster blenden beim Oeffnen und Schliessen weich ein und aus, fliegen
beim Minimieren an ihren Platz in der Taskleiste und von dort zurueck, Zoomen gleitet auf die neue Groesse.

Jedes Fenster gehoert einem eigenen Prozess; der Desktop zeichnet nur Rahmen und Taskleiste. Programme:
`term` (Terminal mit Shell), `files` (Dateien; Doppelklick oeffnet Ordner hier, Bilder in `view`, alles andere im
Texteditor), `textedit`, `textview` (nur ansehen), `view`, `calc`, `clock`, `about`, `paint`, `snake`, `tetris`. Ohne
Desktop gestartet, laufen sie im Vollbild.

Dateien (`files [ordner]`): links die Seitenleiste mit Schnellzugriff (Ordner der Platte), den Orten (Platte
`/disk`, System `/`, angesteckte Datentraeger; aufklappbar) und unten dem belegten Speicher. Rechts Tabs (+, Strg+T,
Strg+W, Strg+Tab) und rechts die Fensterknoepfe (das Fenster hat keine Titelleiste), darunter Zurueck/Vor/Hoch, die Pfadleiste (jeder Teil anklickbar,
dazu Aktualisieren) und die Suche (Strg+F; filtert den Ordner, im Schnellzugriff sucht sie auf der ganzen Platte).
Ohne Ordner beginnt es im Schnellzugriff: grosse Ordner der Platte (mit Zeichen fuer Musik, Bilder, Downloads,
Dokumente) und die zuletzt geaenderten Dateien. In einem Ordner die Liste mit Name, Geaendert, Groesse (Klick auf die
Spalte sortiert; Rechtsklick auf freie Flaeche: Neuer Ordner, Neue Textdatei, Einfuegen, Sortieren, Aktualisieren),
unten der freie Platz.
Auswahl mit Klick, Strg+Klick, Shift+Klick, Pfeilen (mit Shift) und Strg+A; Strg+C/X/V kopieren, ausschneiden,
einfuegen (ueber die Zwischenablage, auch zwischen zwei Fenstern), Strg+D duplizieren, Strg+N neuer Ordner, Entf
loeschen (mit Rueckfrage, Ordner samt Inhalt). Rechtsklick oeffnet ein Kontextmenue (auch "Neue Textdatei"). Ziehen
auf einen Ordner oder Ort verschiebt, mit Strg kopiert; zwischen Datentraegern wird kopiert und danach geloescht.
Mausklicks bringen dafuer die gedrueckten Umschalttasten mit (`MouseInfo.kbd_mods`, bei Grafikprogrammen in
`Event.key`). Auf `/disk` gehen lange Namen (VFAT, auch mit Leerzeichen).

Musik (`music [ordner|datei]`): spielt MP3 und WAV eines Ordners. Oben der laufende Titel (Titel und Interpret aus
ID3v2/ID3v1, sonst der Dateiname), Fortschritt zum Anklicken/Ziehen, Zurueck/Abspielen/Weiter, Zufall, Wiederholen;
darunter die Titelliste mit Laengen, unten Ordner wechseln und die Lautstaerke des Programms. Leertaste
Abspielen/Pause, Pfeil links/rechts 10 s zurueck/vor. Doppelklick auf eine MP3/WAV in Dateien oeffnet sie hier.
Gespielt wird in Portionen von etwa 0,25 s ueber eine eigene Stimme im Mischer, die Oberflaeche wartet also nie;
die Pause schickt beim Fortsetzen nach, was im Kernel noch ungespielt lag (keine Luecke). Bei MP3 liest das Programm
nebenbei alle Frame-Koepfe: daraus die genaue Laenge und eine Sprungtabelle (genaues Springen auch ohne feste
Bitrate). Die Laenge in der Liste kommt aus dem Xing/Info-Kopf oder der Bitrate.

Texteditor (`textedit [datei]`): Zeilennummern, Markieren mit Maus (Doppelklick Wort, Dreifachklick Zeile) und
Shift + Pfeil/Pos1/Ende/Bild, Strg + Pfeil wortweise, Strg+A/C/X/V (Zwischenablage des Systems), Strg+Z/Y
rueckgaengig/wiederholen, Strg+S sichern; Werkzeugleiste mit Neu, Oeffnen, Sichern, Sichern unter. Beim Schliessen
mit ungesicherten Aenderungen fragt er nach. Gespeichert wird z.B. auf `/disk`, mit langen Namen (`/disk/Meine Notizen.txt`).
Shift und Strg kommen mit Sondertasten als `KEY_MODS`, Umschalttasten, Taste an (`KEY_MOD_SHIFT`/`KEY_MOD_CTRL` in
`Event.key`), solange ein Grafikprogramm den Bildschirm hat; Strg+V fuegt dann nicht mehr in die Konsole ein, sondern
geht an das Programm. Stuerzt ein Programm ab, verschwindet nur sein Fenster. Grafikprogramme lassen sich auch im Terminal
starten (`snake`, `view bild.bmp`, mit `&` dahinter laeuft das Terminal weiter): sie melden sich beim Desktop und
bekommen ein Fenster.

- Fensterprotokoll (`Userland/include/winproto.h`): der Desktop startet das Programm mit zwei Pipes (Deskriptor 3
  und 4, Nachrichten zu 64 Byte: Tasten, Maus, Fokus, Schliessen, Bildtakt bzw. Fenster anlegen, geaenderter
  Bereich, Titel, Datei oeffnen). Den Inhalt zeichnet das Programm in geteilten Speicher, den der Desktop mitliest.
  Der Desktop wartet nie auf ein Programm (schreibt nur, wenn die Pipe Platz hat); wer auf den Schliessen-Knopf
  nicht reagiert, wird beim dritten Klick beendet.
- Die Grafikbibliothek erledigt das selbst: unter dem Desktop liefert `gfx_open()` ein Fenster statt des
  Bildschirms, `gfx_open_window_ex(w, h, titel, GFX_RESIZABLE)` waehlt Groesse und Titel, neue Ereignisse sind
  `EV_CLOSE`, `EV_FOCUS` und `EV_RESIZE`. Gemeinsames Aussehen (Masse, Farben, Programmsymbole): `ui.h`
- Geteilter Speicher: `SYS_SHM` (anlegen, per Nummer einblenden, ausblenden); die Seiten tragen ein eigenes
  PTE-Bit, damit `fork` sie nicht kopiert und `munmap`/Programmende sie nicht doppelt freigeben (Test: `shmtest`)
- Benannte Dienste: `SYS_SERVICE` (anmelden, verbinden, annehmen) - wie ein sehr einfacher Unix-Socket, jede
  Verbindung bekommt zwei Pipes. Der Desktop meldet sich als "desktop" an; findet ein Grafikprogramm keine
  Begruessung auf Deskriptor 3, verbindet es sich dort. Endet der Anbieter, verschwindet der Dienst.
- Schriften: Inter und JetBrains Mono (SIL Open Font License, verkleinert in `/share/fonts`), gerastert mit
  stb_truetype (gemeinfrei, `Userland/include/stb_truetype.h`); `ttf.h` fuer Programme
- Zeichnen mit Kantenglaettung und Transparenz (`Userland/lib/draw.c`): abgerundete Rechtecke, Kreise, Linien,
  Schatten, Verlaeufe, Weichzeichnen

## Ton (Intel High Definition Audio)

`Kernel/drivers/sound/hda.c` sucht einen HDA-Controller (PCI-Klasse 04.03), setzt ihn zurueck und fragt die Codecs
ueber CORB/RIRB ab. Von jedem analogen Ausgang (Kopfhoerer, Line-Out, Lautsprecher) sucht er einen Weg zu einem DAC,
schaltet ihn durch und stellt die Verstaerker ein. Steckt an einer Buchse etwas, sind die eingebauten Lautsprecher
aus (Auto-Mute). Bis zu 8 Programme spielen gleichzeitig: jedes hat eine Stimme, deren Abtastrate der Kernel auf
48 kHz umrechnet; ein Mischer-Thread addiert die Stimmen und haelt den DMA-Ring der Soundkarte etwa 60 ms voraus
gefuellt. Ohne Ton ist der Stream aus und der Thread schlaeft.

- Programme: `#include "sound.h"`, `snd_open()`, `snd_tone(hz, ms, lautstaerke)`, `snd_rest(ms)` (Tetris und Snake
  machen damit ihre Effekte); direkt ueber `SYS_AUDIO` gehen beliebige 16-Bit-Daten

- `play datei.wav` spielt WAV-Dateien: PCM mit 8/16/24/32 Bit oder 32-Bit-Gleitkomma, Mono oder Stereo, jede
  Abtastrate (kann der Codec sie nicht, rechnet `play` auf 48 kHz um). Beispiel: `play /share/klang.wav`
- `play lied.mp3` spielt MP3-Dateien (MPEG-1/2/2.5, Layer I-III, mit ID3-Tag), `play -w lied.wav lied.mp3` wandelt
  sie in WAV um. Dekodiert wird mit [minimp3](https://github.com/lieff/minimp3) (CC0, unveraendert in
  `Userland/include/minimp3.h`); die ISO-Testdateien ergeben im System bitgenau dieselben Werte wie auf dem Host
- `play -t [Hz]` spielt einen Testton (links, rechts, beide), `play -v 0-100` setzt die Lautstaerke
- In QEMU: `make run SOUND=pa` (hoerbar) oder `SOUND=wav` (Aufnahme in `Build/sound.wav`)

## Gleitkomma in Programmen

Programme duerfen mit `float`/`double` und SSE rechnen (das Userland wird ohne `-mno-sse` uebersetzt). Der Kernel
selbst nutzt FPU/SSE nicht; beim Threadwechsel sichert er die Register des alten Threads (`fxsave`) und laedt die des
neuen (`fxrstor`), `fork` gibt sie ans Kind weiter, `exec` setzt sie zurueck. Selbsttest: acht `fputest` rechnen
gleichzeitig (mehr Programme als CPUs) und muessen dasselbe Ergebnis erhalten wie ohne Unterbrechung.

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
  net/                IPv4-Stack: ARP, ICMP, DHCP, UDP, TCP, DNS, NTP
  lib/                string, kprintf, UTF-8
  tests/              Selbsttests, je Gruppe eine Datei

Userland/
  include/            Syscalls (user.h), libc, malloc, gfx (Grafik), ui (Aussehen), winproto (Fensterprotokoll),
                      util (Helfer fuer Werkzeuge)
  lib/                libuser.a
  bin/                je Programm eine Datei (NAME.c) oder ein Ordner (sh/, desktop/) -> /bin/NAME

Initrd/               wird 1:1 in die initrd kopiert (/etc/profile, /etc/motd, Test-Skripte)
tools/                Images erzeugen (mkdisk, mkesp, mkstick, mkvmx), FAT pruefen (fatcheck), Schriften (gen_font*)
```

Build-Ergebnisse landen in `Build/` (Objektdateien, `kernel.debug.elf` mit Debug-Infos, `esp.img`, `Out.log`) und
`Image/` (`bootx64.efi`, `kernel.elf`, `initrd.tar`, Datenplatte).
