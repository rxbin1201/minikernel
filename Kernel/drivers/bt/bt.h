#ifndef BT_H
#define BT_H

#include <stdint.h>

/* Bluetooth ueber USB (HCI), Stufe 1: Geraet einrichten (Interrupt-IN fuer Ereignisse, Bulk-IN/-OUT fuer Daten),
 * HCI-Befehle per Control-Transfer, bei Intel (8087:xxxx) Version und Boot-Parameter lesen, Firmware-Datei finden.
 * Gleiches Layout wie BtInfo in Userland/include/user.h (SYS_BT 0). */

enum { BT_MODE_UNKNOWN, BT_MODE_BOOTLOADER, BT_MODE_OPERATIONAL };

typedef struct {
    uint32_t present;
    uint16_t vid, pid;
    char     path[16];               /* USB-Port */
    uint8_t  ep_intr, ep_bulk_in, ep_bulk_out, pad0;
    uint16_t mps_intr, mps_bulk;
    uint32_t events, cmds, vendor_events; /* empfangene Ereignisse, gesendete Befehle, davon Intel-Ereignisse (0xFF) */
    /* Intel Read Version (0xFC05, altes Format) */
    uint32_t ver_ok, mode;           /* BT_MODE_* */
    uint8_t  hw_platform, hw_variant, hw_revision, fw_variant;
    uint8_t  fw_revision, fw_build_num, fw_build_ww, fw_build_yy;
    uint8_t  fw_patch_num, pad1[3];
    /* Intel Read Boot Params (0xFC0D, nur im Bootloader) */
    uint32_t boot_ok;
    uint8_t  otp_format, otp_content, otp_patch, secure_boot;
    uint16_t dev_revid;
    uint8_t  key_from_hdr, key_type, otp_lock, api_lock, debug_lock, limited_cce;
    uint8_t  min_fw_build_nn, min_fw_build_cw, min_fw_build_yy, unlocked_state;
    uint8_t  otp_bdaddr[6];
    /* im Betrieb: HCI_Read_BD_ADDR, HCI_Read_Local_Version */
    uint8_t  bdaddr[6];
    uint32_t local_ok;
    uint8_t  hci_version, lmp_version;
    uint16_t hci_revision, manufacturer, lmp_subversion;
    /* Firmware-Datei aus /firmware */
    char     fw_name[40];
    uint32_t fw_found, fw_size;
    int32_t  last_error;             /* Status des letzten HCI-Befehls bzw. <0: keine Antwort */
    uint16_t last_opcode, pad2;
    char     state[64];
    /* Stufe 2: Firmware laden (SYS_BT 2, "bt load") */
    uint32_t dl_done, dl_ok, dl_ms, dl_frags, dl_result; /* versucht, uebertragen, Dauer, Secure-Send-Befehle,
                                                          * Ergebnis des Bootloaders (0 = ok, 0xFF = keins) */
    uint32_t boot_addr, booted, boot_ms;                 /* Boot-Adresse aus der Datei, Firmware gestartet */
    uint8_t  file_build_num, file_build_ww, file_build_yy, pad3;
    char     dl_msg[64];
    /* Stufe 3: HCI eingerichtet, Suche (SYS_BT 3) */
    uint32_t hci_ready, ddc_records;                     /* HCI_Reset usw. erledigt; DDC-Eintraege geladen */
    uint8_t  features[8];                                /* LMP-Faehigkeiten (Bit 6 von Byte 4: LE) */
    uint16_t acl_mtu, acl_pkts, le_mtu, le_pkts;          /* Puffer des Controllers */
    uint32_t scan_ms, scan_devs;
} BtInfo;

/* Gefundenes Geraet (SYS_BT 4; gleiches Layout in Userland/include/user.h) */
#define BT_MAX_DEVS 64
enum { BT_KIND_BREDR, BT_KIND_LE_PUBLIC, BT_KIND_LE_RANDOM };
typedef struct {
    uint8_t  addr[6];          /* wie geschrieben (hoechstes Byte zuerst) */
    uint8_t  kind;             /* BT_KIND_* */
    int8_t   rssi;             /* dBm, -127 = unbekannt */
    uint32_t cod;              /* Class of Device (klassisch) */
    uint16_t seen, appearance; /* so oft gemeldet; LE: Erscheinungsbild */
    uint8_t  le_flags, le_connectable, name_len, pad;
    char     name[48];
} BtDev;

/* Stufe 4: Verbindung (SYS_BT 5-9; gleiches Layout in Userland/include/user.h) */
enum { BT_CONN_IDLE, BT_CONN_CONNECTING, BT_CONN_READY, BT_CONN_FAILED };
enum { BT_CSTEP_NONE, BT_CSTEP_HCI, BT_CSTEP_PAGE, BT_CSTEP_AUTH, BT_CSTEP_ENCRYPT, BT_CSTEP_L2CAP, BT_CSTEP_DISCOVER,
       BT_CSTEP_CAPS, BT_CSTEP_DONE };
