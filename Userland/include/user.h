#ifndef USER_H
#define USER_H

/* Minimale Userland-Bibliothek: Syscall-Wrapper (Nummern wie Kernel/core/syscall.h). */

typedef unsigned long long u64;
typedef long long          s64;

#define SYS_WRITE     1
#define SYS_EXIT      2
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_SLEEP_MS  5
#define SYS_TICKS     6
#define SYS_GETCHAR   7
#define SYS_OPEN      8
#define SYS_READ      9
#define SYS_CLOSE     10
#define SYS_READDIR   11
#define SYS_SPAWN     12
#define SYS_WAIT      13
#define SYS_BRK       14
#define SYS_MMAP      15
#define SYS_MUNMAP    16
#define SYS_MKDIR     17
#define SYS_UNLINK    18
#define SYS_POWER     19
#define SYS_FORK      20
#define SYS_EXEC      21
#define SYS_PIPE      22
#define SYS_DUP       23
#define SYS_DUP2      24
#define SYS_LSEEK     25
#define SYS_RENAME    26
#define SYS_STAT      27
#define SYS_CHDIR     28
#define SYS_GETCWD    29
#define SYS_SETPGID   30
#define SYS_TTY_FG    31
#define SYS_TTY_MODE  32
#define SYS_KILL      33
#define SYS_PROCINFO  34
#define SYS_USBINFO   35
#define SYS_MOUNTINFO 36
#define SYS_VIDEOINFO 41
#define SYS_GFX       47
#define SYS_FONT      48
#define SYS_FDAVAIL   49
#define SYS_STATFS    46
#define SYS_MOUSEMODE 44
#define SYS_CLIPBOARD 45
#define SYS_KEYMAP    43
#define SYS_MOUSE     42
#define SYS_TIME      39
#define SYS_SETTIME   40
#define SYS_CURSOR    38
#define SYS_ISATTY    37
#define SYS_PCIINFO   50
#define SYS_NETINFO   51
#define SYS_NETCFG    52
#define SYS_PING      53
#define SYS_SOCKET    54
#define SYS_SENDTO    55
#define SYS_RECVFROM  56
#define SYS_RESOLVE   57
#define SYS_NTP       58
#define SYS_SOCKPORT  59
#define SYS_CPUINFO   60
#define SYS_KLOG      61
#define SYS_GPU       62
#define SYS_SETMODE   63
#define SYS_AUDIO     64
#define SYS_SHM       65
#define SYS_SERVICE   66
#define ERR_NOENT     (-2)
#define ERR_IO        (-5)
#define ERR_EXIST     (-17)
#define ERR_INVAL     (-22)
#define ERR_AGAIN     (-11)
#define ERR_INTR      (-4)
#define ERR_NOSYS     (-38)
#define ERR_NOSPC     (-28)
#define ERR_ROFS      (-30)
#define ERR_NOTEMPTY  (-39)
#define ERR_NETUNREACH  (-101)
#define ERR_CONNRESET   (-104)
#define ERR_CONNREFUSED (-111)
#define ERR_TIMEDOUT    (-110)
#define ERR_HOSTUNREACH (-113)

/* Sondertasten, wie sie im Rohmodus (sys_tty_mode(1)) von read(0) geliefert werden */
#define KEY_UP    0xF5
#define KEY_DOWN  0xF6
#define KEY_LEFT  0xF7
#define KEY_RIGHT 0xF8
#define KEY_HOME  0xF9
#define KEY_END   0xFA
#define KEY_DEL   0xFB
#define KEY_PGUP  0xFC
#define KEY_PGDN  0xFD
#define KEY_ALT  0xFE /* SYS_GETCHAR: Alt + die naechste Taste (Kleinbuchstabe, Ziffer, Tab, Sondertaste) */
#define KEY_MODS 0xFF /* SYS_GETCHAR: dann Umschalttasten (1 Shift, 2 Alt, 4 Strg) und die Taste */

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

typedef struct {
    char name[256];
    u64  size;
    u64  is_dir;
    u64  mtime; /* Sekunden seit 1970 (Zeit wie in der RTC), 0 = unbekannt */
} DirEnt;

