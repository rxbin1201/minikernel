#ifndef IWL_INTERNAL_H
#define IWL_INTERNAL_H

/* Gemeinsames von iwl.c (Karte, Firmware, Befehle, Empfang, Suche) und iwl_sta.c (Verbinden, Datenverkehr).
 *
 * Sperren: iwl_op_lock haelt, wer die Firmware laedt, sucht, verbindet oder Schluessel einbaut (eine Operation mit
 * Befehlen und Antworten nach der anderen). iwl_ring_lock schuetzt die Ringe (Befehle, Empfang, Senden) und wird nur
 * kurz gehalten; wer auf eine Antwort wartet, gibt sie beim Schlafen frei, so kann der Netzwerk-Thread weiter
 * empfangen und senden. Reihenfolge: erst iwl_op_lock, dann iwl_ring_lock. */

#include <stdint.h>
#include "drivers/net/iwl.h"
#include "core/sched.h"

extern Mutex iwl_op_lock, iwl_ring_lock;

/* Befehlsgruppen und Befehle */
#define GRP_LEGACY         0x0
#define GRP_LONG           0x1  /* Befehle der alten Gruppe 0 schickt Linux mit Gruppe 1 (DEF_ID) */
#define GRP_SYSTEM         0x2
#define GRP_MAC_CONF       0x3
#define GRP_DATA_PATH      0x5
#define GRP_NVM            0xC
#define GRP_ANY            0xFF /* beim Warten: jede Gruppe */

#define CMD_TX             0x1C /* Senden eines Rahmens; die Antwort meldet, ob er ankam */
#define CMD_RX_MPDU        0xC1

uint32_t iwl_rd(uint32_t off);
void     iwl_wr(uint32_t off, uint32_t v);
void    *iwl_dma(uint64_t bytes);          /* zusammenhaengend, genullt, physisch = virtuell */
uint32_t iwl_le32(const uint8_t *p);
const WlanInfo *iwl_state(void);

/* Was die Suche ausser WlanNet ueber ein Netz weiss (aus Beacon bzw. Probe Response) */
typedef struct {
    uint16_t cap, beacon_int;   /* Capability Information, Beacon-Abstand in TU */
    uint8_t  dtim_period;       /* aus dem TIM-Element, sonst 1 */
    uint8_t  n_rates;
    uint8_t  rates[16];         /* Datenraten (Elemente 1 und 50) in 500 kbit/s, Bit 7 = Grundrate */
    uint8_t  rsn_len;           /* ganzes RSN-Element (Kennung, Laenge, Inhalt), 0 = keins */
    uint8_t  rsn[64];
    /* 802.11n/ac und QoS: HT Capabilities (45), HT Operation (61), VHT Capabilities (191), VHT Operation (192), WMM */
    uint8_t  has_ht, has_vht, has_wmm, ht_ampdu; /* ht_ampdu: A-MPDU-Parameter des AP (Faktor, Abstand) */
    uint16_t ht_cap;            /* HT Capabilities Info */
    uint8_t  ht_mcs[2];         /* MCS 0-7 bzw. 8-15, die der AP empfaengt */
    uint8_t  ht_primary, ht_sec, ht_wide, ht_prot; /* Hauptkanal; Zweitkanal 0 keiner, 1 darueber, 3 darunter;
                                                     * 40 MHz erlaubt; HT-Schutz (0-3) */
    uint8_t  vht_width, vht_center;                 /* VHT Operation: 0 = 20/40 MHz, 1 = 80 MHz (und mehr); Mitte */
    uint16_t vht_rx_mcs;        /* VHT: MCS je Zahl der Datenstroeme, die der AP empfaengt (je 2 Bit, 3 = keine) */
    uint32_t vht_cap;           /* VHT Capabilities Info */
    uint8_t  wmm_ac[4][4];      /* WMM-Parameter je Zugriffsklasse BE, BK, VI, VO: ACI/AIFSN, ECWmin/max, TXOP */
    uint8_t  wmm_params;        /* wmm_ac gueltig (WMM-Parameterelement, nicht nur das Informationselement) */
} IwlBss;

/* mit iwl_op_lock: Firmware (neu) laden und einrichten; 0 oder Fehler wie iwl_load_fw */
int iwl_load_fw_op(void);
/* mit iwl_op_lock: suchen (wie iwl_scan) */
int iwl_scan_op(void);
int iwl_scan_bss(unsigned i, WlanNet *n, IwlBss *x); /* 0 oder -1 */
/* Meldung erwarten, bevor der Befehl abgeht, der sie ausloest (iwl_wait_notif wartet dann darauf) */
void iwl_expect_notif(uint8_t group, uint8_t cmd);
/* mit iwl_op_lock: Befehl senden und auf die Antwort warten (hoechstens 1 s). Antwortdaten nach resp (bis max).
 * Ergebnis: Laenge der Antwort, -1 keine Antwort, -2 Fehler der Firmware, -3 Befehl abgelehnt (REPLY_ERROR) */
int iwl_cmd(uint8_t group, uint8_t cmd, const void *data, uint32_t len, const char *what, void *resp, uint32_t max);
/* mit iwl_op_lock: auf eine Meldung der Firmware warten; Laenge oder <0 wie iwl_cmd */
int iwl_wait_notif(uint8_t group, uint8_t cmd, uint32_t ms, const char *what, void *resp, uint32_t max);
/* Empfangsring auswerten (nimmt iwl_ring_lock) */
void iwl_poll(void);
/* Firmware oder Hardware hat einen Fehler gemeldet (CSR_INT) */
int iwl_fw_failed(void);

/* Faehigkeit der Firmware (Bit aus IWL_UCODE_TLV_ENABLED_CAPABILITIES) */
int iwl_fw_capa(unsigned bit);
#define CAPA_BAID_ML_SUPPORT 63 /* Block-Ack beim Empfang ueber RX_BAID_ALLOCATION_CONFIG_CMD statt ADD_STA */

/* iwl_sta.c, aufgerufen mit iwl_ring_lock aus dem Empfang */
int  iwl_sta_notif(uint8_t grp, uint8_t cmd, const uint8_t *d, uint32_t len); /* 1 = Meldung ausgewertet */
void iwl_sta_rx_mpdu(const uint8_t *d, uint32_t len);                 /* Beschreibung (48 Byte) + 802.11-Rahmen */
void iwl_sta_tx_resp(uint16_t seq, const uint8_t *d, uint32_t len);  /* Antwort auf einen gesendeten Rahmen */
int  iwl_sta_wants_rx(void);                                          /* 1 = Rahmen an iwl_sta_rx_mpdu geben */
void iwl_sta_fw_reset(void);                                          /* Firmware wird neu geladen: alles vergessen */
void iwl_sta_register(void);                                          /* beim Erkennen: wlan0 beim Netzwerk anmelden */

#endif
