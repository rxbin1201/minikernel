#ifndef USER_H
#define USER_H

/* Minimale Userland-Bibliothek: Syscall-Wrapper (Nummern wie Kernel/core/syscall.h). */

typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned int       u32;

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
#define SYS_GPUCOMP   67
#define SYS_THREAD_CREATE 68
#define SYS_THREAD_EXIT   69
#define SYS_THREAD_JOIN   70
#define SYS_GETTID        71
#define SYS_FUTEX_WAIT    72
#define SYS_FUTEX_WAKE    73
#define SYS_MMAP_FILE     74
#define SYS_CLOSEFROM     75
#define SYS_WLAN          76
#define SYS_BT            77
#define ERR_NOENT     (-2)
#define ERR_IO        (-5)
#define ERR_EXIST     (-17)
#define ERR_INVAL     (-22)
#define ERR_AGAIN     (-11)
#define ERR_INTR      (-4)
#define ERR_BADF      (-9)
#define ERR_NOMEM     (-12)
#define ERR_NAMETOOLONG (-36) /* Pfad laenger als PATH_MAX - 1 */
#define PATH_MAX      1024  /* laengster Pfad (mit der abschliessenden 0), wie im Kernel (VFS_PATH_MAX) */
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
    unsigned threads, pad; /* laufende Threads */
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
/* WLAN (Intel AX200, Kernel/drivers/net/iwl.c): Karte, Register, zerlegte Firmware */
typedef struct {
    unsigned       present;
    unsigned char  bus, dev, fn, pad;
    unsigned short vendor, device, sub_vendor, sub_device;
    u64            bar;
    unsigned       hw_rev, rf_id, gp_cntrl, hw_if_config;
    unsigned       fw_found, fw_api, fw_lmac, fw_umac, fw_paging, fw_bytes, fw_paging_bytes;
    unsigned       fw_cpus, fw_capa, fw_api_flags, fw_scan_channels, fw_major, fw_minor, fw_local;
    char           fw_name[64], fw_human[64];
    char           state[64];
    unsigned       wake_done, wake_clock, wake_access, wake_us, gp_after, cnvi_id;
    unsigned       wake_ready, hwif_after, prph_load, prph_cpu1;
    unsigned       load_done, load_alive, load_ms, load_int, load_status, reset_before, reset_after;
    unsigned       wfpm_before, wfpm_after, int_after, st_load, st_umac_pc, st_lmac_pc;
    unsigned       alive_len, alive_cmd, alive_group, alive_status;
    unsigned       dmar_found, dmar_flags, iommu_units, iommu_active, iommu_off; /* IOMMU (VT-d) vor dem Start */
    unsigned       iommu_gsts[4], iommu_pmen[4];
    unsigned       ltr_before, ltr_after;
    unsigned       pci_cmd, bridges_fixed;
    unsigned       init_step, init_complete, rx_packets; /* Stufe 3: 5 = fertig */
    unsigned char  mac[6], mac_pad[2];
    unsigned       nvm_flags, nvm_version, nvm_board, nvm_hw_addrs, nvm_sku, nvm_tx_chains, nvm_rx_chains, nvm_lar;
    unsigned       nvm_channels;
    unsigned       mcc, mcc_status, mcc_channels; /* Land (zwei Buchstaben), Status, erlaubte Kanaele */
    unsigned       scan_ms, scan_frames, scan_nets;
} WlanInfo;
/* Gefundenes Netz (SYS_WLAN 4) */
enum { WLAN_SEC_OPEN, WLAN_SEC_WEP, WLAN_SEC_WPA, WLAN_SEC_WPA2, WLAN_SEC_WPA3, WLAN_SEC_WPA2_3 };
typedef struct {
    unsigned char  bssid[6];
    signed char    signal; /* dBm */
    unsigned char  channel, security, ssid_len;
    unsigned short seen;
    char           ssid[33];
    unsigned char  pad[3];
} WlanNet;
static inline s64 sys_wlan_info(WlanInfo *wi)                 { return syscall3(SYS_WLAN, 0, (u64)wi, 0); }
static inline s64 sys_wlan_wake(void)                         { return syscall3(SYS_WLAN, 1, 0, 0); }
static inline s64 sys_wlan_load(void)                         { return syscall3(SYS_WLAN, 2, 0, 0); }
static inline s64 sys_wlan_scan(void)                         { return syscall3(SYS_WLAN, 3, 0, 0); } /* -> Zahl */
static inline s64 sys_wlan_net(u64 i, WlanNet *n)             { return syscall3(SYS_WLAN, 4, i, (u64)n); }
/* Verbinden (SYS_WLAN 5): wartet, bis die Verbindung steht oder scheitert (Grund in WlanStatus) */
typedef struct {
    char          ssid[33];
    char          pass[65];  /* WPA2: Passphrase 8..63 Zeichen oder 64 Hex-Zeichen; offen: leer */
    unsigned char bssid[6];  /* 0: der staerkste AP mit diesem Namen */
} WlanConnect;
enum { WLAN_ST_IDLE, WLAN_ST_CONNECTING, WLAN_ST_CONNECTED, WLAN_ST_FAILED };
enum { WLAN_STEP_NONE, WLAN_STEP_FW, WLAN_STEP_SCAN, WLAN_STEP_PMK, WLAN_STEP_CONTEXT, WLAN_STEP_STATION,
       WLAN_STEP_QUEUES, WLAN_STEP_PROTECT, WLAN_STEP_AUTH, WLAN_STEP_ASSOC, WLAN_STEP_KEYS, WLAN_STEP_DONE };
