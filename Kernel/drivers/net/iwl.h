#ifndef IWL_H
#define IWL_H

#include <stdint.h>

/* Intel WLAN (AX200, iwlwifi-Familie 22000 "cc"), Stufe 1: Karte finden, Register lesen, Firmware zerlegen.
 * Gleiches Layout wie WlanInfo in Userland/include/user.h (SYS_WLAN 0). */
typedef struct {
    uint32_t present;                         /* Karte gefunden */
    uint8_t  bus, dev, fn, pad;
    uint16_t vendor, device, sub_vendor, sub_device;
    uint64_t bar;
    uint32_t hw_rev, rf_id, gp_cntrl, hw_if_config; /* CSR_HW_REV, CSR_HW_RF_ID, CSR_GP_CNTRL, CSR_HW_IF_CONFIG_REG */
    uint32_t fw_found;                        /* Firmware zerlegt */
    uint32_t fw_api;                          /* API-Nummer aus dem Dateinamen bzw. Kopf (z.B. 77) */
    uint32_t fw_lmac, fw_umac, fw_paging;     /* Abschnitte je Teil */
    uint32_t fw_bytes, fw_paging_bytes;       /* Groesse aller Abschnitte, Paging-Speicher (TLV) */
    uint32_t fw_cpus, fw_capa, fw_api_flags, fw_scan_channels;
    uint32_t fw_major, fw_minor, fw_local;    /* TLV FW_VERSION */
    char     fw_name[64], fw_human[64];
    char     state[64];
    /* Stufe 2a (SYS_WLAN 1, "wlan wake"): Takt anfordern, Zugriff auf die inneren Register, eine Kennung lesen */
    uint32_t wake_done, wake_clock, wake_access; /* versucht, Takt bereit, Zugriff bekommen */
    uint32_t wake_us;                         /* bis zum Zugriff */
    uint32_t gp_after, cnvi_id;               /* GP_CNTRL danach, Kennung des CNVi (Peripherie-Register) */
    uint32_t wake_ready, hwif_after;          /* 1 = NIC_READY blieb stehen, 2 = erst nach PREPARE; HW_IF_CONFIG */
    uint32_t prph_load, prph_cpu1;            /* Peripherie-Register: UREG_UCODE_LOAD_STATUS, SB_CPU_1_STATUS */
    /* Stufe 2b (SYS_WLAN 2, "wlan load"): Firmware laden, auf die erste Nachricht warten */
    uint32_t load_done, load_alive, load_ms, load_int, load_status;
    uint32_t reset_before, reset_after;       /* CSR_RESET vor und nach dem Freigeben des inneren Prozessors */
    uint32_t wfpm_before, wfpm_after, int_after; /* WFPM_CTRL_REG vor/nach ENABLE_WFPM, CSR_INT nach den Lesezugriffen */
    uint32_t st_load, st_umac_pc, st_lmac_pc; /* zuletzt: Ladestatus, Befehlszaehler UMAC/LMAC */
    uint32_t alive_len, alive_cmd, alive_group, alive_status; /* erste Nachricht; ALIVE: Befehl 1, Status 0xCAFE */
    /* IOMMU (VT-d, ACPI-Tabelle DMAR) vor dem Start: Einheiten, davon aktiv (Uebersetzung oder geschuetzte Bereiche),
     * davon abgeschaltet; Flags der Tabelle (Bit 2: Schutz vor DMA beim Booten); LTR-Wert vor und nach dem Setzen */
    uint32_t dmar_found, dmar_flags, iommu_units, iommu_active, iommu_off;
    uint32_t iommu_gsts[4], iommu_pmen[4];
    uint32_t ltr_before, ltr_after;
    uint32_t pci_cmd, bridges_fixed;          /* PCI Kommando/Status der Karte, Bruecken mit neu gesetztem Bus-Master */
    /* Stufe 3 (nach ALIVE): 1 INIT_EXTENDED_CFG, 2 NVM_ACCESS_COMPLETE, 3 INIT_COMPLETE, 4 NVM_GET_INFO,
     * 5 TX_ANT_CONFIGURATION, 6 BT_CONFIG, 7 MCC_UPDATE, 8 SCAN_CFG, 9 fertig */
    uint32_t init_step, init_complete, rx_packets;
    uint8_t  mac[6], mac_pad[2];
    uint32_t nvm_flags, nvm_version, nvm_board, nvm_hw_addrs, nvm_sku, nvm_tx_chains, nvm_rx_chains, nvm_lar;
    uint32_t nvm_channels;
    uint32_t mcc, mcc_status, mcc_channels;   /* Land laut Firmware (z.B. 'DE'), Status, erlaubte Kanaele */
    uint32_t scan_ms, scan_frames, scan_nets; /* letzte Suche */
} WlanInfo;

