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
} WlanInfo;

void iwl_probe(void);
int  iwl_info(WlanInfo *out); /* 0 = gefuellt (auch ohne Karte: dann present = 0) */

#endif