typedef struct {
    uint8_t seid, in_use, media, tsep; /* Endpunkt-Nummer, belegt, Medienart (0 Audio), 0 Quelle / 1 Senke */
    uint8_t codec, caps_len, pad[2];   /* 0 SBC, 2 AAC, 0xFF herstellereigen; Laenge der Codec-Angaben */
    uint8_t caps[8];                   /* Codec-Angaben (SBC: Frequenzen/Kanaele, Bloecke/Baender/Zuteilung, Bitpool) */
} BtSep;
typedef struct {
    uint32_t state, step;               /* BT_CONN_*, BT_CSTEP_* */
    int32_t  error;
    uint8_t  addr[6];                   /* Gegenstelle, hoechstes Byte zuerst */
    uint16_t handle;
    uint8_t  conn_status, auth_status, enc_status, disc_reason; /* HCI-Statuscodes */
    uint8_t  encrypted, paired_new, key_type, n_seps;
    uint16_t l2_local_cid, l2_remote_cid, l2_remote_mtu, pad;
    uint32_t acl_rx, acl_tx, l2_rx;
    BtSep    seps[8];
    char     msg[96];
    /* Stufe 5: A2DP */
    uint16_t media_remote_cid, media_mtu;          /* Medienkanal (Audiodaten) */
    uint8_t  a2dp_seid, a2dp_bitpool, a2dp_state, pad3; /* Endpunkt, Bitpool, 0 aus / 1 bereit / 2 spielt */
    uint32_t a2dp_packets, a2dp_dropped, a2dp_errors; /* gesendete Pakete, verworfene Samples, Sendefehler */
    uint32_t a2dp_underruns;                       /* Luecken: eine Stimme war offen, hatte aber keine Daten */
    uint32_t a2dp_stalls, a2dp_max_wait_ms;        /* Pakete, deren Senden ueber 30 ms dauerte; laengste Wartezeit */
} BtConn;
/* Verbindungsschluessel (nach dem Koppeln; das Programm bt speichert sie, damit nicht jedes Mal neu gekoppelt wird) */
typedef struct {
    uint8_t addr[6];  /* hoechstes Byte zuerst */
    uint8_t type, pad;
    uint8_t key[16];
} BtKey;

int bt_info(BtInfo *out);  /* 0; present = 0 ohne Geraet */
int bt_connect(const uint8_t addr[6]); /* verbinden, koppeln, AVDTP-Endpunkte lesen; 0 oder <0 (Grund in BtConn) */
int bt_disconnect(void);
void bt_conn_info(BtConn *out);
int bt_key_get(unsigned i, BtKey *out); /* 0 oder -1 */
int bt_key_add(const BtKey *k);

/* AVRCP: Tasten der Fernbedienung fuer Programme (SYS_BT 10); Lautstaerke wirkt direkt am Mischer */
enum { BT_KEY_NONE, BT_KEY_PLAY, BT_KEY_PAUSE, BT_KEY_STOP, BT_KEY_NEXT, BT_KEY_PREV };
int bt_media_key(void); /* naechste Taste oder BT_KEY_NONE */

/* AVRCP 1.4 "Absolute Volume": die Soundbar regelt selbst und meldet jede Aenderung (auch ueber ihre Tasten und die
 * Fernbedienung); die Gesamtlautstaerke folgt ihr, und hda_volume stellt sie dort ein. Solange das laeuft, rechnet
 * der Mischer den Ton nicht digital leiser. */
int  bt_abs_volume_active(void);
void bt_abs_volume_set(int percent); /* von hda_volume: an die Soundbar schicken (im Thread "bt") */

/* A2DP (a2dp.c): ist die Soundbar bereit, mischt hda.c in diesen Strom statt auf die Soundkarte */
int      bt_a2dp_active(void);                               /* 1 = Ton geht ueber Bluetooth */
uint32_t bt_a2dp_write(const int16_t *stereo, uint32_t frames); /* 48 kHz Stereo; Zahl angenommener Frames */
uint32_t bt_a2dp_room(void);                                 /* so viele Frames passen noch */
void     bt_a2dp_underrun(void);                             /* Mischer: Luecke im Ton (Stimme ohne Daten) */
int bt_scan(uint32_t seconds); /* Stufe 3: Firmware laden und HCI einrichten (falls noetig), suchen; Zahl der Geraete */
int bt_device(unsigned i, BtDev *out); /* 0 oder -1 */
int bt_query(void);        /* Version usw. vom Geraet holen: 0, -1 kein Geraet, -2 keine Antwort */
int bt_load_fw(void);      /* Stufe 2: Firmware laden und starten; 0 = laeuft (auch: lief schon), <0 Fehler (dl_msg) */

#endif