typedef struct {
    unsigned       state, step;          /* WLAN_ST_*, WLAN_STEP_* */
    int            error;
    unsigned short status_code, reason; /* Status von Auth/Assoc, Grund einer Trennung durch den AP */
    char           ssid[33];
    unsigned char  bssid[6];
    unsigned char  channel, security;
    signed char    signal;
    unsigned char  pad[6];
    unsigned       aid, rate_kbps, connect_ms;
    unsigned       pad2;
    u64            rx_frames, tx_frames, rx_dropped, tx_failed, rekeys;
    char           msg[96];
} WlanStatus;
static inline s64 sys_wlan_connect(const WlanConnect *c)      { return syscall3(SYS_WLAN, 5, (u64)c, 0); }
static inline s64 sys_wlan_disconnect(void)                   { return syscall3(SYS_WLAN, 6, 0, 0); }
static inline s64 sys_wlan_status(WlanStatus *s)              { return syscall3(SYS_WLAN, 7, (u64)s, 0); }
/* Bluetooth (Kernel/drivers/bt): Geraet am USB, Intel-Version, Boot-Parameter, Firmware-Datei */
enum { BT_MODE_UNKNOWN, BT_MODE_BOOTLOADER, BT_MODE_OPERATIONAL };
typedef struct {
    unsigned       present;
    unsigned short vid, pid;
    char           path[16];
    unsigned char  ep_intr, ep_bulk_in, ep_bulk_out, pad0;
    unsigned short mps_intr, mps_bulk;
    unsigned       events, cmds, vendor_events;
    unsigned       ver_ok, mode;
    unsigned char  hw_platform, hw_variant, hw_revision, fw_variant;
    unsigned char  fw_revision, fw_build_num, fw_build_ww, fw_build_yy;
    unsigned char  fw_patch_num, pad1[3];
    unsigned       boot_ok;
    unsigned char  otp_format, otp_content, otp_patch, secure_boot;
    unsigned short dev_revid;
    unsigned char  key_from_hdr, key_type, otp_lock, api_lock, debug_lock, limited_cce;
    unsigned char  min_fw_build_nn, min_fw_build_cw, min_fw_build_yy, unlocked_state;
    unsigned char  otp_bdaddr[6];
    unsigned char  bdaddr[6];
    unsigned       local_ok;
    unsigned char  hci_version, lmp_version;
    unsigned short hci_revision, manufacturer, lmp_subversion;
    char           fw_name[40];
    unsigned       fw_found, fw_size;
    int            last_error;
    unsigned short last_opcode, pad2;
    char           state[64];
    unsigned       dl_done, dl_ok, dl_ms, dl_frags, dl_result; /* Stufe 2: Firmware laden */
    unsigned       boot_addr, booted, boot_ms;
    unsigned char  file_build_num, file_build_ww, file_build_yy, pad3;
    char           dl_msg[64];
    unsigned       hci_ready, ddc_records;                /* Stufe 3: HCI eingerichtet */
    unsigned char  features[8];
    unsigned short acl_mtu, acl_pkts, le_mtu, le_pkts;
    unsigned       scan_ms, scan_devs;
} BtInfo;
/* Gefundenes Geraet (SYS_BT 4) */
enum { BT_KIND_BREDR, BT_KIND_LE_PUBLIC, BT_KIND_LE_RANDOM };
typedef struct {
    unsigned char  addr[6];
    unsigned char  kind;
    signed char    rssi;      /* dBm, -127 = unbekannt */
    unsigned       cod;       /* Class of Device (klassisch) */
    unsigned short seen, appearance;
    unsigned char  le_flags, le_connectable, name_len, pad;
    char           name[48];
} BtDev;
static inline s64 sys_bt_info(BtInfo *bi)                     { return syscall3(SYS_BT, 0, (u64)bi, 0); }
static inline s64 sys_bt_query(void)                          { return syscall3(SYS_BT, 1, 0, 0); }
static inline s64 sys_bt_load(void)                           { return syscall3(SYS_BT, 2, 0, 0); } /* Firmware laden */
static inline s64 sys_bt_scan(u64 seconds)                    { return syscall3(SYS_BT, 3, seconds, 0); } /* -> Zahl */
static inline s64 sys_bt_dev(u64 i, BtDev *d)                 { return syscall3(SYS_BT, 4, i, (u64)d); }
/* Verbindung (Stufe 4) */
enum { BT_CONN_IDLE, BT_CONN_CONNECTING, BT_CONN_READY, BT_CONN_FAILED };
enum { BT_CSTEP_NONE, BT_CSTEP_HCI, BT_CSTEP_PAGE, BT_CSTEP_AUTH, BT_CSTEP_ENCRYPT, BT_CSTEP_L2CAP, BT_CSTEP_DISCOVER,
       BT_CSTEP_CAPS, BT_CSTEP_DONE };