/* Ein gefundenes Netz (SYS_WLAN 4; gleiches Layout in Userland/include/user.h) */
#define WLAN_MAX_NETS 64
enum { WLAN_SEC_OPEN, WLAN_SEC_WEP, WLAN_SEC_WPA, WLAN_SEC_WPA2, WLAN_SEC_WPA3, WLAN_SEC_WPA2_3 /* beides */ };
typedef struct {
    uint8_t  bssid[6];
    int8_t   signal;   /* dBm */
    uint8_t  channel;
    uint8_t  security; /* WLAN_SEC_* */
    uint8_t  ssid_len;
    uint16_t seen;     /* so oft empfangen */
    char     ssid[33];
    uint8_t  pad[3];
} WlanNet;

/* Stufe 5: verbinden (SYS_WLAN 5; gleiches Layout in Userland/include/user.h) */
typedef struct {
    char    ssid[33];
    char    pass[65];  /* WPA2: Passphrase (8..63 Zeichen) oder 64 Hex-Zeichen; offenes Netz: leer */
    uint8_t bssid[6];  /* 0: der staerkste AP mit diesem Namen */
} WlanConnect;

/* Zustand der Verbindung (SYS_WLAN 7) */
enum { WLAN_ST_IDLE, WLAN_ST_CONNECTING, WLAN_ST_CONNECTED, WLAN_ST_FAILED };
enum { WLAN_PHY_LEGACY, WLAN_PHY_HT, WLAN_PHY_VHT };
/* Schritte beim Verbinden: wie weit es kam */
enum { WLAN_STEP_NONE, WLAN_STEP_FW, WLAN_STEP_SCAN, WLAN_STEP_PMK, WLAN_STEP_CONTEXT, WLAN_STEP_STATION,
       WLAN_STEP_QUEUES, WLAN_STEP_PROTECT, WLAN_STEP_AUTH, WLAN_STEP_ASSOC, WLAN_STEP_KEYS, WLAN_STEP_DONE };
typedef struct {
    uint32_t state, step;          /* WLAN_ST_*, WLAN_STEP_* */
    int32_t  error;                /* Ergebnis des letzten Verbindens (0 = ok) */
    uint16_t status_code, reason;  /* Statuscode von Auth/Assoc, Grund einer Trennung durch den AP */
    char     ssid[33];
    uint8_t  bssid[6];
    uint8_t  channel, security;
    int8_t   signal;
    uint8_t  phy_mode;             /* WLAN_PHY_*: 802.11a/g, n (HT), ac (VHT) */
    uint8_t  width;                /* Kanalbreite in MHz (20, 40, 80) */
    uint8_t  nss;                  /* Datenstroeme (1 oder 2) */
    uint8_t  ba_rx;                /* Block-Ack beim Empfang: Bitmaske der TIDs (der AP sendet aggregiert) */
    uint8_t  tlc;                  /* 1 = die Firmware waehlt die Senderate (rate_kbps aus ihrer Meldung) */
    uint8_t  pad;
    uint32_t aid, rate_kbps, connect_ms; /* Assoziationsnummer, Senderate, Dauer des Verbindens */
    uint32_t max_kbps;             /* hoechste moegliche Rate mit diesem AP */
    uint64_t rx_frames, tx_frames, rx_dropped, tx_failed, rekeys;
    char     msg[96];              /* letztes Ereignis im Klartext */
} WlanStatus;

int  iwl_connect(const WlanConnect *c); /* wartet, bis verbunden oder gescheitert: 0, -1 keine Karte, -11 ungueltig,
                                         * -12 schon beim Verbinden, sonst <0 (Schritt und Text in WlanStatus) */
int  iwl_disconnect(void);
void iwl_status(WlanStatus *s);

void iwl_probe(void);
int  iwl_info(WlanInfo *out); /* 0 = gefuellt (auch ohne Karte: dann present = 0) */
int  iwl_load_fw(void);       /* Stufe 2b+3; 0 = Firmware bereit, -3 kein ALIVE, -4 keine Firmware, -5 kein Speicher,
                               * -6 Befehl ohne Antwort (init_step sagt, welcher) */
int  iwl_scan(void);         /* Stufe 4: Zahl der Netze, <0 Fehler (laedt die Firmware, falls noetig) */
int  iwl_scan_result(unsigned i, WlanNet *out);
int  iwl_wake_test(void);     /* Stufe 2a; 0 = Zugriff bekommen, -1 = keine Karte, -2 kein Takt, -3 kein Zugriff */

#endif