typedef struct {
    u64 size;
    u64 is_dir;
    u64 mtime;
} Stat;

/* USB-Geraet (SYS_USBINFO); driver: 0 keiner, 1 Tastatur, 2 Massenspeicher, 3 Hub; speed: 1 Full, 2 Low, 3 High, 4 Super, 5 Super+ */
typedef struct {
    unsigned vid, pid, cls, speed, port, slot, driver;
    char     name[32];
    char     path[16]; /* "1" = Root-Port 1, "1.3" = Port 3 des Hubs an Root-Port 1 */
} UsbInfo;

typedef struct {
    char point[32], device[16], label[16], fstype[16];
    u64  mib, flags; /* flags: 1 = nur lesbar, 2 = nicht unterstuetzt, 4 = nicht sauber getrennt */
} MountInfo;

typedef struct {
    unsigned width, height, current, scale, cols, rows, hz100; /* hz100: Bildrate in 1/100 Hz, 0 = Modus der Firmware */
    u64      kernel_size;
} VideoInfo;

typedef struct {
    int      x, y;
    unsigned buttons; /* Bit 0 links, 1 rechts, 2 Mitte */
    int      wheel;   /* Radschritte seit dem letzten Aufruf (> 0 = nach oben) */
    unsigned events, attached, width, height;
    unsigned left_presses, right_presses; /* zaehlen jedes Druecken (kein Klick geht zwischen zwei Abfragen verloren) */
    int      press_x, press_y;            /* Position beim letzten Druecken der linken Taste */
    unsigned kbd_mods;                    /* gedrueckte Umschalttasten: 1 Shift, 2 Alt, 4 Strg */
} MouseInfo;

typedef struct {
    u64 buf;
    unsigned pitch;
    int x, y, w, h;
} GfxBlit;

/* SYS_CPUINFO: Timer-Ticks (je 10 ms) einer CPU, nach Zustand */
typedef struct {
    unsigned index, apic_id;
    u64      ticks_user, ticks_kernel, ticks_idle;
} CpuInfo;

/* state: 0 = laeuft, 1 = beendet (noch nicht abgeholt) */
typedef struct {
    unsigned pid, ppid, pgid, state;
    char     name[32];
} ProcInfo;

/* PCI-Geraet (SYS_PCIINFO); driver: Name des Kernel-Treibers oder "" */
typedef struct {
    unsigned char  bus, dev, fn, class_code, subclass, prog_if, revision, pad;
    unsigned short vendor, device, sub_vendor, sub_device;
    unsigned       bar[6];
    char           driver[16];
} PciInfo;

/* Netzwerkkarte (SYS_NETINFO); Adressen als Bytes a.b.c.d; dhcp: 0 aus (statisch), 1 laeuft, 2 Adresse erhalten, 3 fehlgeschlagen */
typedef struct {
    char          name[8];
    char          model[40];
    unsigned char mac[6];
    unsigned char link, full_duplex;
    unsigned      mbps;
    unsigned char ip[4], mask[4], gateway[4], dns[4], dhcp_server[4];
    unsigned      dhcp, lease_s;
    u64           rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    unsigned      irq, pad; /* irq: 1 = Interrupts per MSI, 0 = wird abgefragt */
    u64           irqs;
} NetInfo;

/* Datagramm fuer sys_sendto/sys_recvfrom (Layout wie im Kernel) */
typedef struct {
    u64            buf;
    unsigned       len;
    unsigned char  ip[4];
    unsigned short port, pad;
    unsigned       pad2;
} SockMsg;

/* Ergebnis von sys_ntp */
typedef struct {
    s64           offset_ms;   /* Uhr des Rechners minus richtige Zeit (positiv: ging vor) */
    u64           utc, local;  /* richtige Zeit (Sekunden seit 1970) als UTC und als Ortszeit */
    unsigned      rtt_ms, stratum;
    unsigned char ip[4];
    int           tz_offset_s;
    char          server[64];
    char          tz[12];
    unsigned      pad;
} NtpResult;

/* TCP-Verbindung (sys_tcpinfo); state: 1 SYN_SENT, 2 ESTABLISHED, 3 FIN_WAIT_1, 4 FIN_WAIT_2, 5 CLOSING,
 * 6 TIME_WAIT, 7 CLOSE_WAIT, 8 LAST_ACK, 9 CLOSED */
