#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Syscall-Nummern (Aufrufkonvention wie Linux x86-64: rax = Nummer, Argumente rdi, rsi, rdx, r10, r8, r9;
 * Rueckgabe in rax, negative Werte sind Fehler; rcx und r11 werden zerstoert). Gleiche Nummern in Userland/user.h. */
#define SYS_WRITE     1  /* (fd, buf, len) -> geschriebene Bytes (fd 0-2 sind anfangs die Konsole) */
#define SYS_EXIT      2  /* (code) */
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_SLEEP_MS  5  /* (ms), wird durch Ctrl-C/kill unterbrochen */
#define SYS_TICKS     6  /* (0) -> Timer-Ticks seit Start (100 Hz); (1) -> Mikrosekunden seit Start */
#define SYS_GETCHAR   7  /* -> Zeichen oder -1, wenn keins bereitliegt (roh, ohne Terminal-Zeilenmodus) */
#define SYS_OPEN      8  /* (path, flags) -> fd; flags wie Linux: O_WRONLY 1, O_RDWR 2, O_CREAT 0x40, O_TRUNC 0x200, O_APPEND 0x400 */
#define SYS_READ      9  /* (fd, buf, len) -> gelesene Bytes, 0 = Dateiende/Ende der Pipe */
#define SYS_CLOSE     10 /* (fd) */
#define SYS_READDIR   11 /* (path, index, DirEnt*) -> 0 oder ERR_NOENT am Ende des Verzeichnisses */
#define SYS_SPAWN     12 /* (path, cmdline) -> PID; cmdline wird an Leerzeichen zu argv zerlegt */
#define SYS_WAIT      13 /* (pid, int *code) -> 0 = normal beendet, 1 = durch Fehler, 2 = per Ctrl-C/kill beendet; nur eigene Kinder */
#define SYS_BRK       14 /* (addr) -> neues Heap-Ende; addr 0 fragt nur ab */
#define SYS_MMAP      15 /* (len) -> Adresse eines genullten, beschreibbaren Bereichs */
#define SYS_MUNMAP    16 /* (addr, len) */
#define SYS_MKDIR     17 /* (path) */
#define SYS_UNLINK    18 /* (path): Datei oder leeres Verzeichnis */
#define SYS_POWER     19 /* (modus): 0 = ausschalten, 1 = neu starten; kehrt nur bei ungueltigem Modus zurueck */
#define SYS_FORK      20 /* () -> im Elternprozess die PID des Kindes, im Kind 0 */
#define SYS_EXEC      21 /* (path, cmdline): ersetzt das Programm; kehrt nur bei einem Fehler zurueck */
#define SYS_PIPE      22 /* (int fds[2]): fds[0] = Leseende, fds[1] = Schreibende */
#define SYS_DUP       23 /* (fd) -> neuer fd (kleinster freier) auf dasselbe Objekt */
#define SYS_DUP2      24 /* (old, new) -> new; ein offener new wird vorher geschlossen */
#define SYS_LSEEK     25 /* (fd, offset, whence 0/1/2) -> neue Position; nur bei Dateien */
#define SYS_RENAME    26 /* (alt, neu): umbenennen oder verschieben (Datei oder Ordner) auf demselben FAT-Datentraeger */
#define SYS_STAT      27 /* (path, Stat*) */
#define SYS_CHDIR     28 /* (path) */
#define SYS_GETCWD    29 /* (buf, size) -> Laenge */
#define SYS_SETPGID   30 /* (pid, pgid): 0 = aktueller Prozess bzw. pid als Gruppe */
#define SYS_TTY_FG    31 /* (pgid): Vordergrundgruppe fuer Ctrl-C; 0 = keine (Ctrl-C wird dann als Zeichen 3 geliefert) */
#define SYS_TTY_MODE  32 /* (modus): 0 = Zeilenmodus mit Echo und Editieren, 1 = roh (Zeichen sofort, Pfeiltasten als 0x81..) */
#define SYS_KILL      33 /* (pid) */
#define SYS_PROCINFO  34 /* (index, ProcInfo*) -> 0 oder ERR_NOENT am Ende */
#define SYS_USBINFO   35 /* (index, UsbInfo*) -> 0 oder ERR_NOENT am Ende (Struktur siehe usb.h) */