typedef struct {
    unsigned char seid, in_use, media, tsep;
    unsigned char codec, caps_len, pad[2];
    unsigned char caps[8];
} BtSep;
typedef struct {
    unsigned       state, step;
    int            error;
    unsigned char  addr[6];
    unsigned short handle;
    unsigned char  conn_status, auth_status, enc_status, disc_reason;
    unsigned char  encrypted, paired_new, key_type, n_seps;
    unsigned short l2_local_cid, l2_remote_cid, l2_remote_mtu, pad;
    unsigned       acl_rx, acl_tx, l2_rx;
    BtSep          seps[8];
    char           msg[96];
    unsigned short media_remote_cid, media_mtu;      /* Stufe 5: A2DP */
    unsigned char  a2dp_seid, a2dp_bitpool, a2dp_state, pad3;
    unsigned       a2dp_packets, a2dp_dropped, a2dp_errors;
    unsigned       a2dp_underruns;           /* Luecken im Ton (Programm lieferte zu langsam) */
    unsigned       a2dp_stalls, a2dp_max_wait_ms; /* Senden dauerte ueber 30 ms; laengste Wartezeit */
} BtConn;
typedef struct {
    unsigned char addr[6];
    unsigned char type, pad;
    unsigned char key[16];
} BtKey;
static inline s64 sys_bt_connect(const unsigned char addr[6]) { return syscall3(SYS_BT, 5, (u64)addr, 0); }
static inline s64 sys_bt_disconnect(void)                     { return syscall3(SYS_BT, 6, 0, 0); }
static inline s64 sys_bt_conn(BtConn *c)                      { return syscall3(SYS_BT, 7, (u64)c, 0); }
static inline s64 sys_bt_key_get(u64 i, BtKey *k)             { return syscall3(SYS_BT, 8, i, (u64)k); }
static inline s64 sys_bt_key_add(const BtKey *k)              { return syscall3(SYS_BT, 9, (u64)k, 0); }
/* Tasten der Bluetooth-Fernbedienung (AVRCP, z.B. an der Soundbar): naechste oder BT_KEY_NONE */
enum { BT_KEY_NONE, BT_KEY_PLAY, BT_KEY_PAUSE, BT_KEY_STOP, BT_KEY_NEXT, BT_KEY_PREV };
static inline s64 sys_bt_media_key(void)                      { return syscall3(SYS_BT, 10, 0, 0); }
static inline s64 sys_read(int fd, void *buf, u64 len)        { return syscall3(SYS_READ, fd, (u64)buf, len); }
static inline s64 sys_close(int fd)                           { return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline s64 sys_closefrom(int fd)                       { return syscall3(SYS_CLOSEFROM, fd, 0, 0); } /* alle ab fd schliessen */
static inline s64 sys_readdir(const char *path, u64 index, DirEnt *ent) { return syscall3(SYS_READDIR, (u64)path, index, (u64)ent); }
static inline s64 sys_spawn(const char *path, const char *cmdline)      { return syscall3(SYS_SPAWN, (u64)path, (u64)cmdline, 0); }
static inline s64 sys_wait(int pid, int *code)                { return syscall3(SYS_WAIT, pid, (u64)code, 0); }
static inline s64 sys_wait_nohang(int pid, int *code)         { return syscall3(SYS_WAIT, pid, (u64)code, 1); } /* ERR_AGAIN: laeuft noch */
static inline s64 sys_videoinfo(u64 index, VideoInfo *vi)  { return syscall3(SYS_VIDEOINFO, index, (u64)vi, 0); }
static inline s64 sys_audio(u64 op, u64 a, u64 b)          { return syscall3(SYS_AUDIO, op, a, b); } /* siehe play.c */
/* Ausgabe waehlen (SYS_AUDIO 7/8/9): Ausgaenge der Soundkarte und ggf. Bluetooth */
#define AUDIO_OUT_AUTO (-1)
#define AUDIO_OUT_BT   100
typedef struct {
    char          name[24];
    unsigned char kind;    /* 0 Soundkarte, 1 Bluetooth */
    unsigned char plugged; /* an der Buchse steckt etwas */
    unsigned char on;      /* der Ton kommt hier heraus (bzw. kaeme) */
    unsigned char pad;
    int           id;      /* fuer sys_audio_select */
} AudioOutput;
static inline s64 sys_audio_output(u64 i, AudioOutput *o)     { return syscall3(SYS_AUDIO, 7, i, (u64)o); }
static inline s64 sys_audio_select(int id)                    { return syscall3(SYS_AUDIO, 8, (u64)(s64)id, 0); }
static inline s64 sys_audio_selected(void)                    { return syscall3(SYS_AUDIO, 9, 0, 0); }
static inline s64 sys_setmode(u64 w, u64 h, u64 hz100)     { return syscall3(SYS_SETMODE, w, h, hz100); } /* sofort umschalten (Intel-Treiber) */
/* Geteilter Speicher: anlegen -> Adresse (und Nummer), einblenden per Nummer, ausblenden, Groesse */
static inline s64 sys_shm_create(u64 bytes, unsigned *id)  { return syscall3(SYS_SHM, 0, bytes, (u64)id); }
static inline s64 sys_shm_map(unsigned id)                 { return syscall3(SYS_SHM, 1, id, 0); }
static inline s64 sys_shm_unmap(void *addr)                { return syscall3(SYS_SHM, 2, (u64)addr, 0); }
static inline s64 sys_shm_size(unsigned id)                { return syscall3(SYS_SHM, 3, id, 0); }

/* Zusammensetzen auf der GPU (Desktop): 0 Art -> 0 keins, 1 GPU, 2 CPU-Ersatz im Kernel; 1 shm-Flaeche anmelden
 * (nummer, breite | hoehe << 16) -> Nummer; 2 abmelden; 3 Auftraege ausfuehren (GpuOp *, anzahl) -> 0 wenn fertig;
 * 4 Messwert (0 CPU / 1 GPU / 2 GPU ohne Warten, mikrosekunden | pixel << 32); 5 auf die letzte Liste warten;
 * 6 wie 3, aber nur abschicken */
typedef struct {
    unsigned short kind; /* 1 kopieren, 2 mischen (Quelle mit ihrem Alpha in Byte 3 ueber das Ziel), 3 senkrecht und
                          * 4 waagerecht skalieren (w, h Vielfache von 8, alles muss in die Flaechen passen),
                          * 5 anzeigen: Rechteck der Quelle (Bildschirmbild) an dieselbe Stelle auf den Monitor */
    unsigned short dst, src;
    unsigned short pad;
    int dx, dy, sx, sy, w, h;
    int alpha;           /* mischen: Deckung 0-256 (256 = Alpha der Quelle unveraendert) */
    int step;            /* skalieren: Quellzeile/-spalte = Anfang + (i * step >> 8), 1-65535 */
} GpuOp;
/* Aufschluesselung eines Bildes des Desktops (SYS_GPUCOMP 7), alles in Mikrosekunden */
typedef struct {
    unsigned total, wait, render, ov, ov_px, shadow, alloc, queue, submit, path; /* path: 0 CPU, 1 GPU, 2 ohne Warten */
} FrameProf;
static inline s64 sys_gpucomp(u64 op, u64 a, u64 b)          { return syscall3(SYS_GPUCOMP, op, a, b); }
/* 3D zeichnen (SYS_GPUCOMP 8, nur mit Intel-GPU; sonst ERR_NOSYS - gl.c zeichnet dann selbst). Flaechen wie oben
 * angemeldet: Ziel, Tiefenpuffer (Breite Vielfaches von 32, Hoehe des Zeichenbereichs auf 32 aufgerundet), Textur
 * (Byte-Reihenfolge B, G, R, A). Erst wird auf Wunsch geloescht, dann gezeichnet: Eckpunkt mal m (Zeilen X, Y, Z, W,
 * Ergebnis in Pixeln des Zeichenbereichs vor dem Teilen durch W, Tiefe 0-1), Farbe mal Helligkeit
 * ambient + diffuse * max(0, Normale . light), dann mal Textur. Kehrt zurueck, wenn die GPU fertig ist. */
#define GPU3D_DEPTH       1 /* Tiefentest "kleiner" mit Schreiben */
#define GPU3D_LINEAR      2 /* Textur bilinear */
#define GPU3D_CLEAR_COLOR 4
#define GPU3D_CLEAR_DEPTH 8
#define GPU3D_CULL_BACK   16 /* Rueckseiten weglassen (vorn = auf dem Bildschirm gegen den Uhrzeigersinn) */
#define GPU3D_CULL_FRONT  32 /* Vorderseiten weglassen (beide: alle Dreiecke) */
#define GPU3D_BLEND       64 /* mischen: Ergebnis = Quelle * Faktor blend & 0xFF + Ziel * Faktor blend >> 8 */
#define GPU3D_NO_DEPTH_WRITE 128 /* Tiefentest ohne Schreiben (glDepthMask(GL_FALSE)) */
#define GPU3D_KEEP_ALPHA  256 /* Byte 3 im Ziel nicht schreiben (Fenster ohne Alpha-Kanal, bleibt deckend) */
/* Faktoren fuer GPU3D_BLEND (Codes der Hardware, BLENDFACTOR_*) */
#define GPU3D_BF_ONE           0x01
#define GPU3D_BF_SRC_COLOR     0x02
#define GPU3D_BF_SRC_ALPHA     0x03
#define GPU3D_BF_DST_ALPHA     0x04
#define GPU3D_BF_DST_COLOR     0x05
#define GPU3D_BF_SRC_ALPHA_SAT 0x06
#define GPU3D_BF_ZERO          0x11
#define GPU3D_BF_INV_SRC_COLOR 0x12
#define GPU3D_BF_INV_SRC_ALPHA 0x13
#define GPU3D_BF_INV_DST_ALPHA 0x14
#define GPU3D_BF_INV_DST_COLOR 0x15
#define GPU3D_MAX_VERT    1365
typedef struct {
    unsigned short dst, depth, tex, flags;      /* Flaechen (depth 0 = keiner), GPU3D_* */
    unsigned short tex_w, tex_h;                /* benutzter Teil der Textur (0 = ganze Flaeche) */
    unsigned short blend, pad1;                 /* GPU3D_BLEND: Faktor Quelle | Faktor Ziel << 8 (GPU3D_BF_*) */
    int            x, y, w, h;                  /* Zeichenbereich im Ziel, x Vielfaches von 16 */
    unsigned       clear_color;                 /* 0xAARRGGBB */
    float          clear_depth;
    float          m[16];
    float          light[3], ambient, diffuse;
    unsigned       nvert;                       /* Vielfaches von 3, hoechstens GPU3D_MAX_VERT */
    const float   *verts;                       /* je Eckpunkt x, y, z, u, v, nx, ny, nz, r, g, b, a */
} Gpu3dDraw;
static inline s64 sys_gpu3d(const Gpu3dDraw *d)               { return syscall3(SYS_GPUCOMP, 8, (u64)d, 0); }
/* 3D aus Puffern (SYS_GPUCOMP 9): Eckpunkte und Indizes liegen in angemeldeten Flaechen (geteilter Speicher, bleibt
 * ueber viele Bilder) statt im Auftrag. Je Attribut (0 Position, 1 Textur u, v, 2 Normale, 3 Farbe) ein Puffer mit
 * Anfang und Abstand je Eckpunkt oder surf = 0: fester Wert aus value. Fehlende Komponenten: Position z = 0,
 * Textur v = 0, Farbe a = 1. Die GPU liest nur innerhalb der Flaechen (dahinter 0). Kein Abschneiden an der nahen
 * Ebene - das prueft gl.c vorher (Kasten um die Eckpunkte). */
#define GPU3D_F_FLOAT1  1
#define GPU3D_F_FLOAT2  2
#define GPU3D_F_FLOAT3  3
#define GPU3D_F_FLOAT4  4
#define GPU3D_F_UBYTE4N 5               /* 4 Byte r, g, b, a: 0-255 = 0.0-1.0 */
#define GPU3D_PRIM_TRIANGLES 4
#define GPU3D_PRIM_STRIP     5
#define GPU3D_PRIM_FAN       6
typedef struct {
    unsigned short surf, format;        /* Flaeche (0 = fester Wert), GPU3D_F_* */
    unsigned       offset, stride;      /* Bytes, Vielfache von 4; stride hoechstens 2048 */
} Gpu3dAttr;
typedef struct {
    Gpu3dDraw      d;                   /* wie bei sys_gpu3d, nvert und verts unbenutzt */
    Gpu3dAttr      attr[4];
    float          value[4][4];
    unsigned short index_surf, index_size; /* 0 = ohne Indizes; sonst Bytes je Index (1, 2, 4) */
    unsigned       index_offset;
    unsigned       prim, first, count;  /* GPU3D_PRIM_*; erster Index (bzw. Eckpunkt), Anzahl */
} Gpu3dDrawVB;
static inline s64 sys_gpu3d_vb(const Gpu3dDrawVB *d)          { return syscall3(SYS_GPUCOMP, 9, (u64)d, 0); }
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
static inline s64 sys_fdwait(int fd, u64 ms)                  { return syscall3(SYS_FDAVAIL, fd, ms, 0); } /* wie sys_fdavail, wartet aber bis zu ms (max. 1000) auf Daten */
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

/* Threads (bequemer: thread.h). Nummern 0..15 je Prozess, 0 = erster Thread. Der neue Thread startet bei
 * entry(arg) mit rsp = stack_top - 8; kehrt entry zurueck, stuerzt er ab - also am Ende sys_thread_exit. */
static inline s64 sys_thread_create(void (*entry)(void *), void *stack_top, void *arg)
{
    return syscall3(SYS_THREAD_CREATE, (u64)entry, (u64)stack_top, (u64)arg); /* ERR_AGAIN: alle Plaetze belegt */
}
static inline __attribute__((noreturn)) void sys_thread_exit(u64 value) { syscall3(SYS_THREAD_EXIT, value, 0, 0); for (;;); }
static inline s64 sys_thread_join(int tid, u64 *value)        { return syscall3(SYS_THREAD_JOIN, tid, (u64)value, 0); }
static inline s64 sys_gettid(void)                            { return syscall3(SYS_GETTID, 0, 0, 0); }
/* Schlafen, solange *addr == val (0 geweckt, ERR_AGAIN Wert anders, ERR_TIMEDOUT); timeout_ms 0 = ohne Grenze */
static inline s64 sys_futex_wait(volatile u32 *addr, u32 val, u64 timeout_ms) { return syscall3(SYS_FUTEX_WAIT, (u64)addr, val, timeout_ms); }
static inline s64 sys_futex_wake(volatile u32 *addr, u32 count) { return syscall3(SYS_FUTEX_WAKE, (u64)addr, count, 0); }
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
/* Datei einblenden: liefert eine Adresse, ab der die Datei (ab offset, Vielfaches von 4096) im Speicher steht. Gelesen
 * wird erst, wenn das Programm eine Seite anfasst - auch bei grossen Dateien geht das sofort. Privat: mit writable
 * darf das Programm hineinschreiben, die Datei aendert sich dadurch nicht. Hinter dem Dateiende stehen Nullen. Der
 * Deskriptor darf danach geschlossen werden; freigeben mit sys_munmap. ERR_BADF: fd ist keine Datei. */
static inline s64 sys_mmap_file(int fd, u64 len, u64 offset, int writable)
{
    return syscall3(SYS_MMAP_FILE, (u64)(unsigned)fd | ((u64)(writable ? 1 : 0) << 32), len, offset);
}

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
