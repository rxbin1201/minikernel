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
} WlanInfo;

void iwl_probe(void);
int  iwl_info(WlanInfo *out); /* 0 = gefuellt (auch ohne Karte: dann present = 0) */
int  iwl_load_fw(void);       /* Stufe 2b; 0 = erste Nachricht da, -3 keine, -4 keine Firmware, -5 kein Speicher */
int  iwl_wake_test(void);     /* Stufe 2a; 0 = Zugriff bekommen, -1 = keine Karte, -2 kein Takt, -3 kein Zugriff */

#endif