#define SYS_VIDEOINFO 41 /* (index, VideoInfo*) -> 0 oder ERR_NOENT am Ende: verfuegbare Grafikmodi */
#define SYS_GFX       47 /* (op, arg): 0 = Bildschirm uebernehmen -> (Breite << 32) | Hoehe, 1 = GfxBlit* kopieren, 2 = freigeben, 3 = Hardware-Mauszeiger (arg = x | y << 16 | sichtbar << 32; ERR_NOSYS ohne) */
#define SYS_FONT      48 /* (zeichen, out[16]) -> 0: 8x16-Glyph (Bit 7 = links) */
#define SYS_FDAVAIL   49 /* (fd, warten_ms) -> wartende Bytes einer Pipe, 0 = noch nichts, -1 = Ende; Schreibende: freier Platz, -1 = kein Leser.
                          * warten_ms (hoechstens 1000): am Leseende einer leeren Pipe so lange auf Daten warten (wacht sofort auf) */
#define SYS_STATFS    46 /* (pfad, u64 out[2]) -> 0: Groesse und freier Platz in Bytes */
#define SYS_MOUSEMODE 44 /* (1 = Programm wertet die Maus aus, 0 = Konsole) */
#define SYS_CLIPBOARD 45 /* (0, buf, max) -> Laenge der Zwischenablage (kopiert hoechstens max Bytes); (1, text, len) -> 0: setzen */
#define SYS_KEYMAP    43 /* (name oder 0, out[16]) -> 0 / ERR_INVAL: Layout setzen (us, de, uk) und/oder abfragen */
#define SYS_MOUSE     42 /* (MouseInfo*) -> 0: Position, Tasten, Rad (seit dem letzten Aufruf), Zaehler */
#define SYS_TIME      39 /* () -> Sekunden seit 1970-01-01 (Zeit wie in der RTC, meist Ortszeit), 0 = unbekannt */
#define SYS_SETTIME   40 /* (sekunden) -> 0 oder Fehler: stellt die RTC */
#define SYS_CURSOR    38 /* () -> (Spalte << 16) | Zeile des Konsolen-Cursors */
#define SYS_ISATTY    37 /* (fd) -> 0 = kein Terminal, sonst (Spalten << 16) | Zeilen der Konsole */
#define SYS_MOUNTINFO 36 /* (index, MountInfo*) -> 0 oder ERR_NOENT am Ende; index 0 scannt vorher neu nach Datentraegern */
#define SYS_PCIINFO   50 /* (index, PciInfo*) -> 0 oder ERR_NOENT am Ende (Struktur siehe pci.h) */
#define SYS_NETINFO   51 /* (index, NetInfo*) -> 0 oder ERR_NOENT am Ende (Struktur siehe net.h) */
#define SYS_NETCFG    52 /* (op, index, arg): 0 = statisch (arg: u8[16] ip, maske, gateway, dns), 1 = DHCP starten, 2 = ARP-Eintrag index nach ArpInfo* arg */
#define SYS_PING      53 /* (ip a|b<<8|c<<16|d<<24, seq | groesse << 16, timeout_ms) -> Mikrosekunden | TTL << 40, oder Fehler */
#define SYS_SOCKET    54 /* (1 = UDP, port; 0 = frei waehlen) -> fd; ERR_EXIST, wenn der Port belegt ist.
                          * (2 = TCP verbinden, ip (a | b<<8 | c<<16 | d<<24), port | timeout_ms << 16) -> fd.
                          * (3 = TCP-Verbindung Nr. index, TcpInfo *) -> 0, ERR_NOENT am Ende */
#define SYS_SENDTO    55 /* (fd, SockMsg*) -> gesendete Bytes */
#define SYS_RECVFROM  56 /* (fd, SockMsg*, timeout_ms; 0 = nicht warten, 0xFFFFFFFF = unbegrenzt) -> Bytes; ip/port = Absender */
#define SYS_RESOLVE   57 /* (name, u8 out[max][4], max) -> Anzahl der IPv4-Adressen oder Fehler (DNS) */
#define SYS_NTP       58 /* (server oder 0, NtpResult*, uhr_stellen) -> 0 oder Fehler */
#define SYS_SOCKPORT  59 /* (fd) -> lokaler Port des Sockets */
#define SYS_CPUINFO   60 /* (index, CpuInfo*) -> 0 oder ERR_NOENT am Ende */
#define SYS_GPU       62 /* (op): Intel-Grafik-Tests: 1 = Page-Flipping, 2 = Hardware-Mauszeiger, 3 = Blitter, 4 = Zaehler/Messung, 5 = EDID, 6 = Skalierer, 7 = Moduswechsel, 8 = DisplayPort, 9 = DP-Moduswechsel, 10 = DP-Link-Training, 11 | port << 8 = Anschluss, 12 = Bildwechsel-Interrupt (igdtest) -> 0 ok, < 0 Fehler (Details im Kernel-Log) */
#define SYS_AUDIO     64 /* (op, a, b): 0 oeffnen (rate, kanaele; 16 Bit), 1 schreiben (buf, len) -> Bytes, 2 ausspielen lassen,
                            3 schliessen, 4 Gesamtlautstaerke (0-100, -1 = abfragen) -> jetzt, 5 gemischte Bytes,
                            6 Lautstaerke der eigenen Stimme. Bis zu 8 Programme gleichzeitig (Mischer).
                            ERR_NOSYS kein Geraet, ERR_AGAIN belegt, ERR_INVAL Format geht nicht */