typedef struct {
    unsigned char  local_ip[4], ip[4];
    unsigned short lport, rport;
    unsigned       state;
    unsigned       rx_queued, tx_queued;
    unsigned       rto_ms, srtt_ms;
} TcpInfo;

/* ARP-Eintrag (IP -> MAC) */
typedef struct {
    unsigned char  ip[4], mac[6];
    unsigned short pad;
    unsigned       age_s;
    char           dev[8];
} ArpInfo;

static inline s64 syscall3(u64 n, u64 a1, u64 a2, u64 a3)
{
    s64 ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline s64 sys_write(int fd, const void *buf, u64 len) { return syscall3(SYS_WRITE, fd, (u64)buf, len); }
static inline __attribute__((noreturn)) void sys_exit(int code) { syscall3(SYS_EXIT, code, 0, 0); for (;;); }
static inline s64 sys_getpid(void)                            { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline void sys_yield(void)                            { syscall3(SYS_YIELD, 0, 0, 0); }
static inline void sys_sleep_ms(u64 ms)                       { syscall3(SYS_SLEEP_MS, ms, 0, 0); }
static inline s64 sys_ticks(void)                             { return syscall3(SYS_TICKS, 0, 0, 0); }
static inline s64 sys_time_us(void)                           { return syscall3(SYS_TICKS, 1, 0, 0); } /* Mikrosekunden seit Start */
static inline s64 sys_getchar(void)                           { return syscall3(SYS_GETCHAR, 0, 0, 0); }
static inline s64 sys_open(const char *path, int flags)       { return syscall3(SYS_OPEN, (u64)path, flags, 0); }
static inline s64 sys_mkdir(const char *path)                 { return syscall3(SYS_MKDIR, (u64)path, 0, 0); }
static inline s64 sys_unlink(const char *path)                { return syscall3(SYS_UNLINK, (u64)path, 0, 0); }
static inline s64 sys_power(int mode)                        { return syscall3(SYS_POWER, mode, 0, 0); } /* 0 = aus, 1 = neu starten */
static inline s64 sys_fork(void)                              { return syscall3(SYS_FORK, 0, 0, 0); }
static inline s64 sys_exec(const char *path, const char *cmdline) { return syscall3(SYS_EXEC, (u64)path, (u64)cmdline, 0); }
static inline s64 sys_pipe(int fds[2])                        { return syscall3(SYS_PIPE, (u64)fds, 0, 0); }
static inline s64 sys_dup(int fd)                             { return syscall3(SYS_DUP, fd, 0, 0); }
static inline s64 sys_dup2(int old_fd, int new_fd)            { return syscall3(SYS_DUP2, old_fd, new_fd, 0); }
static inline s64 sys_lseek(int fd, s64 off, int whence)      { return syscall3(SYS_LSEEK, fd, (u64)off, whence); }
static inline s64 sys_rename(const char *from, const char *to) { return syscall3(SYS_RENAME, (u64)from, (u64)to, 0); }
static inline s64 sys_stat(const char *path, Stat *st)        { return syscall3(SYS_STAT, (u64)path, (u64)st, 0); }
static inline s64 sys_chdir(const char *path)                 { return syscall3(SYS_CHDIR, (u64)path, 0, 0); }
static inline s64 sys_getcwd(char *buf, u64 size)             { return syscall3(SYS_GETCWD, (u64)buf, size, 0); }
static inline s64 sys_setpgid(int pid, int pgid)              { return syscall3(SYS_SETPGID, pid, pgid, 0); }
static inline s64 sys_tty_fg(int pgid)                        { return syscall3(SYS_TTY_FG, pgid, 0, 0); }
static inline s64 sys_tty_mode(int raw)                       { return syscall3(SYS_TTY_MODE, raw, 0, 0); }
static inline s64 sys_kill(int pid)                           { return syscall3(SYS_KILL, pid, 0, 0); }
static inline s64 sys_procinfo(u64 index, ProcInfo *pi)       { return syscall3(SYS_PROCINFO, index, (u64)pi, 0); }
static inline s64 sys_mountinfo(u64 index, MountInfo *info) { return syscall3(SYS_MOUNTINFO, index, (u64)info, 0); }
static inline s64 sys_usbinfo(u64 index, UsbInfo *info)      { return syscall3(SYS_USBINFO, index, (u64)info, 0); }
static inline s64 sys_read(int fd, void *buf, u64 len)        { return syscall3(SYS_READ, fd, (u64)buf, len); }
static inline s64 sys_close(int fd)                           { return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline s64 sys_readdir(const char *path, u64 index, DirEnt *ent) { return syscall3(SYS_READDIR, (u64)path, index, (u64)ent); }
static inline s64 sys_spawn(const char *path, const char *cmdline)      { return syscall3(SYS_SPAWN, (u64)path, (u64)cmdline, 0); }
static inline s64 sys_wait(int pid, int *code)                { return syscall3(SYS_WAIT, pid, (u64)code, 0); }
static inline s64 sys_wait_nohang(int pid, int *code)         { return syscall3(SYS_WAIT, pid, (u64)code, 1); } /* ERR_AGAIN: laeuft noch */
static inline s64 sys_videoinfo(u64 index, VideoInfo *vi)  { return syscall3(SYS_VIDEOINFO, index, (u64)vi, 0); }
static inline s64 sys_audio(u64 op, u64 a, u64 b)          { return syscall3(SYS_AUDIO, op, a, b); } /* siehe play.c */
static inline s64 sys_setmode(u64 w, u64 h, u64 hz100)     { return syscall3(SYS_SETMODE, w, h, hz100); } /* sofort umschalten (Intel-Treiber) */
/* Geteilter Speicher: anlegen -> Adresse (und Nummer), einblenden per Nummer, ausblenden, Groesse */
static inline s64 sys_shm_create(u64 bytes, unsigned *id)  { return syscall3(SYS_SHM, 0, bytes, (u64)id); }
static inline s64 sys_shm_map(unsigned id)                 { return syscall3(SYS_SHM, 1, id, 0); }
static inline s64 sys_shm_unmap(void *addr)                { return syscall3(SYS_SHM, 2, (u64)addr, 0); }
static inline s64 sys_shm_size(unsigned id)                { return syscall3(SYS_SHM, 3, id, 0); }
/* Benannte Dienste: anmelden, abmelden, verbinden (fds: lesen, schreiben), annehmen (fds: lesen, schreiben, PID;
 * ERR_AGAIN = niemand wartet). Damit finden Programme aus dem Terminal den Desktop. */
static inline s64 sys_service_register(const char *name)     { return syscall3(SYS_SERVICE, 0, (u64)name, 0); }
static inline s64 sys_service_unregister(const char *name)   { return syscall3(SYS_SERVICE, 1, (u64)name, 0); }
static inline s64 sys_service_connect(const char *name, int fds[2]) { return syscall3(SYS_SERVICE, 2, (u64)name, (u64)fds); }
static inline s64 sys_service_accept(const char *name, int out[3])  { return syscall3(SYS_SERVICE, 3, (u64)name, (u64)out); }
static inline s64 sys_keymap(const char *set, char out[16]) { return syscall3(SYS_KEYMAP, (u64)set, (u64)out, 0); } /* set = 0: nur abfragen */
static inline s64 sys_gfx(int op, const void *arg)          { return syscall3(SYS_GFX, op, (u64)arg, 0); } /* 0 = uebernehmen, 1 = GfxBlit, 2 = freigeben */
static inline s64 sys_font(unsigned cp, unsigned char out[16]) { return syscall3(SYS_FONT, cp, (u64)out, 0); }
static inline s64 sys_fdavail(int fd)                         { return syscall3(SYS_FDAVAIL, fd, 0, 0); } /* Pipe: Bytes, 0 = nichts, -1 = Ende; Schreibende: freier Platz */
static inline s64 sys_statfs(const char *path, u64 out[2])  { return syscall3(SYS_STATFS, (u64)path, (u64)out, 0); } /* Groesse, frei (Bytes) */
static inline s64 sys_mousemode(int app)                    { return syscall3(SYS_MOUSEMODE, app, 0, 0); } /* 1 = Programm wertet die Maus aus */
static inline s64 sys_clipboard_get(char *buf, u64 max)      { return syscall3(SYS_CLIPBOARD, 0, (u64)buf, max); } /* Laenge */
static inline s64 sys_clipboard_set(const char *t, u64 len) { return syscall3(SYS_CLIPBOARD, 1, (u64)t, len); }
static inline s64 sys_mouse(MouseInfo *mi)                { return syscall3(SYS_MOUSE, (u64)mi, 0, 0); }
static inline s64 sys_time(void)                            { return syscall3(SYS_TIME, 0, 0, 0); } /* Sekunden seit 1970 (RTC-Zeit), 0 = unbekannt */
static inline s64 sys_settime(u64 secs)                       { return syscall3(SYS_SETTIME, secs, 0, 0); }
static inline s64 sys_cursor(void)                          { return syscall3(SYS_CURSOR, 0, 0, 0); } /* (Spalte << 16) | Zeile */
static inline s64 sys_isatty(int fd)                          { return syscall3(SYS_ISATTY, fd, 0, 0); } /* 0 = kein Terminal, sonst (Spalten << 16) | Zeilen */
static inline s64 sys_pciinfo(u64 index, PciInfo *pi)        { return syscall3(SYS_PCIINFO, index, (u64)pi, 0); }
static inline s64 sys_netinfo(u64 index, NetInfo *ni)        { return syscall3(SYS_NETINFO, index, (u64)ni, 0); }
static inline s64 sys_net_static(u64 index, const unsigned char cfg[16]) { return syscall3(SYS_NETCFG, 0, index, (u64)cfg); } /* ip, maske, gateway, dns */
static inline s64 sys_net_dhcp(u64 index)                    { return syscall3(SYS_NETCFG, 1, index, 0); }
static inline s64 sys_arpinfo(u64 index, ArpInfo *ai)        { return syscall3(SYS_NETCFG, 2, index, (u64)ai); }
/* ICMP-Echo: Ergebnis = Mikrosekunden | TTL << 40, oder ERR_TIMEDOUT/ERR_HOSTUNREACH/ERR_NETUNREACH/ERR_INTR */
static inline s64 sys_ping(const unsigned char ip[4], unsigned seq, unsigned size, unsigned timeout_ms)
{
    u64 a = ip[0] | ((u64)ip[1] << 8) | ((u64)ip[2] << 16) | ((u64)ip[3] << 24);
    return syscall3(SYS_PING, a, (seq & 0xFFFF) | ((u64)(size & 0xFFFF) << 16), timeout_ms);
}
/* UDP: Socket anlegen (port 0 = frei gewaehlt) -> fd; mit sys_close schliessen */
static inline s64 sys_udp_socket(unsigned port)              { return syscall3(SYS_SOCKET, 1, port, 0); }
/* TCP: Verbindung aufbauen -> fd (read/write wie eine Datei, read 0 = Gegenseite hat geschlossen; sys_close baut ab).
 * Fehler: ERR_CONNREFUSED, ERR_TIMEDOUT, ERR_NETUNREACH, ERR_HOSTUNREACH, ERR_INTR */
static inline s64 sys_tcp_connect(const unsigned char ip[4], unsigned port, unsigned timeout_ms)
{
    u64 a = ip[0] | ((u64)ip[1] << 8) | ((u64)ip[2] << 16) | ((u64)ip[3] << 24);
    return syscall3(SYS_SOCKET, 2, a, (port & 0xFFFF) | ((u64)timeout_ms << 16));
}
static inline s64 sys_tcpinfo(unsigned index, TcpInfo *ti)   { return syscall3(SYS_SOCKET, 3, index, (u64)ti); } /* ERR_NOENT am Ende */
static inline s64 sys_sockport(int fd)                       { return syscall3(SYS_SOCKPORT, fd, 0, 0); }
static inline s64 sys_cpuinfo(u64 index, CpuInfo *ci)        { return syscall3(SYS_CPUINFO, index, (u64)ci, 0); } /* ERR_NOENT: keine CPU mehr */
static inline s64 sys_gpu(u64 op)                           { return syscall3(SYS_GPU, op, 0, 0); } /* 1 = Page-Flip-Test, 2 = Mauszeiger-Test, 3 = Blitter-Test, 4 = Info, 5 = EDID, 6 = Skalierer, 7 = Moduswechsel, 8 = DisplayPort, 9 = DP-Moduswechsel, 10 = DP-Link-Training, 11 | Port << 8 = Anschluss, 12 = Bildwechsel */
static inline s64 sys_klog(u64 *pos, char *buf, u64 max)     { return syscall3(SYS_KLOG, (u64)pos, (u64)buf, max); } /* 0 = Ende des Kernel-Logs; *pos = ~0: setzt *pos auf das Ende */
static inline s64 sys_sendto(int fd, const unsigned char ip[4], unsigned port, const void *buf, unsigned len)
{
    SockMsg m = {(u64)buf, len, {ip[0], ip[1], ip[2], ip[3]}, (unsigned short)port, 0, 0};
    return syscall3(SYS_SENDTO, fd, (u64)&m, 0);
}
/* timeout_ms: 0 = nicht warten (ERR_AGAIN), 0xFFFFFFFF = unbegrenzt; ip/port = Absender (duerfen 0 sein) */
static inline s64 sys_recvfrom(int fd, void *buf, unsigned len, unsigned char ip[4], unsigned *port, unsigned timeout_ms)
{
    SockMsg m = {(u64)buf, len, {0, 0, 0, 0}, 0, 0, 0};
    s64 r = syscall3(SYS_RECVFROM, fd, (u64)&m, timeout_ms);
    if (r >= 0) {
        if (ip)
            for (int i = 0; i < 4; i++)
                ip[i] = m.ip[i];
        if (port)
            *port = m.port;
    }
    return r;
}
/* DNS: Name -> bis zu max IPv4-Adressen; Anzahl oder ERR_NOENT/ERR_TIMEDOUT/ERR_NETUNREACH */
static inline s64 sys_resolve(const char *name, unsigned char out[][4], int max) { return syscall3(SYS_RESOLVE, (u64)name, (u64)out, max); }
/* NTP: server 0 = per DHCP genannter bzw. pool.ntp.org; set = 1 stellt die Uhr */
static inline s64 sys_ntp(const char *server, NtpResult *r, int set) { return syscall3(SYS_NTP, (u64)server, (u64)r, set); }

static inline const char *net_strerror(s64 e)
{
    return e == ERR_NOENT ? "Name nicht gefunden" : e == ERR_TIMEDOUT ? "keine Antwort (Zeitueberschreitung)" :
           e == ERR_NETUNREACH ? "kein Netz (keine Adresse oder kein Gateway, siehe ifconfig)" :
           e == ERR_HOSTUNREACH ? "Ziel nicht erreichbar" : e == ERR_INVAL ? "ungueltiger Name" :
           e == ERR_EXIST ? "Port ist schon belegt" : e == ERR_CONNREFUSED ? "Verbindung abgelehnt" :
           e == ERR_CONNRESET ? "Verbindung von der Gegenseite abgebrochen" : e == ERR_INTR ? "abgebrochen" : e == ERR_IO ? "ungueltige Antwort" : "Fehler";
}

static inline void *sys_brk(void *addr)                       { return (void *)syscall3(SYS_BRK, (u64)addr, 0, 0); }
static inline s64 sys_mmap(u64 len)                           { return syscall3(SYS_MMAP, len, 0, 0); }
static inline s64 sys_munmap(void *addr, u64 len)             { return syscall3(SYS_MUNMAP, (u64)addr, len, 0); }

static inline u64 u_strlen(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int u_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline void u_puts(const char *s) { sys_write(1, s, u_strlen(s)); }

static inline void u_putc(char c) { sys_write(1, &c, 1); }

static inline void u_putdec(s64 v)
{
    char buf[22];
    int i = 21;
    int neg = v < 0;
    u64 u = neg ? (u64)-v : (u64)v;
    buf[i] = 0;
    do {
        buf[--i] = '0' + (u % 10);
        u /= 10;
    } while (u);
    if (neg)
        buf[--i] = '-';
    u_puts(&buf[i]);
}

#endif