#define SYS_SETMODE   63 /* (breite, hoehe, hz100; 0 = egal) -> 0; breite 0 = Modus der Firmware. ERR_NOSYS ohne Treiber, ERR_NOENT unbekannt, ERR_AGAIN Grafikprogramm laeuft, ERR_IO */
#define SYS_SHM       65 /* (op, a, b): geteilter Speicher fuer Fenster-Programme, 0 anlegen (bytes, u32 *nummer) -> Adresse,
                            1 einblenden (nummer) -> Adresse, 2 ausblenden (adresse), 3 Groesse (nummer) -> Bytes */
#define SYS_SERVICE   66 /* (op, name, int *fds): benannte Dienste (service.h): 0 anmelden, 1 abmelden, 2 verbinden (fds[2]:
                            lesen, schreiben), 3 annehmen (fds[3]: lesen, schreiben, PID; ERR_AGAIN = keine wartet) */
#define SYS_GPUCOMP   67 /* (op, a, b): Zusammensetzen auf der GPU (igd_comp.c). 0 Art -> 0 keins, 1 GPU, 2 CPU-Ersatz im Kernel;
                            1 Flaeche anmelden (shm-Nummer, breite | hoehe << 16) -> Nummer; 2 abmelden (nummer);
                            3 ausfuehren (GpuOp *, anzahl) -> 0, erst zurueck, wenn alles fertig ist;
                            4 Messwert melden (0 CPU / 1 GPU, mikrosekunden | pixel << 32) */
#define SYS_KLOG      61 /* (u64 *pos, buf, max) -> Bytes aus dem Kernel-Log ab *pos (wird weitergezaehlt), 0 = Ende */
/* Threads (process.h): teilen Speicher und Deskriptoren; Nummer 0..15, 0 = erster Thread */
#define SYS_THREAD_CREATE 68 /* (entry, stack_top, arg) -> Nummer: neuer Thread startet bei entry(arg) mit rsp = stack_top - 8;
                              * ERR_AGAIN = alle 16 Plaetze belegt (auch beendete, nicht abgeholte) */
#define SYS_THREAD_EXIT   69 /* (wert): nur diesen Thread beenden; mit dem letzten endet der Prozess (Code 0) */
#define SYS_THREAD_JOIN   70 /* (nummer, u64 *wert) -> 0: wartet auf das Ende, holt den Wert ab, gibt die Nummer frei */
#define SYS_GETTID        71 /* () -> eigene Nummer */
#define SYS_FUTEX_WAIT    72 /* (u32 *adr, wert, timeout_ms; 0 = ohne Grenze) -> 0 geweckt, ERR_AGAIN *adr != wert,
                              * ERR_TIMEDOUT; nur Threads desselben Prozesses */
#define SYS_FUTEX_WAKE    73 /* (u32 *adr, anzahl) -> geweckte Threads */

/* Argument fuer SYS_SENDTO/SYS_RECVFROM (gleiches Layout in Userland/user.h) */
typedef struct {
    uint64_t buf;
    uint32_t len;
    uint8_t  ip[4];
    uint16_t port;
    uint16_t pad;
    uint32_t pad2;
} SockMsg;

/* Eintrag fuer SYS_MOUNTINFO (gleiches Layout in Userland/user.h); flags: 1 = nur lesbar, 2 = Dateisystem nicht unterstuetzt, 4 = nicht sauber getrennt */
typedef struct {
    char     point[32];
    char     device[16];
    char     label[16];
    char     fstype[16];
    uint64_t mib;
    uint64_t flags;
} MountInfo;

/* Dateisysteme: "/" ist die initrd (nur lesbar), "/disk" die FAT32-Platte MINIKERNEL (lesen und schreiben, lange Namen),
 * "/mnt/<geraet>" weitere FAT12/16/32-Datentraeger (nur lesbar, lange Namen). */

#define ERR_NOENT     (-2)
#define ERR_INTR      (-4)
#define ERR_AGAIN     (-11)
#define ERR_IO        (-5)
#define ERR_BADF      (-9)
#define ERR_CHILD     (-10)
#define ERR_NOMEM     (-12)
#define ERR_FAULT     (-14)
#define ERR_EXIST     (-17)
#define ERR_NOTDIR    (-20)
#define ERR_ISDIR     (-21)
#define ERR_INVAL     (-22)
#define ERR_SPIPE     (-29)
#define ERR_NOSPC     (-28)
#define ERR_ROFS      (-30)
#define ERR_PIPE      (-32)
#define ERR_NOSYS     (-38)
#define ERR_NOTEMPTY  (-39)
#define ERR_NETUNREACH  (-101)
#define ERR_CONNRESET   (-104)
#define ERR_CONNREFUSED (-111)
#define ERR_TIMEDOUT    (-110)
#define ERR_HOSTUNREACH (-113)

/* Argument fuer SYS_GFX op 1: Rechteck (x, y, w, h) aus dem Puffer buf (Zeilenlaenge pitch Pixel, 0x00RRGGBB) zeigen */
typedef struct {
    uint64_t buf;
    uint32_t pitch;
    int32_t  x, y, w, h;
} GfxBlit;

/* Eintrag fuer SYS_CPUINFO (gleiches Layout in Userland/include/user.h): Timer-Ticks (je 10 ms) nach Zustand */
typedef struct {
    uint32_t index, apic_id;
    uint64_t ticks_user, ticks_kernel, ticks_idle;
} CpuInfo;

/* Eintrag fuer SYS_MOUSE (gleiches Layout in Userland/user.h); x, y in Pixeln, buttons: Bit 0 links, 1 rechts, 2 Mitte */
typedef struct {
    int32_t  x, y;
    uint32_t buttons;
    int32_t  wheel;
    uint32_t events;   /* zaehlt jede Meldung der Maus */
    uint32_t attached; /* 1 = eine Maus ist angeschlossen */
    uint32_t width, height;
    uint32_t left_presses, right_presses; /* zaehlen jedes Druecken (auch Klicks, die zwischen zwei Abfragen liegen) */
    int32_t  press_x, press_y;             /* Position beim letzten Druecken der linken Taste */
    uint32_t kbd_mods;                     /* gerade gedrueckte Umschalttasten: 1 Shift, 2 Alt, 4 Strg (fuer Klicks) */
} MouseInfo;

/* Eintrag fuer SYS_VIDEOINFO (gleiches Layout in Userland/user.h): ein Grafikmodus; scale/cols/rows beschreiben die Konsole */
typedef struct {
    uint32_t width, height;
    uint32_t current;    /* 1 = aktuell benutzter Modus */
    uint32_t scale;      /* Schriftvergroesserung */
    uint32_t cols, rows; /* Textfeld */
    uint32_t hz100;      /* Bildrate in 1/100 Hz; 0 = Modus der Firmware (umschalten erst nach Neustart) */
    uint64_t kernel_size; /* Groesse von \kernel.elf auf dem Boot-Volume */
} VideoInfo;

/* Eintrag fuer SYS_READDIR (gleiches Layout in Userland/user.h); Namen sind UTF-8; mtime = Sekunden seit 1970 (Zeit wie in der RTC) oder 0 */
typedef struct {
    char     name[256];
    uint64_t size;
    uint64_t is_dir;
    uint64_t mtime;
} DirEnt;

/* SYS_STAT */
typedef struct {
    uint64_t size;
    uint64_t is_dir;
    uint64_t mtime;
} Stat;

/* SYS_PROCINFO: state 0 = laeuft, 1 = beendet (Zombie, noch nicht abgeholt) */
typedef struct {
    uint32_t pid, ppid, pgid, state;
    char     name[32];
    uint32_t threads, pad; /* laufende Threads */
} ProcInfo;

/* Gesicherte Register beim syscall (Layout muss zu den Pushes in syscall_entry.S passen, niedrigste Adresse zuerst).
 * Enthaelt alle Register, damit ein fork das Kind mit genau dem Zustand des Elternprozesses weiterlaufen lassen kann. */
typedef struct {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t r9, r8, r10, rdx, rsi, rdi, rax, rcx, r11, user_rsp;
} SyscallFrame;

/* Aktiviert syscall/sysret (EFER.SCE, STAR, LSTAR, SFMASK). */
void syscall_init(void);

#endif
